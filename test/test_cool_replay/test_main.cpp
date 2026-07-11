// Cooling-chain replay against the recorded 2026-07-09/10 field trace
// (issue #140). Feeds the fused-temperature trace captured during the 24 h
// shadow-vs-OEM run through the REAL control chain — ModeStateMachine ->
// error->duty map -> StagedCoolShaper, gated by a real CompressorGuard —
// and asserts the cycle structure the issue's acceptance demands. The OLD
// chain (stagedRequestPct -> HpRelayShaper, the pre-#140 glue) runs on the
// same trace for the before/after comparison.
//
// Open-loop by design: the trace does not respond to our demands. That is
// exactly how the shadow scoreboard itself works — it measures DECISION
// STRUCTURE (cycle count, run/rest lengths, starts hygiene), not outcomes.
//
// Baseline being beaten (kdocker2:/data/slylog/reports/shadow-vs-oem-20260709.md):
//   OEM:        18 cool cycles / 24 h, runs 14.5-29.3 min at 30 %
//   old shaper: 26 cycles, runs 8-19 min at 100 %, a 5 h on/off toggling stretch
// Acceptance (issue #140): cycles <= 18 per 24 h equivalent, min run >= 12 min,
// starts/hour never > 3.
//
// Fixture: tools/fixtures/shadow_cool_20260709.csv (epoch_s,fused_temp_c,
// set_cool_c; 60 s cadence; exported from the slylog shadow_demands table).
// Stretches of fused_temp_c == 0 are real sensor-invalid dropouts — they are
// replayed as invalid input (supervisor safety stop), matching firmware.

#include <unity.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "CompressorGuard.h"
#include "DemandShaper.h"
#include "DettsonConfig.h"
#include "ModeStateMachine.h"

using namespace dettson;

void setUp() {}
void tearDown() {}

// ------------------------------------------------------------------ fixture

struct Row {
  uint32_t epochS;
  float tempC;
  float coolSpC;
};

static std::vector<Row> gRows;

static bool loadFixture() {
  if (!gRows.empty()) return true;
  const char* candidates[] = {
      "tools/fixtures/shadow_cool_20260709.csv",
      "../tools/fixtures/shadow_cool_20260709.csv",
      "../../tools/fixtures/shadow_cool_20260709.csv",
  };
  FILE* f = nullptr;
  if (const char* env = std::getenv("SLYTHERM_FIXTURE")) f = std::fopen(env, "r");
  for (size_t i = 0; !f && i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
    f = std::fopen(candidates[i], "r");
  }
  if (!f) return false;
  char line[128];
  while (std::fgets(line, sizeof(line), f)) {
    Row r{};
    unsigned long e = 0;
    if (std::sscanf(line, "%lu,%f,%f", &e, &r.tempC, &r.coolSpC) == 3) {
      r.epochS = static_cast<uint32_t>(e);
      gRows.push_back(r);
    }
  }
  std::fclose(f);
  return gRows.size() > 1000;  // ~1430 rows expected; a stub file is a failure
}

// ------------------------------------------------------------- chain glue

// Firmware GuardGate replica (src/main_thermostat.cpp): the shaper's
// transitions consult the real CompressorGuard.
struct GuardGate : public CompressorGate {
  CompressorGuard& g;
  explicit GuardGate(CompressorGuard& guard) : g(guard) {}
  bool canStart(uint32_t nowS) override { return g.requestStart(nowS).allowed; }
  bool canStop(uint32_t nowS) override {
    return g.requestStop(nowS, /*safety=*/false).allowed;
  }
};

// Pre-#140 glue knobs (src/thermostat_config.h): the staged-HP request map
// the cool path used to ride. Reproduced verbatim for the before-metrics.
static float oldStagedRequestPct(float errC) {
  constexpr float kHpDutyMinPct = 40.0f;
  constexpr float kHpFullScaleErrC = 2.0f;
  if (errC <= 0.0f) return kHpDutyMinPct;
  const float frac = errC >= kHpFullScaleErrC ? 1.0f : errC / kHpFullScaleErrC;
  return kHpDutyMinPct + (100.0f - kHpDutyMinPct) * frac;
}

// ---------------------------------------------------------------- metrics

struct Metrics {
  int    cycles     = 0;
  double minRunMin  = 0;  // closed, policy-ended runs only (see safetyCut/open)
  double meanRunMin = 0;
  double maxRunMin  = 0;
  double minOffMin  = 0;  // rests BETWEEN runs (leading idle excluded)
  int    maxStartsHr = 0;
  double onHours    = 0;
  int    safetyCut  = 0;  // runs ended by a sensor-invalid safety stop
  bool   openEnded  = false;  // last run still on at end of trace
};

class MetricsCollector {
 public:
  void sample(uint32_t nowS, bool on, bool safetyStop) {
    if (on && !wasOn_) {
      starts_.push_back(nowS);
      if (haveOffStart_) offLens_.push_back(nowS - offStartS_);
      runStartS_ = nowS;
    } else if (!on && wasOn_) {
      runLens_.push_back(nowS - runStartS_);
      runSafety_.push_back(safetyStop);
      offStartS_ = nowS;
      haveOffStart_ = true;
    }
    if (wasOn_) onS_ += nowS - lastS_;
    wasOn_ = on;
    lastS_ = nowS;
  }

  Metrics finish(uint32_t endS) {
    Metrics m;
    if (wasOn_) {  // censored trailing run: counts as a cycle, not toward min
      runLens_.push_back(endS - runStartS_);
      runSafety_.push_back(false);
      m.openEnded = true;
    }
    m.cycles = static_cast<int>(starts_.size());
    double sum = 0, mn = 1e18, mx = 0;
    int nClosed = 0;
    for (size_t i = 0; i < runLens_.size(); ++i) {
      sum += runLens_[i];
      if (runLens_[i] > mx) mx = runLens_[i];
      if (runSafety_[i]) { ++m.safetyCut; continue; }        // truncated by safety
      if (m.openEnded && i + 1 == runLens_.size()) continue;  // censored
      if (runLens_[i] < mn) mn = runLens_[i];
      ++nClosed;
    }
    m.minRunMin = nClosed ? mn / 60.0 : 0;
    m.maxRunMin = mx / 60.0;
    m.meanRunMin = runLens_.empty() ? 0 : sum / runLens_.size() / 60.0;
    mn = 1e18;
    for (uint32_t o : offLens_) if (o < mn) mn = o;
    m.minOffMin = offLens_.empty() ? 0 : mn / 60.0;
    for (size_t i = 0; i < starts_.size(); ++i) {
      int n = 0;
      for (size_t j = 0; j <= i; ++j) {
        if (starts_[i] - starts_[j] < 3600) ++n;
      }
      if (n > m.maxStartsHr) m.maxStartsHr = n;
    }
    m.onHours = onS_ / 3600.0;
    return m;
  }

 private:
  bool wasOn_ = false;
  bool haveOffStart_ = false;
  uint32_t runStartS_ = 0, offStartS_ = 0, lastS_ = 0;
  double onS_ = 0;
  std::vector<uint32_t> starts_, runLens_, offLens_;
  std::vector<bool> runSafety_;
};

// ----------------------------------------------------------------- replay

// One control tick per trace row (60 s cadence, the shadow scoreboard's own
// resolution). Mirrors the firmware loop: sensor validity -> supervisor
// safety stop; ModeStateMachine call; cooling lockouts; request -> shaper.
template <typename ShaperT, typename RequestFn>
static Metrics replay(ShaperT& shaper, CompressorGuard& guard, RequestFn requestPct) {
  ModeStateMachine sm;
  const uint32_t t0 = gRows.front().epochS;
  guard.bootRestore(nullptr, t0, /*abnormalReset=*/false);  // unknown -> full hold-off
  sm.setHeatSetpoint(16.5f);  // field values during the shadow run
  sm.setCoolSetpoint(gRows.front().coolSpC);
  sm.setMode(UserMode::kCool, t0);

  MetricsCollector mc;
  float lastSp = gRows.front().coolSpC;
  for (const Row& r : gRows) {
    const uint32_t nowS = r.epochS;
    if (r.coolSpC != lastSp) { sm.setCoolSetpoint(r.coolSpC); lastSp = r.coolSpC; }
    // Fused validity: the logger writes 0 during fusion dropouts; the range
    // gate is 5-40 C (docs/05). Invalid -> supervisor demand-drop, which is
    // a SAFETY stop in firmware (guard stop + shaper reset), never comfort.
    const bool valid = std::isfinite(r.tempC) && r.tempC >= kSensorRangeMinC &&
                       r.tempC <= kSensorRangeMaxC;
    bool safetyStop = false;
    Call call{};
    if (!valid) {
      if (guard.running()) guard.requestStop(nowS, /*safety=*/true);
      shaper.reset();
      safetyStop = true;
      sm.update(r.tempC, false, nowS);
    } else {
      call = sm.update(r.tempC, true, nowS);
    }
    float req = 0.0f;
    if (valid && call.type == CallType::kCool &&
        r.tempC >= kCoolingIndoorLockoutC) {  // cooling indoor lockout (docs/05)
      req = requestPct(call.errorC);
    }
    const float out = safetyStop ? 0.0f : shaper.shape(req, nowS);
    mc.sample(nowS, out > 0.0f, safetyStop);
  }
  return mc.finish(gRows.back().epochS);
}

static Metrics runNewChain() {
  CompressorGuard guard;
  GuardGate gate(guard);
  StagedCoolShaper shaper(gate);
  return replay(shaper, guard,
                [&shaper](float errC) { return shaper.requestFromError(errC); });
}

static Metrics runOldChain() {
  CompressorGuard guard;
  GuardGate gate(guard);
  HpRelayShaper shaper(gate);  // pre-#140: cool rode gHpShaper at 0/100
  return replay(shaper, guard, [](float errC) { return oldStagedRequestPct(errC); });
}

static double windowHours() {
  return (gRows.back().epochS - gRows.front().epochS) / 3600.0;
}

static void printMetrics(const char* name, const Metrics& m, double hrs) {
  std::printf(
      "%-9s cycles=%d (%.1f/24h)  run min/mean/max=%.1f/%.1f/%.1f min  "
      "off min=%.1f min  max starts/h=%d  on=%.2f h  safety-cut=%d%s\n",
      name, m.cycles, m.cycles * 24.0 / hrs, m.minRunMin, m.meanRunMin,
      m.maxRunMin, m.minOffMin, m.maxStartsHr, m.onHours, m.safetyCut,
      m.openEnded ? "  (trailing run open)" : "");
}

// ------------------------------------------------------------------- tests

static void test_fixture_loads() {
  TEST_ASSERT_TRUE_MESSAGE(loadFixture(),
                           "fixture tools/fixtures/shadow_cool_20260709.csv not found "
                           "(run pio test from the repo root or set SLYTHERM_FIXTURE)");
  TEST_ASSERT_TRUE(windowHours() > 20.0);  // a truncated export would flatter the metrics
}

static void test_new_chain_meets_cycle_acceptance() {
  TEST_ASSERT_TRUE(loadFixture());
  const double hrs = windowHours();
  const Metrics oldM = runOldChain();
  const Metrics newM = runNewChain();
  std::printf("\n--- #140 cool replay, %.1f h field trace (OEM baseline: 18 cycles, "
              "runs 14.5-29.3 min) ---\n", hrs);
  printMetrics("old chain", oldM, hrs);
  printMetrics("new chain", newM, hrs);

  // Acceptance (issue #140, replayed offline).
  char msg[128];
  std::snprintf(msg, sizeof(msg), "cycles/24h = %.1f (limit 18)", newM.cycles * 24.0 / hrs);
  TEST_ASSERT_TRUE_MESSAGE(newM.cycles * 24.0 / hrs <= 18.0, msg);
  std::snprintf(msg, sizeof(msg), "min run = %.1f min (limit 12)", newM.minRunMin);
  TEST_ASSERT_TRUE_MESSAGE(newM.minRunMin >= 12.0, msg);
  std::snprintf(msg, sizeof(msg), "max starts/h = %d (limit 3)", newM.maxStartsHr);
  TEST_ASSERT_TRUE_MESSAGE(newM.maxStartsHr <= 3, msg);
  // Hygiene beyond the letter of the acceptance: rests never shorter than
  // the demand-level min-off.
  std::snprintf(msg, sizeof(msg), "min off = %.1f min (limit 8)", newM.minOffMin);
  TEST_ASSERT_TRUE_MESSAGE(newM.minOffMin >= 8.0, msg);
}

static void test_new_chain_beats_old_chain_cycle_count() {
  TEST_ASSERT_TRUE(loadFixture());
  const Metrics oldM = runOldChain();
  const Metrics newM = runNewChain();
  TEST_ASSERT_TRUE_MESSAGE(newM.cycles < oldM.cycles,
                           "new chain must cycle less than the old chain");
  // The old chain reproduces the live shadow's pathology on this trace
  // (26 cycles observed in the field): if this stops holding, the harness
  // no longer models the firmware and the before/after story is void.
  TEST_ASSERT_TRUE_MESSAGE(oldM.cycles >= 20, "old chain lost its bang-bang? harness drift");
  TEST_ASSERT_TRUE_MESSAGE(oldM.maxStartsHr >= newM.maxStartsHr,
                           "new chain must not start more often per hour");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fixture_loads);
  RUN_TEST(test_new_chain_meets_cycle_acceptance);
  RUN_TEST(test_new_chain_beats_old_chain_cycle_count);
  return UNITY_END();
}
