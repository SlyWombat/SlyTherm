// Unit tests for lib/SensorFusion TrendEstimator — the EMA'd fused-temp
// slope feeding crossing prediction (issue #141, docs/13 §2): warm-up
// gating, convergence, dropout robustness (skip + bridge), long-gap reset,
// per-sample spike clamping, and clock-misbehavior conventions.
#include <unity.h>

#include <cmath>

#include "DettsonConfig.h"
#include "TrendEstimator.h"

using namespace dettson;

void setUp() {}
void tearDown() {}

// Feed a steady ramp at rateCPerH with the given cadence for durS seconds,
// starting at (t0, temp0). Returns the time after the last sample.
static uint32_t feedRamp(TrendEstimator& te, float temp0, float rateCPerH,
                         uint32_t t0, uint32_t durS, uint32_t stepS = 60) {
  uint32_t t = t0;
  for (; t <= t0 + durS; t += stepS) {
    te.update(temp0 + rateCPerH * static_cast<float>(t - t0) / 3600.0f,
              true, t);
  }
  return t;
}

// ---------- warm-up gating ----------

static void test_invalid_until_warmup_then_valid() {
  TrendEstimator te;
  TEST_ASSERT_FALSE(te.valid());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, te.slopeCPerH());  // 0 while invalid

  // One sample anchors but proves nothing.
  te.update(21.0f, true, 1000);
  TEST_ASSERT_FALSE(te.valid());

  // Span accumulates only across bridged valid pairs.
  uint32_t t = 1000;
  while (t < 1000 + kTrendWarmupS - 60) {
    t += 60;
    te.update(21.0f, true, t);
    TEST_ASSERT_FALSE(te.valid());
  }
  te.update(21.0f, true, t + 60);
  TEST_ASSERT_TRUE(te.valid());  // warmupS of history now observed
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, te.slopeCPerH());  // flat room
}

// ---------- convergence ----------

static void test_converges_to_steady_slope() {
  TrendEstimator te;
  // 40 min of +0.6 C/h at 60 s cadence: EMA (tau 600 s) has fully settled.
  feedRamp(te, 21.0f, 0.6f, 1000, 2400);
  TEST_ASSERT_TRUE(te.valid());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.6f, te.slopeCPerH());

  // Direction change tracks with ~tau lag: 20 min of -0.9 C/h lands near it.
  feedRamp(te, 21.4f, -0.9f, 10000, 1200);
  TEST_ASSERT_FLOAT_WITHIN(0.25f, -0.9f, te.slopeCPerH());
}

// ---------- dropout robustness ----------

static void test_invalid_samples_skipped_and_bridged() {
  TrendEstimator te;
  uint32_t t = feedRamp(te, 21.0f, 0.6f, 1000, 1800);
  const float before = te.slopeCPerH();
  TEST_ASSERT_TRUE(te.valid());

  // A 5 min fusion dropout (the fixture's fused_temp=0 stretches replay as
  // valid=false): the garbage values must not perturb the trend...
  for (int i = 0; i < 5; ++i) { te.update(0.0f, false, t); t += 60; }
  TEST_ASSERT_TRUE(te.valid());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, before, te.slopeCPerH());

  // ...and the next valid sample bridges the gap: still on the same ramp, so
  // the slope estimate is undisturbed (gap well inside kTrendMaxGapS).
  te.update(21.0f + 0.6f * static_cast<float>(t - 1000) / 3600.0f, true, t);
  TEST_ASSERT_TRUE(te.valid());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.6f, te.slopeCPerH());

  // NaN is treated as invalid regardless of the flag — never propagates.
  te.update(std::nanf(""), true, t + 60);
  TEST_ASSERT_FALSE(std::isnan(te.slopeCPerH()));
}

static void test_long_gap_resets_the_trend() {
  TrendEstimator te;
  uint32_t t = feedRamp(te, 21.0f, 0.6f, 1000, 1800);
  TEST_ASSERT_TRUE(te.valid());

  // Blind past kTrendMaxGapS: the room may have moved arbitrarily — the old
  // trend is discarded and warm-up starts over from the new anchor.
  t += kTrendMaxGapS + 60;
  te.update(22.0f, true, t);
  TEST_ASSERT_FALSE(te.valid());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, te.slopeCPerH());

  // Fresh history re-validates after warm-up and re-converges (EMA from 0
  // needs a few tau; 40 min is plenty).
  feedRamp(te, 22.0f, 0.3f, t, 2400);
  TEST_ASSERT_TRUE(te.valid());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.3f, te.slopeCPerH());
}

// ---------- spike clamp + clock conventions ----------

static void test_spike_clamped_to_plausibility_band() {
  TrendEstimator te;
  uint32_t t = feedRamp(te, 21.0f, 0.0f, 1000, 1800);  // settled flat
  // A 2 C step in one 60 s sample is 120 C/h raw; the clamp caps its EMA
  // contribution at kTrendMaxSlopeCPerH so one glitch cannot swamp the trend.
  te.update(23.0f, true, t);
  const float w = 60.0f / (60.0f + static_cast<float>(kTrendTauS));
  TEST_ASSERT_TRUE(te.slopeCPerH() <= kTrendMaxSlopeCPerH * w + 0.001f);
}

static void test_non_monotonic_and_duplicate_time_ignored() {
  TrendEstimator te;
  feedRamp(te, 21.0f, 0.6f, 1000, 1800);
  const float before = te.slopeCPerH();
  te.update(25.0f, true, 2800);  // same tick / clock step backward:
  te.update(25.0f, true, 500);   //  no interval -> no learning, no NaN/inf
  TEST_ASSERT_FLOAT_WITHIN(0.001f, before, te.slopeCPerH());
  TEST_ASSERT_TRUE(std::isfinite(te.slopeCPerH()));
}

static void test_reset_clears_state() {
  TrendEstimator te;
  feedRamp(te, 21.0f, 0.6f, 1000, 1800);
  TEST_ASSERT_TRUE(te.valid());
  te.reset();
  TEST_ASSERT_FALSE(te.valid());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, te.slopeCPerH());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_invalid_until_warmup_then_valid);
  RUN_TEST(test_converges_to_steady_slope);
  RUN_TEST(test_invalid_samples_skipped_and_bridged);
  RUN_TEST(test_long_gap_resets_the_trend);
  RUN_TEST(test_spike_clamped_to_plausibility_band);
  RUN_TEST(test_non_monotonic_and_duplicate_time_ignored);
  RUN_TEST(test_reset_clears_state);
  return UNITY_END();
}
