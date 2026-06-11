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
  return UNITY_END();
}
