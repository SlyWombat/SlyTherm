// Unit tests for lib/FanCirculate — the fan "circulate" duty helpers (issue
// #128): minutes/speed clamp+snap and the per-second duty decision, including
// the #53/#142 pre-circulation credit ledger the driver reads at runtime.
#include <unity.h>

#include "FanCirculate.h"

using namespace dettson;

void setUp() {}
void tearDown() {}

// ---- clampCirculateMinPerHour: [0, 60] ----
void test_clamp_min_in_range() {
  TEST_ASSERT_EQUAL_UINT32(15u, fan::clampCirculateMinPerHour(15));
  TEST_ASSERT_EQUAL_UINT32(0u, fan::clampCirculateMinPerHour(0));
  TEST_ASSERT_EQUAL_UINT32(60u, fan::clampCirculateMinPerHour(60));
}
void test_clamp_min_below_floor() {
  TEST_ASSERT_EQUAL_UINT32(0u, fan::clampCirculateMinPerHour(-5));
}
void test_clamp_min_above_ceiling() {
  TEST_ASSERT_EQUAL_UINT32(60u, fan::clampCirculateMinPerHour(120));
  TEST_ASSERT_EQUAL_UINT32(60u, fan::clampCirculateMinPerHour(61));
}

// ---- snapCirculatePct: nearest of Low/Med/High ----
void test_snap_pct_exact() {
  TEST_ASSERT_EQUAL_FLOAT(25.0f, fan::snapCirculatePct(25.0f));
  TEST_ASSERT_EQUAL_FLOAT(50.0f, fan::snapCirculatePct(50.0f));
  TEST_ASSERT_EQUAL_FLOAT(75.0f, fan::snapCirculatePct(75.0f));
}
void test_snap_pct_nearest() {
  TEST_ASSERT_EQUAL_FLOAT(25.0f, fan::snapCirculatePct(30.0f));   // closer to Low
  TEST_ASSERT_EQUAL_FLOAT(50.0f, fan::snapCirculatePct(40.0f));   // closer to Med
  TEST_ASSERT_EQUAL_FLOAT(75.0f, fan::snapCirculatePct(70.0f));   // closer to High
  TEST_ASSERT_EQUAL_FLOAT(75.0f, fan::snapCirculatePct(100.0f));  // above -> High
  TEST_ASSERT_EQUAL_FLOAT(25.0f, fan::snapCirculatePct(0.0f));    // below -> Low
}

// ---- circulateRequestPct: window at the top of the hour ----
void test_duty_inside_window_runs() {
  // 15 min/hr, no credit: seconds 0..899 request the speed.
  TEST_ASSERT_EQUAL_FLOAT(25.0f, fan::circulateRequestPct(0, 15, 25.0f, 0));
  TEST_ASSERT_EQUAL_FLOAT(25.0f, fan::circulateRequestPct(899, 15, 25.0f, 0));
  // second 3600 wraps to the top of the next hour.
  TEST_ASSERT_EQUAL_FLOAT(25.0f, fan::circulateRequestPct(3600, 15, 25.0f, 0));
}
void test_duty_outside_window_idle() {
  TEST_ASSERT_EQUAL_FLOAT(0.0f, fan::circulateRequestPct(900, 15, 25.0f, 0));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, fan::circulateRequestPct(3599, 15, 25.0f, 0));
}
void test_duty_reads_runtime_speed() {
  // The driver runs at whatever runtime speed config says (Med/High), proving
  // it reads the runtime value, not the compile-time Low default.
  TEST_ASSERT_EQUAL_FLOAT(50.0f, fan::circulateRequestPct(10, 15, 50.0f, 0));
  TEST_ASSERT_EQUAL_FLOAT(75.0f, fan::circulateRequestPct(10, 15, 75.0f, 0));
}
void test_duty_reads_runtime_minutes() {
  // 5 min/hr: window is 0..299; 20 min/hr: window is 0..1199.
  TEST_ASSERT_EQUAL_FLOAT(25.0f, fan::circulateRequestPct(299, 5, 25.0f, 0));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, fan::circulateRequestPct(300, 5, 25.0f, 0));
  TEST_ASSERT_EQUAL_FLOAT(25.0f, fan::circulateRequestPct(1000, 20, 25.0f, 0));
}
void test_duty_precirc_credit_shrinks_window() {
  // 15 min/hr = 900 s window; 600 s already spent pre-circulating this hour ->
  // effective window is 300 s (#53/#142: credit counts toward duty).
  TEST_ASSERT_EQUAL_FLOAT(25.0f, fan::circulateRequestPct(299, 15, 25.0f, 600));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, fan::circulateRequestPct(300, 15, 25.0f, 600));
}
void test_duty_credit_exceeds_window_idle() {
  // Pre-run already covered the whole duty: never runs on top of it.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, fan::circulateRequestPct(0, 15, 25.0f, 900));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, fan::circulateRequestPct(0, 15, 25.0f, 5000));
}
void test_duty_zero_minutes_never_runs() {
  TEST_ASSERT_EQUAL_FLOAT(0.0f, fan::circulateRequestPct(0, 0, 25.0f, 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_clamp_min_in_range);
  RUN_TEST(test_clamp_min_below_floor);
  RUN_TEST(test_clamp_min_above_ceiling);
  RUN_TEST(test_snap_pct_exact);
  RUN_TEST(test_snap_pct_nearest);
  RUN_TEST(test_duty_inside_window_runs);
  RUN_TEST(test_duty_outside_window_idle);
  RUN_TEST(test_duty_reads_runtime_speed);
  RUN_TEST(test_duty_reads_runtime_minutes);
  RUN_TEST(test_duty_precirc_credit_shrinks_window);
  RUN_TEST(test_duty_credit_exceeds_window_idle);
  RUN_TEST(test_duty_zero_minutes_never_runs);
  return UNITY_END();
}
