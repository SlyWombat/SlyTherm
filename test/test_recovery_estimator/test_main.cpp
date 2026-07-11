// Unit tests for lib/RecoveryEstimator — smart recovery advisor (issue #50):
// seeded behavior before data, learning convergence, segment gates + robust
// outlier rejection, lookahead bounding, disabled-by-default, and the
// advisory-only contract (advice is just numbers — no demand authority).
#include <unity.h>

#include <cmath>
#include <initializer_list>

#include "DettsonConfig.h"
#include "RecoveryEstimator.h"

using namespace dettson;

void setUp() {}
void tearDown() {}

static RecoveryEstimator enabledEstimator() {
  RecoveryConfig cfg;
  cfg.enabled = true;
  return RecoveryEstimator(cfg);
}

// Feed n clean segments at the given rate (C/h) into one channel.
static void feedSegments(RecoveryEstimator& re, RecoveryMode m,
                         RecoveryEquipment e, float rateCPerH, int n,
                         uint32_t& t) {
  const uint32_t durS = 3600;
  const float dir = (m == RecoveryMode::kHeat) ? 1.0f : -1.0f;
  for (int i = 0; i < n; ++i) {
    re.startSegment(m, e, 20.0f, t);
    re.endSegment(20.0f + dir * rateCPerH, t + durS);
    t += durS + 600;
  }
}

// ---------- disabled by default ----------

static void test_disabled_by_default_gives_no_advice() {
  TEST_ASSERT_FALSE(kRecoveryEnabledDefault);
  RecoveryEstimator re;  // default config -> disabled
  TEST_ASSERT_FALSE(re.enabled());

  RecoveryTarget t;
  t.setpointC = 22.0f;
  t.mode = RecoveryMode::kHeat;
  t.inS = 3600;
  RecoveryAdvice a = re.advise(t, 18.0f, RecoveryEquipment::kGas);
  TEST_ASSERT_EQUAL_UINT32(0, a.startEarlyByS);
  TEST_ASSERT_FALSE(a.startNow);

  re.setEnabled(true);
  TEST_ASSERT_TRUE(re.enabled());
  TEST_ASSERT_TRUE(re.advise(t, 18.0f, RecoveryEquipment::kGas).startEarlyByS > 0);
  re.setEnabled(false);
  TEST_ASSERT_EQUAL_UINT32(0,
      re.advise(t, 18.0f, RecoveryEquipment::kGas).startEarlyByS);
}

static void test_learning_continues_while_disabled() {
  RecoveryEstimator re;  // disabled
  uint32_t t = 1000;
  feedSegments(re, RecoveryMode::kHeat, RecoveryEquipment::kGas, 2.0f, 5, t);
  TEST_ASSERT_EQUAL_UINT32(5,
      re.samples(RecoveryMode::kHeat, RecoveryEquipment::kGas));
  TEST_ASSERT_TRUE(re.rateCPerH(RecoveryMode::kHeat, RecoveryEquipment::kGas) >
                   kRecoverySeedHeatCPerH);
}

// ---------- seeded behavior before data ----------

static void test_seeded_rates_before_any_data() {
  RecoveryEstimator re = enabledEstimator();
  for (RecoveryEquipment e : {RecoveryEquipment::kHp, RecoveryEquipment::kGas}) {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, kRecoverySeedHeatCPerH,
                             re.rateCPerH(RecoveryMode::kHeat, e));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, kRecoverySeedCoolCPerH,
                             re.rateCPerH(RecoveryMode::kCool, e));
    TEST_ASSERT_EQUAL_UINT32(0, re.samples(RecoveryMode::kHeat, e));
    TEST_ASSERT_EQUAL_UINT32(0, re.samples(RecoveryMode::kCool, e));
  }

  // 1.0 C heat deficit at the 1.0 C/h seed -> 3600 s lead.
  RecoveryTarget t;
  t.setpointC = 21.0f;
  t.mode = RecoveryMode::kHeat;
  t.inS = 5400;
  RecoveryAdvice a = re.advise(t, 20.0f, RecoveryEquipment::kHp);
  TEST_ASSERT_EQUAL_UINT32(3600, a.startEarlyByS);
  TEST_ASSERT_FALSE(a.startNow);  // 3600 < 5400 remaining

  // 1.0 C cool deficit at the 0.8 C/h seed -> 4500 s lead; with only
  // 1800 s remaining the advice is "start now".
  t.setpointC = 24.0f;
  t.mode = RecoveryMode::kCool;
  t.inS = 1800;
  a = re.advise(t, 25.0f, RecoveryEquipment::kHp);
  TEST_ASSERT_EQUAL_UINT32(4500, a.startEarlyByS);
  TEST_ASSERT_TRUE(a.startNow);
}

static void test_no_advice_when_already_at_or_past_target() {
  RecoveryEstimator re = enabledEstimator();
  RecoveryTarget t;
  t.setpointC = 20.0f;
  t.mode = RecoveryMode::kHeat;
  t.inS = 3600;
  TEST_ASSERT_EQUAL_UINT32(0,
      re.advise(t, 21.0f, RecoveryEquipment::kGas).startEarlyByS);  // warmer
  TEST_ASSERT_EQUAL_UINT32(0,
      re.advise(t, 20.0f, RecoveryEquipment::kGas).startEarlyByS);  // exactly at
  t.mode = RecoveryMode::kCool;
  TEST_ASSERT_EQUAL_UINT32(0,
      re.advise(t, 19.0f, RecoveryEquipment::kHp).startEarlyByS);   // cooler
  // NaN inputs -> no advice, never NaN propagation.
  t.mode = RecoveryMode::kHeat;
  TEST_ASSERT_EQUAL_UINT32(0,
      re.advise(t, std::nanf(""), RecoveryEquipment::kGas).startEarlyByS);
}

// ---------- lookahead bounding ----------

static void test_lookahead_bounded() {
  RecoveryEstimator re = enabledEstimator();
  RecoveryTarget t;
  t.setpointC = 30.0f;  // 12 C deficit at 1.0 C/h -> 43200 s unbounded
  t.mode = RecoveryMode::kHeat;
  t.inS = 86400;
  RecoveryAdvice a = re.advise(t, 18.0f, RecoveryEquipment::kGas);
  TEST_ASSERT_EQUAL_UINT32(kRecoveryMaxLookaheadS, a.startEarlyByS);
  TEST_ASSERT_FALSE(a.startNow);  // capped lead < 86400 remaining
  t.inS = 600;
  TEST_ASSERT_TRUE(re.advise(t, 18.0f, RecoveryEquipment::kGas).startNow);
}

// ---------- learning convergence ----------

static void test_learning_converges_per_channel() {
  RecoveryEstimator re = enabledEstimator();
  uint32_t t = 1000;
  feedSegments(re, RecoveryMode::kHeat, RecoveryEquipment::kGas, 2.0f, 15, t);
  TEST_ASSERT_EQUAL_UINT32(15,
      re.samples(RecoveryMode::kHeat, RecoveryEquipment::kGas));
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 2.0f,
      re.rateCPerH(RecoveryMode::kHeat, RecoveryEquipment::kGas));

  // Channel isolation: heat/hp and both cool channels stay seeded.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, kRecoverySeedHeatCPerH,
      re.rateCPerH(RecoveryMode::kHeat, RecoveryEquipment::kHp));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, kRecoverySeedCoolCPerH,
      re.rateCPerH(RecoveryMode::kCool, RecoveryEquipment::kGas));
  TEST_ASSERT_EQUAL_UINT32(0,
      re.samples(RecoveryMode::kCool, RecoveryEquipment::kHp));

  // The learned rate drives the advice: 1.0 C deficit at ~2.0 C/h -> ~1800 s.
  RecoveryTarget tgt;
  tgt.setpointC = 21.0f;
  tgt.mode = RecoveryMode::kHeat;
  tgt.inS = 7200;
  RecoveryAdvice a = re.advise(tgt, 20.0f, RecoveryEquipment::kGas);
  TEST_ASSERT_UINT32_WITHIN(60, 1800, a.startEarlyByS);

  // Cool channel learns independently (falling temps).
  feedSegments(re, RecoveryMode::kCool, RecoveryEquipment::kHp, 1.6f, 15, t);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.6f,
      re.rateCPerH(RecoveryMode::kCool, RecoveryEquipment::kHp));
}

// ---------- segment gates + outlier rejection ----------

static void test_segment_gates_reject_short_small_and_wrong_direction() {
  RecoveryEstimator re = enabledEstimator();
  const float seed = re.rateCPerH(RecoveryMode::kHeat, RecoveryEquipment::kGas);

  // No open segment.
  TEST_ASSERT_FALSE(re.endSegment(21.0f, 5000));

  // Too short: 899 s < kRecoveryMinSegmentS.
  re.startSegment(RecoveryMode::kHeat, RecoveryEquipment::kGas, 20.0f, 1000);
  TEST_ASSERT_FALSE(re.endSegment(21.0f, 1000 + kRecoveryMinSegmentS - 1));

  // Too little movement: 0.1 C < kRecoveryMinSegmentDeltaC.
  re.startSegment(RecoveryMode::kHeat, RecoveryEquipment::kGas, 20.0f, 10000);
  TEST_ASSERT_FALSE(re.endSegment(20.1f, 10000 + 3600));

  // Wrong direction: a heat segment that lost ground.
  re.startSegment(RecoveryMode::kHeat, RecoveryEquipment::kGas, 20.0f, 20000);
  TEST_ASSERT_FALSE(re.endSegment(19.0f, 20000 + 3600));

  // Implausible rate: 5 C in 900 s = 20 C/h > kRecoveryRateMaxCPerH.
  re.startSegment(RecoveryMode::kHeat, RecoveryEquipment::kGas, 20.0f, 30000);
  TEST_ASSERT_FALSE(re.endSegment(25.0f, 30000 + 900));

  // Nothing leaked into the estimate.
  TEST_ASSERT_EQUAL_UINT32(0,
      re.samples(RecoveryMode::kHeat, RecoveryEquipment::kGas));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, seed,
      re.rateCPerH(RecoveryMode::kHeat, RecoveryEquipment::kGas));

  // A consumed segment cannot be ended twice.
  re.startSegment(RecoveryMode::kHeat, RecoveryEquipment::kGas, 20.0f, 40000);
  TEST_ASSERT_TRUE(re.endSegment(22.0f, 40000 + 3600));
  TEST_ASSERT_FALSE(re.endSegment(23.0f, 40000 + 7200));
}

static void test_ratio_outlier_rejected_after_min_samples() {
  RecoveryEstimator re = enabledEstimator();
  uint32_t t = 1000;
  feedSegments(re, RecoveryMode::kHeat, RecoveryEquipment::kGas, 2.0f,
               kRecoveryOutlierMinSamples, t);
  const float est = re.rateCPerH(RecoveryMode::kHeat, RecoveryEquipment::kGas);
  const uint32_t n = re.samples(RecoveryMode::kHeat, RecoveryEquipment::kGas);

  // 9 C/h is > 3x the ~1.66 estimate -> rejected even though it is inside
  // the absolute plausibility band.
  re.startSegment(RecoveryMode::kHeat, RecoveryEquipment::kGas, 20.0f, t);
  TEST_ASSERT_FALSE(re.endSegment(29.0f, t + 3600));
  // 0.3 C/h is < estimate/3 -> rejected low-side too.
  re.startSegment(RecoveryMode::kHeat, RecoveryEquipment::kGas, 20.0f, t);
  TEST_ASSERT_FALSE(re.endSegment(20.3f, t + 3600));

  TEST_ASSERT_EQUAL_UINT32(n,
      re.samples(RecoveryMode::kHeat, RecoveryEquipment::kGas));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, est,
      re.rateCPerH(RecoveryMode::kHeat, RecoveryEquipment::kGas));
}

static void test_before_min_samples_plausible_rates_accepted() {
  // The ratio test only arms after kRecoveryOutlierMinSamples — early on, a
  // genuinely fast system (5x the seed) must still be able to teach us.
  RecoveryEstimator re = enabledEstimator();
  re.startSegment(RecoveryMode::kHeat, RecoveryEquipment::kGas, 20.0f, 1000);
  TEST_ASSERT_TRUE(re.endSegment(25.0f, 1000 + 3600));  // 5.0 C/h
  TEST_ASSERT_EQUAL_UINT32(1,
      re.samples(RecoveryMode::kHeat, RecoveryEquipment::kGas));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f + kRecoveryEmaAlpha * 4.0f,
      re.rateCPerH(RecoveryMode::kHeat, RecoveryEquipment::kGas));
}

static void test_restarted_segment_discards_the_open_one() {
  RecoveryEstimator re = enabledEstimator();
  re.startSegment(RecoveryMode::kHeat, RecoveryEquipment::kGas, 20.0f, 1000);
  // Equipment restarted: new anchor wins; old (interrupted) one is dropped.
  re.startSegment(RecoveryMode::kHeat, RecoveryEquipment::kGas, 20.5f, 2000);
  TEST_ASSERT_TRUE(re.endSegment(21.5f, 2000 + 1800));  // 1.0 C / 0.5 h = 2.0
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f + kRecoveryEmaAlpha * 1.0f,
      re.rateCPerH(RecoveryMode::kHeat, RecoveryEquipment::kGas));

  // NaN anchors never open (or close) a segment.
  re.startSegment(RecoveryMode::kHeat, RecoveryEquipment::kGas,
                  std::nanf(""), 9000);
  TEST_ASSERT_FALSE(re.endSegment(22.0f, 9000 + 3600));
  re.startSegment(RecoveryMode::kHeat, RecoveryEquipment::kGas, 20.0f, 20000);
  TEST_ASSERT_FALSE(re.endSegment(std::nanf(""), 20000 + 3600));
}

// ---------- #141 steady-state crossing prediction (docs/13 §2) ----------

static void test_crossing_bias_predicts_inside_horizon_only() {
  // 0.1 C to go at 0.6 C/h -> crossing in 600 s: inside the 1200 s horizon,
  // bias at the linear midpoint.
  CrossingBias cb = RecoveryEstimator::crossingBias(0.1f, 0.6f, 1200, 0.10f);
  TEST_ASSERT_TRUE(cb.predicted);
  TEST_ASSERT_UINT32_WITHIN(1, 600, cb.inS);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.05f, cb.biasC);

  // Same margin at a slower approach -> outside the horizon: no prediction.
  cb = RecoveryEstimator::crossingBias(0.1f, 0.25f, 1200, 0.10f);
  TEST_ASSERT_FALSE(cb.predicted);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, cb.biasC);

  // Crossing imminent -> bias approaches the cap (and never exceeds it).
  cb = RecoveryEstimator::crossingBias(0.001f, 0.9f, 1200, 0.10f);
  TEST_ASSERT_TRUE(cb.predicted);
  TEST_ASSERT_TRUE(cb.biasC > 0.09f && cb.biasC <= 0.10f);
}

static void test_crossing_bias_needs_a_real_approach() {
  // Receding or flat: no crossing ahead, whatever the margin.
  TEST_ASSERT_FALSE(RecoveryEstimator::crossingBias(0.1f, 0.0f, 1200, 0.1f).predicted);
  TEST_ASSERT_FALSE(RecoveryEstimator::crossingBias(0.1f, -0.6f, 1200, 0.1f).predicted);
  // Already at/past the crossing: the call machinery owns violations.
  TEST_ASSERT_FALSE(RecoveryEstimator::crossingBias(0.0f, 0.6f, 1200, 0.1f).predicted);
  TEST_ASSERT_FALSE(RecoveryEstimator::crossingBias(-0.2f, 0.6f, 1200, 0.1f).predicted);
  // Degenerate parameters and NaN inputs -> no prediction, never NaN out.
  TEST_ASSERT_FALSE(RecoveryEstimator::crossingBias(0.1f, 0.6f, 0, 0.1f).predicted);
  TEST_ASSERT_FALSE(RecoveryEstimator::crossingBias(0.1f, 0.6f, 1200, 0.0f).predicted);
  TEST_ASSERT_FALSE(
      RecoveryEstimator::crossingBias(std::nanf(""), 0.6f, 1200, 0.1f).predicted);
  const CrossingBias cb =
      RecoveryEstimator::crossingBias(0.1f, std::nanf(""), 1200, 0.1f);
  TEST_ASSERT_FALSE(cb.predicted);
  TEST_ASSERT_FALSE(std::isnan(cb.biasC));
}

static void test_crossing_bias_ramps_linearly_toward_the_crossing() {
  // Fixed approach rate, shrinking margin: bias must increase monotonically.
  float last = -1.0f;
  for (float toGo = 0.19f; toGo > 0.005f; toGo -= 0.02f) {
    const CrossingBias cb =
        RecoveryEstimator::crossingBias(toGo, 0.6f, 1200, 0.10f);
    TEST_ASSERT_TRUE(cb.predicted);
    TEST_ASSERT_TRUE(cb.biasC > last);
    TEST_ASSERT_TRUE(cb.biasC <= 0.10f);
    last = cb.biasC;
  }
}

// ---------- #141 two-ramp recovery (heat fallback ramp) ----------

static RecoveryEstimator twoRampEstimator() {
  RecoveryConfig cfg;
  cfg.enabled = true;
  cfg.twoRampEnabled = true;
  return RecoveryEstimator(cfg);
}

static void test_two_ramp_disabled_by_default_and_gated() {
  TEST_ASSERT_FALSE(kRecoveryTwoRampEnabledDefault);  // WINTER task (#141)
  RecoveryTarget t;
  t.setpointC = 21.0f;
  t.mode = RecoveryMode::kHeat;
  t.inS = 3600;

  RecoveryEstimator off;  // both gates off
  TEST_ASSERT_FALSE(off.adviseRamps(t, 15.0f).fallbackValid);

  RecoveryConfig cfg;  // recovery on, two-ramp still off -> pre-#141 behavior
  cfg.enabled = true;
  RecoveryEstimator re(cfg);
  const RecoveryRamps r = re.adviseRamps(t, 18.0f);
  TEST_ASSERT_FALSE(r.fallbackValid);
  TEST_ASSERT_FALSE(r.gasAdvised);
  // ...while the hp arm still mirrors advise() exactly.
  const RecoveryAdvice a = re.advise(t, 18.0f, RecoveryEquipment::kHp);
  TEST_ASSERT_EQUAL_UINT32(a.startEarlyByS, r.hp.startEarlyByS);
  TEST_ASSERT_EQUAL(a.startNow, r.hp.startNow);
}

static void test_fallback_ramp_line_math() {
  RecoveryEstimator re = twoRampEstimator();
  // Seeded gas rate is kRecoverySeedHeatCPerH (1.0), derated by the 0.85
  // margin -> 0.85 C/h effective. The line ends AT the setpoint...
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f, re.fallbackTempAt(21.0f, 0));
  // ...and one hour out sits 0.85 C below it.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f - kRecoveryFallbackMargin,
                           re.fallbackTempAt(21.0f, 3600));
  // Learned gas rate steepens the line: teach ~2 C/h and re-check.
  uint32_t t = 1000;
  feedSegments(re, RecoveryMode::kHeat, RecoveryEquipment::kGas, 2.0f, 15, t);
  const float rate =
      re.rateCPerH(RecoveryMode::kHeat, RecoveryEquipment::kGas);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f - rate * kRecoveryFallbackMargin,
                           re.fallbackTempAt(21.0f, 3600));
}

static void test_fallback_gas_advised_exactly_below_the_line() {
  RecoveryEstimator re = twoRampEstimator();
  RecoveryTarget t;
  t.setpointC = 21.0f;
  t.mode = RecoveryMode::kHeat;
  t.inS = 3600;  // line now = 21.0 - 0.85 = 20.15
  const float lineNow = re.fallbackTempAt(t.setpointC, t.inS);

  RecoveryRamps r = re.adviseRamps(t, lineNow + 0.1f);  // above: HP holds it
  TEST_ASSERT_TRUE(r.fallbackValid);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, lineNow, r.fallbackTempNowC);
  TEST_ASSERT_FALSE(r.gasAdvised);

  r = re.adviseRamps(t, lineNow - 0.1f);  // below: even gas would miss it
  TEST_ASSERT_TRUE(r.gasAdvised);

  // The verdict tightens as target time nears (the line rises toward the
  // setpoint): the same room temp flips from fine to gas-advised.
  t.inS = 900;  // line now = 21.0 - 0.2125 = 20.79
  r = re.adviseRamps(t, 20.5f);
  TEST_ASSERT_TRUE(r.gasAdvised);
  t.inS = 7200;  // line now = 19.3: plenty of runway again
  r = re.adviseRamps(t, 20.5f);
  TEST_ASSERT_FALSE(r.gasAdvised);
}

static void test_deep_cold_advises_gas_from_the_recovery_start() {
  // docs/13 §2 "one rule": when HP-alone cannot make the target even from
  // the capped early start, the room already sits below the fallback line at
  // the recovery start — gas blends in from the first minute.
  RecoveryEstimator re = twoRampEstimator();
  RecoveryTarget t;
  t.setpointC = 21.0f;
  t.mode = RecoveryMode::kHeat;
  // 5 C deficit needs 5 h at the 1.0 C/h HP seed; the lead caps at 7200 s.
  RecoveryRamps r = re.adviseRamps(t, 16.0f);
  TEST_ASSERT_EQUAL_UINT32(kRecoveryMaxLookaheadS, r.hp.startEarlyByS);
  t.inS = r.hp.startEarlyByS;  // evaluate AT the early start
  r = re.adviseRamps(t, 16.0f);
  // Line at 2 h out = 21 - 0.85*2 = 19.3 > 16.0 -> gas advised immediately.
  TEST_ASSERT_TRUE(r.fallbackValid);
  TEST_ASSERT_TRUE(r.gasAdvised);
}

static void test_two_ramp_cool_targets_have_no_fallback() {
  RecoveryEstimator re = twoRampEstimator();
  RecoveryTarget t;
  t.setpointC = 24.0f;
  t.mode = RecoveryMode::kCool;  // single fuel: nothing to fall back to
  t.inS = 3600;
  const RecoveryRamps r = re.adviseRamps(t, 27.0f);
  TEST_ASSERT_FALSE(r.fallbackValid);
  TEST_ASSERT_FALSE(r.gasAdvised);
  TEST_ASSERT_TRUE(r.hp.startEarlyByS > 0);  // hp arm still advises

  // NaN inputs: hp arm already guards; the fallback must too.
  t.mode = RecoveryMode::kHeat;
  TEST_ASSERT_FALSE(re.adviseRamps(t, std::nanf("")).fallbackValid);
  t.setpointC = std::nanf("");
  TEST_ASSERT_FALSE(re.adviseRamps(t, 20.0f).fallbackValid);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_disabled_by_default_gives_no_advice);
  RUN_TEST(test_learning_continues_while_disabled);
  RUN_TEST(test_seeded_rates_before_any_data);
  RUN_TEST(test_no_advice_when_already_at_or_past_target);
  RUN_TEST(test_lookahead_bounded);
  RUN_TEST(test_learning_converges_per_channel);
  RUN_TEST(test_segment_gates_reject_short_small_and_wrong_direction);
  RUN_TEST(test_ratio_outlier_rejected_after_min_samples);
  RUN_TEST(test_before_min_samples_plausible_rates_accepted);
  RUN_TEST(test_restarted_segment_discards_the_open_one);
  RUN_TEST(test_crossing_bias_predicts_inside_horizon_only);
  RUN_TEST(test_crossing_bias_needs_a_real_approach);
  RUN_TEST(test_crossing_bias_ramps_linearly_toward_the_crossing);
  RUN_TEST(test_two_ramp_disabled_by_default_and_gated);
  RUN_TEST(test_fallback_ramp_line_math);
  RUN_TEST(test_fallback_gas_advised_exactly_below_the_line);
  RUN_TEST(test_deep_cold_advises_gas_from_the_recovery_start);
  RUN_TEST(test_two_ramp_cool_targets_have_no_fallback);
  return UNITY_END();
}
