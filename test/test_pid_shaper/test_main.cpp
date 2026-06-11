// PidShaper unit tests: clamping, anti-windup, per-mode gains + integrator
// reset on mode change, defrost-temper freeze, invalid-input -> zero output.
#include <unity.h>
#include <cmath>
#include "PidShaper.h"

using namespace dettson;

void setUp() {}
void tearDown() {}

static PidShaper::Config gainsOnly(float kp, float ki, float kd = 0.0f) {
  PidShaper::Config cfg;
  for (uint8_t m = 0; m < PidShaper::kMaxModes; ++m) cfg.gains[m] = {kp, ki, kd};
  return cfg;
}

static void test_boot_output_zero() {
  PidShaper pid;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, pid.output());
}

static void test_proportional_response() {
  PidShaper pid(gainsOnly(10.0f, 0.0f));
  TEST_ASSERT_EQUAL_FLOAT(10.0f, pid.update(22.0f, 21.0f, true, false, 0));
}

static void test_output_clamped_0_to_100() {
  PidShaper pid(gainsOnly(10.0f, 0.0f));
  TEST_ASSERT_EQUAL_FLOAT(100.0f, pid.update(40.0f, 20.0f, true, false, 0));  // p = 200
  TEST_ASSERT_EQUAL_FLOAT(0.0f, pid.update(20.0f, 25.0f, true, false, 10));   // p = -50
}

static void test_integral_accumulates_within_band() {
  PidShaper pid(gainsOnly(0.0f, 0.1f));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, pid.update(25.0f, 20.0f, true, false, 0));    // first call: dt = 0
  TEST_ASSERT_EQUAL_FLOAT(50.0f, pid.update(25.0f, 20.0f, true, false, 100)); // 0.1 * 5 * 100
}

static void test_anti_windup_no_growth_while_saturated() {
  PidShaper pid(gainsOnly(10.0f, 0.1f));
  pid.update(25.0f, 20.0f, true, false, 0);     // p = 50, integ = 0
  pid.update(25.0f, 20.0f, true, false, 100);   // integ -> 50, out = 100 (just saturated)
  // Hours of saturated error: the integral must stop accumulating.
  pid.update(25.0f, 20.0f, true, false, 4000);
  TEST_ASSERT_EQUAL_FLOAT(100.0f, pid.output());
  // Error reverses: output must leave saturation immediately, not after
  // unwinding a runaway integral.
  float out = pid.update(25.0f, 26.0f, true, false, 4010);  // p = -10
  TEST_ASSERT_TRUE(out < 50.0f);
}

static void test_anti_windup_unwinding_always_allowed() {
  PidShaper pid(gainsOnly(10.0f, 0.1f));
  pid.update(25.0f, 20.0f, true, false, 0);
  pid.update(25.0f, 20.0f, true, false, 100);   // saturated high, integ = 50
  // While still saturated, negative error must be allowed to shrink the integral.
  pid.update(25.0f, 26.0f, true, false, 110);   // integ 50 -> 49, out = 39
  TEST_ASSERT_EQUAL_FLOAT(39.0f, pid.output());
  pid.update(25.0f, 26.0f, true, false, 120);   // keeps unwinding in-band
  TEST_ASSERT_EQUAL_FLOAT(38.0f, pid.output());
}

static void test_integrator_reset_on_mode_change() {
  PidShaper pid(gainsOnly(0.0f, 0.1f));
  pid.update(25.0f, 20.0f, true, false, 0);
  pid.update(25.0f, 20.0f, true, false, 100);   // integ = 50
  TEST_ASSERT_EQUAL_FLOAT(50.0f, pid.output());
  pid.selectMode(1);                            // docs/05: reset on mode change
  TEST_ASSERT_EQUAL_UINT8(1, pid.mode());
  // Same conditions, but integral and dt history are gone: output is P-only = 0.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, pid.update(25.0f, 20.0f, true, false, 200));
}

static void test_reselecting_same_mode_keeps_state() {
  PidShaper pid(gainsOnly(0.0f, 0.1f));
  pid.update(25.0f, 20.0f, true, false, 0);
  pid.update(25.0f, 20.0f, true, false, 100);
  pid.selectMode(0);  // no change -> no reset
  TEST_ASSERT_EQUAL_FLOAT(100.0f, pid.update(25.0f, 20.0f, true, false, 200));
}

static void test_per_mode_gain_sets() {
  PidShaper pid(gainsOnly(10.0f, 0.0f));
  pid.setGains(1, {20.0f, 0.0f, 0.0f});
  TEST_ASSERT_EQUAL_FLOAT(10.0f, pid.update(22.0f, 21.0f, true, false, 0));
  pid.selectMode(1);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, pid.update(22.0f, 21.0f, true, false, 10));
}

static void test_freeze_holds_output_and_stops_integration() {
  PidShaper pid(gainsOnly(10.0f, 0.1f));
  pid.update(22.0f, 21.0f, true, false, 0);  // out = 10
  // Defrost tempering: room temp dips hard, but the PID must hold, not wind up.
  TEST_ASSERT_EQUAL_FLOAT(10.0f, pid.update(22.0f, 15.0f, true, true, 100));
  TEST_ASSERT_EQUAL_FLOAT(10.0f, pid.update(22.0f, 15.0f, true, true, 800));
  // Unfreeze with the dip recovered: only the small post-freeze dt integrates —
  // a wound-up integral would have added 0.1 * 7 * 800 = 560.
  float out = pid.update(22.0f, 21.0f, true, false, 810);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 11.0f, out);  // p = 10 + integ = 0.1*1*10
}

static void test_invalid_input_forces_zero_and_clean_recovery() {
  PidShaper pid(gainsOnly(10.0f, 0.1f));
  pid.update(25.0f, 20.0f, true, false, 0);
  pid.update(25.0f, 20.0f, true, false, 100);  // saturated, integ = 50
  TEST_ASSERT_EQUAL_FLOAT(0.0f, pid.update(25.0f, 20.0f, false, false, 200));  // flag set -> no output
  TEST_ASSERT_EQUAL_FLOAT(0.0f, pid.output());
  // Recovery starts clean: P-only first step, no stale integral, no dt kick.
  TEST_ASSERT_EQUAL_FLOAT(10.0f, pid.update(22.0f, 21.0f, true, false, 5000));
}

static void test_nan_inputs_force_zero() {
  PidShaper pid(gainsOnly(10.0f, 0.1f));
  pid.update(22.0f, 21.0f, true, false, 0);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, pid.update(NAN, 21.0f, true, false, 10));
  pid.update(22.0f, 21.0f, true, false, 20);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, pid.update(22.0f, NAN, true, false, 30));
}

static void test_derivative_on_measurement() {
  PidShaper pid(gainsOnly(0.0f, 0.0f, 60.0f));
  pid.update(22.0f, 20.0f, true, false, 0);                            // seed history
  TEST_ASSERT_EQUAL_FLOAT(0.0f, pid.update(22.0f, 21.0f, true, false, 60));   // rising temp -> -1, clamps to 0
  TEST_ASSERT_EQUAL_FLOAT(1.0f, pid.update(22.0f, 20.0f, true, false, 120));  // falling temp -> +1
}

static void test_out_of_range_mode_index_is_clamped() {
  PidShaper pid(gainsOnly(10.0f, 0.0f));
  pid.setGains(200, {20.0f, 0.0f, 0.0f});  // lands on the last slot, no overflow
  pid.selectMode(200);
  TEST_ASSERT_EQUAL_UINT8(PidShaper::kMaxModes - 1, pid.mode());
  TEST_ASSERT_EQUAL_FLOAT(20.0f, pid.update(22.0f, 21.0f, true, false, 0));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_boot_output_zero);
  RUN_TEST(test_proportional_response);
  RUN_TEST(test_output_clamped_0_to_100);
  RUN_TEST(test_integral_accumulates_within_band);
  RUN_TEST(test_anti_windup_no_growth_while_saturated);
  RUN_TEST(test_anti_windup_unwinding_always_allowed);
  RUN_TEST(test_integrator_reset_on_mode_change);
  RUN_TEST(test_reselecting_same_mode_keeps_state);
  RUN_TEST(test_per_mode_gain_sets);
  RUN_TEST(test_freeze_holds_output_and_stops_integration);
  RUN_TEST(test_invalid_input_forces_zero_and_clean_recovery);
  RUN_TEST(test_nan_inputs_force_zero);
  RUN_TEST(test_derivative_on_measurement);
  RUN_TEST(test_out_of_range_mode_index_is_clamped);
  return UNITY_END();
}
