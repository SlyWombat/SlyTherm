// DemandArbiter unit tests: invariant trips (heat+cool, gas+hp), the sole
// sanctioned defrost-temper overlap, changeover dwell, boot state, invalid input.
#include <unity.h>
#include <cmath>
#include "DemandArbiter.h"
#include "DettsonConfig.h"

using namespace dettson;

void setUp() {}
void tearDown() {}

static constexpr uint32_t kDwell = kCompressorMinOffS;  // default config dwell

static void test_boot_state_all_zero() {
  DemandArbiter a(0);
  TEST_ASSERT_FALSE(a.current().anyNonzero());
  TEST_ASSERT_FALSE(a.invariantAlarm());
}

static void test_boot_dwell_holds_both_directions() {
  DemandArbiter a(0);
  DemandRequest heat; heat.gasHeatPct = 50.0f;
  DemandSet out = a.set(heat, 10);  // prior activity unknown: assume worst
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.gasHeatPct);
  TEST_ASSERT_TRUE(a.heatHeldByDwell());

  DemandArbiter b(0);
  DemandRequest cool; cool.coolPct = 50.0f;
  out = b.set(cool, 10);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.coolPct);
  TEST_ASSERT_TRUE(b.coolHeldByDwell());

  out = a.set(heat, kDwell);  // dwell served from boot
  TEST_ASSERT_EQUAL_FLOAT(50.0f, out.gasHeatPct);
  TEST_ASSERT_FALSE(a.heatHeldByDwell());
}

static void test_heat_and_cool_conflict_trips_and_latches() {
  DemandArbiter a(0);
  DemandRequest req;
  req.gasHeatPct = 50.0f;
  req.coolPct = 50.0f;
  req.fanPct = 30.0f;
  DemandSet out = a.set(req, kDwell);
  TEST_ASSERT_FALSE(out.anyNonzero());  // ALL channels zero, including fan
  TEST_ASSERT_TRUE(a.invariantAlarm());

  // Latched: a now-valid request still emits nothing.
  DemandRequest ok; ok.gasHeatPct = 50.0f;
  out = a.set(ok, kDwell + 100);
  TEST_ASSERT_FALSE(out.anyNonzero());

  a.clearInvariantAlarm();  // manual clear (docs/04 §2)
  out = a.set(ok, kDwell + 200);
  TEST_ASSERT_EQUAL_FLOAT(50.0f, out.gasHeatPct);
}

static void test_hp_heat_plus_cool_trips() {
  DemandArbiter a(0);
  DemandRequest req;
  req.hpHeatPct = 60.0f;
  req.coolPct = 40.0f;
  TEST_ASSERT_FALSE(a.set(req, kDwell).anyNonzero());
  TEST_ASSERT_TRUE(a.invariantAlarm());
}

static void test_defrost_temper_plus_cool_trips() {
  DemandArbiter a(0);
  DemandRequest req;
  req.defrostTemperPct = kDefrostTemperHeatPct;
  req.coolPct = 40.0f;
  TEST_ASSERT_FALSE(a.set(req, kDwell).anyNonzero());
  TEST_ASSERT_TRUE(a.invariantAlarm());
}

static void test_gas_plus_hp_heat_trips() {
  DemandArbiter a(0);
  DemandRequest req;
  req.gasHeatPct = 50.0f;
  req.hpHeatPct = 60.0f;
  TEST_ASSERT_FALSE(a.set(req, kDwell).anyNonzero());
  TEST_ASSERT_TRUE(a.invariantAlarm());
}

static void test_defrost_temper_with_hp_is_the_sanctioned_overlap() {
  DemandArbiter a(0);
  DemandRequest req;
  req.hpHeatPct = 60.0f;
  req.defrostTemperPct = kDefrostTemperHeatPct;
  DemandSet out = a.set(req, kDwell);
  TEST_ASSERT_EQUAL_FLOAT(60.0f, out.hpHeatPct);
  TEST_ASSERT_EQUAL_FLOAT(kDefrostTemperHeatPct, out.defrostTemperPct);
  TEST_ASSERT_FALSE(a.invariantAlarm());
}

static void test_gas_plus_defrost_temper_trips() {
  DemandArbiter a(0);
  DemandRequest req;
  req.gasHeatPct = 50.0f;
  req.defrostTemperPct = kDefrostTemperHeatPct;  // two gas channels at once
  TEST_ASSERT_FALSE(a.set(req, kDwell).anyNonzero());
  TEST_ASSERT_TRUE(a.invariantAlarm());
}

static void test_heat_to_cool_changeover_dwell() {
  DemandArbiter a(0);
  DemandRequest heat; heat.gasHeatPct = 50.0f;
  a.set(heat, kDwell);            // heat active; last-active = kDwell
  a.set(heat, kDwell + 100);      // still active; last-active = kDwell + 100
  a.set(DemandRequest{}, kDwell + 200);  // call ends (does not move last-active)

  DemandRequest cool; cool.coolPct = 70.0f; cool.fanPct = 30.0f;
  DemandSet out = a.set(cool, kDwell + 100 + kDwell - 1);  // 1 s short of dwell
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.coolPct);
  TEST_ASSERT_TRUE(a.coolHeldByDwell());
  TEST_ASSERT_EQUAL_FLOAT(30.0f, out.fanPct);  // fan has no opposite: passes through

  out = a.set(cool, kDwell + 100 + kDwell);
  TEST_ASSERT_EQUAL_FLOAT(70.0f, out.coolPct);
  TEST_ASSERT_FALSE(a.coolHeldByDwell());
  TEST_ASSERT_FALSE(a.invariantAlarm());  // dwell hold is a suppression, not a trip
}

static void test_cool_to_heat_changeover_dwell() {
  DemandArbiter a(0);
  DemandRequest cool; cool.coolPct = 70.0f;
  a.set(cool, kDwell);                   // cool active
  a.set(DemandRequest{}, kDwell + 50);   // call ends

  DemandRequest heat; heat.hpHeatPct = 60.0f;
  DemandSet out = a.set(heat, kDwell + kDwell - 1);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.hpHeatPct);
  TEST_ASSERT_TRUE(a.heatHeldByDwell());

  out = a.set(heat, kDwell + kDwell);
  TEST_ASSERT_EQUAL_FLOAT(60.0f, out.hpHeatPct);
}

static void test_suppressed_request_does_not_reset_opposite_dwell() {
  DemandArbiter a(0);
  DemandRequest heat; heat.gasHeatPct = 50.0f;
  a.set(heat, kDwell);  // heat emitted at kDwell, then silence
  // A cool request during the dwell is suppressed — and that suppressed
  // request must not count as cool activity against future heat calls.
  DemandRequest cool; cool.coolPct = 60.0f;
  a.set(cool, kDwell + 10);
  DemandSet out = a.set(heat, kDwell + 20);
  TEST_ASSERT_EQUAL_FLOAT(50.0f, out.gasHeatPct);  // no fresh cool-dwell imposed
}

static void test_invalid_input_zeroes_all_and_flags_not_latched() {
  DemandArbiter a(0);
  DemandRequest req;
  req.gasHeatPct = 50.0f;
  req.coolPct = NAN;
  DemandSet out = a.set(req, kDwell);
  TEST_ASSERT_FALSE(out.anyNonzero());
  TEST_ASSERT_TRUE(a.inputAlarm());
  TEST_ASSERT_FALSE(a.invariantAlarm());  // input fault is not the latched invariant trip

  DemandRequest neg; neg.fanPct = -1.0f;
  out = a.set(neg, kDwell + 10);
  TEST_ASSERT_FALSE(out.anyNonzero());
  TEST_ASSERT_TRUE(a.inputAlarm());

  DemandRequest ok; ok.gasHeatPct = 50.0f;
  out = a.set(ok, kDwell + 20);  // next valid set works
  TEST_ASSERT_EQUAL_FLOAT(50.0f, out.gasHeatPct);
  TEST_ASSERT_FALSE(a.inputAlarm());
}

static void test_overrange_requests_clamp_to_100() {
  DemandArbiter a(0);
  DemandRequest req;
  req.gasHeatPct = 150.0f;
  req.fanPct = 200.0f;
  DemandSet out = a.set(req, kDwell);
  TEST_ASSERT_EQUAL_FLOAT(100.0f, out.gasHeatPct);
  TEST_ASSERT_EQUAL_FLOAT(100.0f, out.fanPct);
}

static void test_every_set_revalidates_current_tracks_emission() {
  DemandArbiter a(0);
  DemandRequest heat; heat.gasHeatPct = 50.0f;
  a.set(heat, kDwell);
  TEST_ASSERT_EQUAL_FLOAT(50.0f, a.current().gasHeatPct);
  DemandRequest bad; bad.gasHeatPct = 50.0f; bad.coolPct = 1.0f;
  a.set(bad, kDwell + 10);
  TEST_ASSERT_FALSE(a.current().anyNonzero());  // trip reflected immediately
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_boot_state_all_zero);
  RUN_TEST(test_boot_dwell_holds_both_directions);
  RUN_TEST(test_heat_and_cool_conflict_trips_and_latches);
  RUN_TEST(test_hp_heat_plus_cool_trips);
  RUN_TEST(test_defrost_temper_plus_cool_trips);
  RUN_TEST(test_gas_plus_hp_heat_trips);
  RUN_TEST(test_defrost_temper_with_hp_is_the_sanctioned_overlap);
  RUN_TEST(test_gas_plus_defrost_temper_trips);
  RUN_TEST(test_heat_to_cool_changeover_dwell);
  RUN_TEST(test_cool_to_heat_changeover_dwell);
  RUN_TEST(test_suppressed_request_does_not_reset_opposite_dwell);
  RUN_TEST(test_invalid_input_zeroes_all_and_flags_not_latched);
  RUN_TEST(test_overrange_requests_clamp_to_100);
  RUN_TEST(test_every_set_revalidates_current_tracks_emission);
  return UNITY_END();
}
