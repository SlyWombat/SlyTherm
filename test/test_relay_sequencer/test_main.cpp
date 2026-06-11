// RelaySequencer unit tests: boot/goSilent all-off, OB-only-when-idle
// (incl. attempted heat->cool flip under load), forced G with Y, Y2 gating,
// transition spacing, defrost passthrough (issue #35; docs/04 §2 relay rows).
#include <unity.h>
#include "RelaySequencer.h"
#include "DettsonConfig.h"

using namespace dettson;

void setUp() {}
void tearDown() {}

namespace {

struct FakeGate : CompressorIdleGate {
  bool idle = true;
  bool compressorIdle(uint32_t) override { return idle; }
};

constexpr uint32_t kGapMs = kRelayMinTransitionMs;  // 500

RelaySequencer::Inputs coolCall(float pct = 100.0f) {
  RelaySequencer::Inputs in;
  in.demand.coolPct = pct;
  return in;
}

RelaySequencer::Inputs hpHeatCall(float pct = 100.0f) {
  RelaySequencer::Inputs in;
  in.demand.hpHeatPct = pct;
  return in;
}

// Drives updates with widely spaced timestamps until outputs settle (or the
// step budget runs out) so sequencing tests aren't entangled with spacing.
const RelayOutputs& settle(RelaySequencer& s, const RelaySequencer::Inputs& in,
                           uint32_t& nowS, uint32_t& nowMs, int steps = 6) {
  for (int i = 0; i < steps; ++i) {
    nowS += 10;
    nowMs += 10000;
    s.update(in, nowS, nowMs);
  }
  return s.outputs();
}

void assertAllOff(const RelayOutputs& o) {
  TEST_ASSERT_FALSE(o.y1);
  TEST_ASSERT_FALSE(o.y2);
  TEST_ASSERT_FALSE(o.ob);
  TEST_ASSERT_FALSE(o.g);
  TEST_ASSERT_FALSE(o.requiresBlowerProof);
}

}  // namespace

// ------------------------------------------------------------ boot / goSilent

static void test_boot_all_off() {
  FakeGate gate;
  RelaySequencer s(gate);
  assertAllOff(s.outputs());
  TEST_ASSERT_FALSE(s.outputs().defrostActive);
}

static void test_go_silent_all_off_immediately_mid_call() {
  FakeGate gate;
  RelaySequencer s(gate);
  uint32_t nowS = 0, nowMs = 0;
  RelaySequencer::Inputs in = coolCall();
  in.stage2 = true;
  settle(s, in, nowS, nowMs);
  TEST_ASSERT_TRUE(s.outputs().y1);  // mid-call
  s.goSilent();                       // no time argument: unconditional
  assertAllOff(s.outputs());
}

// ----------------------------------------------------------- O/B only at idle

static void test_ob_prepositions_to_heat_when_idle_and_gate_open() {
  FakeGate gate;
  RelaySequencer s(gate);
  RelaySequencer::Inputs in;
  in.obPreposition = ObPreposition::kHeat;
  const RelayOutputs& o = s.update(in, 0, 0);
  TEST_ASSERT_TRUE(o.ob);   // energized = heating (kObEnergizedIsHeat)
  TEST_ASSERT_FALSE(o.y1);  // pre-position never raises a call
  TEST_ASSERT_FALSE(o.g);
}

static void test_ob_flip_blocked_until_gate_grants() {
  FakeGate gate;
  gate.idle = false;  // min-off guard unserved
  RelaySequencer s(gate);
  uint32_t nowS = 0, nowMs = 0;
  const RelayOutputs& o = settle(s, hpHeatCall(), nowS, nowMs);
  TEST_ASSERT_FALSE(o.ob);  // boot OB = cool position; flip denied
  TEST_ASSERT_FALSE(o.y1);  // Y must NOT close with the valve in the wrong position
  gate.idle = true;
  settle(s, hpHeatCall(), nowS, nowMs);
  TEST_ASSERT_TRUE(s.outputs().ob);
  TEST_ASSERT_TRUE(s.outputs().y1);
  TEST_ASSERT_TRUE(s.outputs().g);
}

static void test_ob_flips_before_y_closes_never_same_update() {
  FakeGate gate;
  RelaySequencer s(gate);
  // Step manually: the update that flips OB must not also close Y.
  const RelayOutputs& o1 = s.update(hpHeatCall(), 0, 0);
  TEST_ASSERT_TRUE(o1.ob);
  TEST_ASSERT_FALSE(o1.y1);
  const RelayOutputs& o2 = s.update(hpHeatCall(), 10, 10000);
  TEST_ASSERT_TRUE(o2.ob);
  TEST_ASSERT_TRUE(o2.y1);
}

static void test_attempted_heat_to_cool_flip_under_load() {
  FakeGate gate;
  RelaySequencer s(gate);
  uint32_t nowS = 0, nowMs = 0;
  settle(s, hpHeatCall(), nowS, nowMs);
  TEST_ASSERT_TRUE(s.outputs().y1);
  TEST_ASSERT_TRUE(s.outputs().ob);

  // Compressor now running -> the guard is unserved again.
  gate.idle = false;
  // Upstream (wrongly, or after a too-short dwell) demands cool immediately.
  nowS += 10;
  nowMs += 10000;
  const RelayOutputs& o1 = s.update(coolCall(), nowS, nowMs);
  TEST_ASSERT_FALSE(o1.y1);  // Y drops first...
  TEST_ASSERT_TRUE(o1.ob);   // ...but OB does NOT flip under load
  const RelayOutputs& o2 = settle(s, coolCall(), nowS, nowMs);
  TEST_ASSERT_TRUE(o2.ob);   // still heating position while guard unserved
  TEST_ASSERT_FALSE(o2.y1);

  gate.idle = true;  // min-off served
  const RelayOutputs& o3 = settle(s, coolCall(), nowS, nowMs);
  TEST_ASSERT_FALSE(o3.ob);  // flipped to cool at idle
  TEST_ASSERT_TRUE(o3.y1);   // then the cool call closes Y
}

static void test_conflicting_demands_drop_y() {
  FakeGate gate;
  RelaySequencer s(gate);
  RelaySequencer::Inputs in;  // DemandArbiter makes this unreachable; defensive
  in.demand.hpHeatPct = 100.0f;
  in.demand.coolPct = 100.0f;
  uint32_t nowS = 0, nowMs = 0;
  const RelayOutputs& o = settle(s, in, nowS, nowMs);
  TEST_ASSERT_FALSE(o.y1);
  TEST_ASSERT_FALSE(o.y2);
}

// --------------------------------------------------------------- G and blower

static void test_g_forced_on_with_y_even_without_fan_demand() {
  FakeGate gate;
  RelaySequencer s(gate);
  uint32_t nowS = 0, nowMs = 0;
  const RelayOutputs& o = settle(s, coolCall(), nowS, nowMs);
  TEST_ASSERT_TRUE(o.y1);
  TEST_ASSERT_TRUE(o.g);                    // docs/04 coil-freeze row
  TEST_ASSERT_TRUE(o.requiresBlowerProof);  // sense-side interlock cue
}

static void test_g_follows_fan_demand_alone() {
  FakeGate gate;
  RelaySequencer s(gate);
  RelaySequencer::Inputs in;
  in.demand.fanPct = 100.0f;
  const RelayOutputs& o = s.update(in, 0, 0);
  TEST_ASSERT_TRUE(o.g);
  TEST_ASSERT_FALSE(o.y1);
  TEST_ASSERT_FALSE(o.requiresBlowerProof);  // no Y, no proof needed
}

// ------------------------------------------------------------------ Y2 gating

static void test_y2_only_with_y1() {
  FakeGate gate;
  RelaySequencer s(gate);
  uint32_t nowS = 0, nowMs = 0;

  RelaySequencer::Inputs in;  // stage2 with no Y demand: nothing closes
  in.stage2 = true;
  settle(s, in, nowS, nowMs);
  TEST_ASSERT_FALSE(s.outputs().y2);
  TEST_ASSERT_FALSE(s.outputs().y1);

  in = coolCall();
  in.stage2 = true;
  settle(s, in, nowS, nowMs);
  TEST_ASSERT_TRUE(s.outputs().y1);
  TEST_ASSERT_TRUE(s.outputs().y2);

  in.stage2 = false;  // stage-down keeps Y1
  settle(s, in, nowS, nowMs);
  TEST_ASSERT_TRUE(s.outputs().y1);
  TEST_ASSERT_FALSE(s.outputs().y2);

  in.demand.coolPct = 0.0f;  // call ends: both open
  in.stage2 = true;
  settle(s, in, nowS, nowMs);
  TEST_ASSERT_FALSE(s.outputs().y1);
  TEST_ASSERT_FALSE(s.outputs().y2);
}

// ---------------------------------------------------------- transition spacing

static void test_transition_spacing_defers_close_changes() {
  FakeGate gate;
  RelaySequencer s(gate);
  // OB pre-positioned at boot time 0 (first transition, allowed).
  RelaySequencer::Inputs in;
  in.obPreposition = ObPreposition::kCool;
  in.demand.fanPct = 100.0f;
  const RelayOutputs& o1 = s.update(in, 0, 0);
  TEST_ASSERT_TRUE(o1.g);

  in.demand.fanPct = 0.0f;  // change requested inside the spacing window
  const RelayOutputs& o2 = s.update(in, 0, kGapMs - 1);
  TEST_ASSERT_TRUE(o2.g);   // deferred: previous contact state held

  const RelayOutputs& o3 = s.update(in, 1, kGapMs);  // window served
  TEST_ASSERT_FALSE(o3.g);

  // And the next change is spaced from THIS transition, not the first one.
  in.demand.fanPct = 100.0f;
  TEST_ASSERT_FALSE(s.update(in, 1, kGapMs + 100).g);
  TEST_ASSERT_TRUE(s.update(in, 2, 2 * kGapMs).g);
}

static void test_flags_track_even_while_transition_deferred() {
  FakeGate gate;
  RelaySequencer s(gate);
  RelaySequencer::Inputs in;
  in.demand.fanPct = 100.0f;
  s.update(in, 0, 0);
  in.defrostSense = true;
  in.demand.fanPct = 0.0f;          // contact change deferred...
  const RelayOutputs& o = s.update(in, 0, 100);
  TEST_ASSERT_TRUE(o.g);
  TEST_ASSERT_TRUE(o.defrostActive);  // ...but the sense flag passes through
}

// -------------------------------------------------------------------- defrost

static void test_defrost_flag_passthrough_does_not_alter_outputs() {
  FakeGate gate;
  RelaySequencer s(gate);
  uint32_t nowS = 0, nowMs = 0;
  RelaySequencer::Inputs in = hpHeatCall();
  settle(s, in, nowS, nowMs);
  const RelayOutputs before = s.outputs();

  in.defrostSense = true;  // ODU enters defrost; we hold steady
  nowS += 10;
  nowMs += 10000;
  const RelayOutputs& o = s.update(in, nowS, nowMs);
  TEST_ASSERT_TRUE(o.defrostActive);
  TEST_ASSERT_EQUAL(before.y1, o.y1);
  TEST_ASSERT_EQUAL(before.ob, o.ob);
  TEST_ASSERT_EQUAL(before.g, o.g);

  in.defrostSense = false;
  nowS += 10;
  nowMs += 10000;
  TEST_ASSERT_FALSE(s.update(in, nowS, nowMs).defrostActive);
}

// ------------------------------------------------------------------- defaults

static void test_config_constants() {
  TEST_ASSERT_EQUAL_UINT32(500, kRelayMinTransitionMs);
  TEST_ASSERT_TRUE(kObEnergizedIsHeat);  // Gree B convention (docs/03 §7)
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_boot_all_off);
  RUN_TEST(test_go_silent_all_off_immediately_mid_call);
  RUN_TEST(test_ob_prepositions_to_heat_when_idle_and_gate_open);
  RUN_TEST(test_ob_flip_blocked_until_gate_grants);
  RUN_TEST(test_ob_flips_before_y_closes_never_same_update);
  RUN_TEST(test_attempted_heat_to_cool_flip_under_load);
  RUN_TEST(test_conflicting_demands_drop_y);
  RUN_TEST(test_g_forced_on_with_y_even_without_fan_demand);
  RUN_TEST(test_g_follows_fan_demand_alone);
  RUN_TEST(test_y2_only_with_y1);
  RUN_TEST(test_transition_spacing_defers_close_changes);
  RUN_TEST(test_flags_track_even_while_transition_deferred);
  RUN_TEST(test_defrost_flag_passthrough_does_not_alter_outputs);
  RUN_TEST(test_config_constants);
  return UNITY_END();
}
