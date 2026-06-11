// SensorFusion unit tests (issue #19): health gates, occupancy weighting,
// slew-limited participant changes, fallback ladder, all-bad -> invalid.
#include <unity.h>

#include <cmath>

#include "DettsonConfig.h"
#include "SensorFusion.h"

using dettson::FusedTemp;
using dettson::Occupancy;
using dettson::SensorFusion;
using dettson::SourceTier;

namespace {
constexpr uint8_t kA = 1, kB = 2, kC = 3, kL = 10;
constexpr float kSlewEps = 1e-3f;  // float headroom on the 0.1 C/min budget

// Stuck window pushed out of the way for tests that hold values constant.
SensorFusion makeFusion() {
  SensorFusion f;
  f.setStuckWindowS(1000000);
  return f;
}

// Tiny alternation so a deliberately-fresh sensor never trips zero-variance.
float jitter(float base, int i) { return base + 0.01f * static_cast<float>(i % 2); }
}  // namespace

void setUp() {}
void tearDown() {}

// ---------- Boot & registration ----------

static void test_boot_state_invalid_no_demand() {
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  FusedTemp r = f.fusedTemp(0);  // registered but never updated
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kNone), static_cast<int>(r.tier));
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmAllBad);
}

static void test_registration_limits_and_duplicates() {
  SensorFusion f = makeFusion();
  for (uint8_t i = 0; i < SensorFusion::kMaxSensors; ++i)
    TEST_ASSERT_TRUE(f.registerSensor(static_cast<uint8_t>(20 + i)));
  TEST_ASSERT_FALSE(f.registerSensor(99));   // table full
  TEST_ASSERT_FALSE(f.registerSensor(20));   // duplicate id
  TEST_ASSERT_FALSE(f.update(99, 21.0f, Occupancy::kUnknown, 0));  // unregistered
}

static void test_basic_fusion_seed() {
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  f.registerSensor(kB);
  f.update(kA, 20.0f, Occupancy::kUnknown, 0);
  f.update(kB, 24.0f, Occupancy::kUnknown, 0);
  FusedTemp r = f.fusedTemp(0);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_FALSE(r.degraded);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kFusedRemotes), static_cast<int>(r.tier));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 22.0f, r.value);
  TEST_ASSERT_EQUAL_UINT16(0, r.alarms);
}

// ---------- Occupancy weighting ----------

static void test_occupancy_weight_ramp_continuity() {
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  f.registerSensor(kB);
  f.update(kA, 20.0f, Occupancy::kUnknown, 0);
  f.update(kB, 24.0f, Occupancy::kUnknown, 0);
  FusedTemp r = f.fusedTemp(0);
  float prev = r.value;

  uint32_t t = 0;
  for (t = 10; t <= 600; t += 10) {  // settle unoccupied
    f.update(kA, 20.0f, Occupancy::kUnknown, t);
    f.update(kB, 24.0f, Occupancy::kUnknown, t);
    r = f.fusedTemp(t);
    prev = r.value;
  }

  // B becomes occupied: weight ramps in exponentially -> output drifts toward
  // (20 + kOccupiedWeight*24)/(1 + kOccupiedWeight) with NO step at any tick.
  float maxStep = 0.0f;
  for (; t <= 4800; t += 10) {
    f.update(kA, 20.0f, Occupancy::kUnknown, t);
    f.update(kB, 24.0f, Occupancy::kOccupied, t);
    r = f.fusedTemp(t);
    float step = std::fabs(r.value - prev);
    if (step > maxStep) maxStep = step;
    prev = r.value;
  }
  const float target =
      (20.0f + dettson::kOccupiedWeight * 24.0f) / (1.0f + dettson::kOccupiedWeight);
  TEST_ASSERT_TRUE_MESSAGE(maxStep < 0.05f, "weight ramp stepped the output");
  TEST_ASSERT_FLOAT_WITHIN(0.05f, target, r.value);
  TEST_ASSERT_EQUAL_UINT16(0, r.alarms);
}

// ---------- Staleness dropout + slew-limited rejoin ----------

static void test_stale_dropout_slew_and_no_step_rejoin() {
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  f.registerSensor(kB);  // default max age = kSensorMaxAgeS (300 s)
  f.update(kA, 20.0f, Occupancy::kUnknown, 0);
  f.update(kB, 24.0f, Occupancy::kUnknown, 0);
  float prev = f.fusedTemp(0).value;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 22.0f, prev);

  // B goes silent; A stays fresh. After 300 s B drops out — output must RAMP
  // to ~20 at <= kFusionSlewCPerMin, never step.
  int i = 0;
  bool sawStale = false;
  FusedTemp r{};
  uint32_t t;
  for (t = 60; t <= 2100; t += 60) {
    f.update(kA, jitter(20.0f, i++), Occupancy::kUnknown, t);
    r = f.fusedTemp(t);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_TRUE_MESSAGE(
        std::fabs(r.value - prev) <= dettson::kFusionSlewCPerMin + kSlewEps,
        "dropout stepped the output past the slew limit");
    prev = r.value;
    if (r.alarms & dettson::kAlarmStale) sawStale = true;
  }
  TEST_ASSERT_TRUE(sawStale);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kSingleRemote), static_cast<int>(r.tier));
  TEST_ASSERT_FLOAT_WITHIN(0.15f, 20.0f, prev);

  // B rejoins: same rule — ramp back toward 22, no step.
  for (t = 2160; t <= 4500; t += 60) {
    f.update(kA, jitter(20.0f, i), Occupancy::kUnknown, t);
    f.update(kB, jitter(24.0f, i), Occupancy::kUnknown, t);
    ++i;
    r = f.fusedTemp(t);
    TEST_ASSERT_TRUE_MESSAGE(
        std::fabs(r.value - prev) <= dettson::kFusionSlewCPerMin + kSlewEps,
        "rejoin stepped the output past the slew limit");
    prev = r.value;
  }
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kFusedRemotes), static_cast<int>(r.tier));
  TEST_ASSERT_FLOAT_WITHIN(0.15f, 22.0f, prev);
}

// ---------- Range gate (incl. NaN) ----------

static void test_range_gate_and_nan_rejection() {
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  f.registerSensor(kB);
  f.update(kA, 20.0f, Occupancy::kUnknown, 0);
  f.update(kB, 24.0f, Occupancy::kUnknown, 0);
  float prev = f.fusedTemp(0).value;

  f.update(kA, 20.01f, Occupancy::kUnknown, 60);
  f.update(kB, 45.0f, Occupancy::kUnknown, 60);  // above kSensorRangeMaxC
  FusedTemp r = f.fusedTemp(60);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmRange);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kSingleRemote), static_cast<int>(r.tier));
  TEST_ASSERT_TRUE(std::fabs(r.value - prev) <= dettson::kFusionSlewCPerMin + kSlewEps);

  f.update(kA, 20.0f, Occupancy::kUnknown, 120);
  f.update(kB, std::nanf(""), Occupancy::kUnknown, 120);  // invalid input
  r = f.fusedTemp(120);
  TEST_ASSERT_TRUE(r.valid);  // A still carries the input
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmRange);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kSingleRemote), static_cast<int>(r.tier));
  TEST_ASSERT_FALSE(std::isnan(r.value));
}

// ---------- Stuck-value detection ----------

static void test_stuck_value_exclusion() {
  SensorFusion f;  // real stuck logic wanted here
  f.setStuckWindowS(600);
  f.registerSensor(kA);
  f.registerSensor(kB);
  f.update(kA, 20.0f, Occupancy::kUnknown, 0);
  f.update(kB, 24.0f, Occupancy::kUnknown, 0);
  float prev = f.fusedTemp(0).value;

  int i = 0;
  FusedTemp r{};
  for (uint32_t t = 60; t <= 1200; t += 60) {
    f.update(kA, jitter(20.0f, i++), Occupancy::kUnknown, t);  // alive
    f.update(kB, 24.0f, Occupancy::kUnknown, t);               // frozen value
    r = f.fusedTemp(t);
    TEST_ASSERT_TRUE(std::fabs(r.value - prev) <= dettson::kFusionSlewCPerMin + kSlewEps);
    prev = r.value;
  }
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmStuck);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kSingleRemote), static_cast<int>(r.tier));
  TEST_ASSERT_FALSE(f.status(kB, 1200).live);
  TEST_ASSERT_TRUE(f.status(kA, 1200).live);
}

// ---------- Outlier vs median ----------

static void test_outlier_excluded_with_alarm() {
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  f.registerSensor(kB);
  f.registerSensor(kC);
  f.update(kA, 20.0f, Occupancy::kUnknown, 0);
  f.update(kB, 20.5f, Occupancy::kUnknown, 0);
  f.update(kC, 28.0f, Occupancy::kUnknown, 0);  // 7.5 C from median 20.5
  FusedTemp r = f.fusedTemp(0);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmOutlier);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kFusedRemotes), static_cast<int>(r.tier));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.25f, r.value);  // fused without the outlier
  TEST_ASSERT_FALSE(f.status(kC, 0).live);
}

static void test_two_sensors_no_false_outlier_kill() {
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  f.registerSensor(kB);
  f.update(kA, 20.0f, Occupancy::kUnknown, 0);
  f.update(kB, 27.0f, Occupancy::kUnknown, 0);  // 7 C apart: can't tell which is wrong
  FusedTemp r = f.fusedTemp(0);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_FALSE(r.alarms & dettson::kAlarmOutlier);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kFusedRemotes), static_cast<int>(r.tier));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 23.5f, r.value);  // both kept
}

// ---------- Fallback ladder & degraded mode ----------

static void test_ladder_to_local_degraded_and_back() {
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  f.registerSensor(kB);
  f.registerSensor(kL, /*isLocal=*/true);
  f.update(kA, 20.0f, Occupancy::kUnknown, 0);
  f.update(kB, 24.0f, Occupancy::kUnknown, 0);
  f.update(kL, 19.0f, Occupancy::kUnknown, 0);
  FusedTemp r = f.fusedTemp(0);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kFusedRemotes), static_cast<int>(r.tier));
  TEST_ASSERT_FALSE(r.degraded);
  TEST_ASSERT_FALSE(r.alarms & dettson::kAlarmLocalDisagree);  // |19-22| < 5
  float prev = r.value;

  // Remotes go silent; local DS18B20 keeps reporting -> DEGRADED, valid, no step.
  int i = 0;
  bool sawDegraded = false;
  uint32_t t;
  for (t = 60; t <= 1800; t += 60) {
    f.update(kL, jitter(19.0f, i++), Occupancy::kUnknown, t);
    r = f.fusedTemp(t);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_TRUE_MESSAGE(
        std::fabs(r.value - prev) <= dettson::kFusionSlewCPerMin + kSlewEps,
        "degraded transition stepped the output");
    prev = r.value;
    if (r.tier == SourceTier::kLocalDegraded) {
      sawDegraded = true;
      TEST_ASSERT_TRUE(r.degraded);
      TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmDegraded);
    }
  }
  TEST_ASSERT_TRUE(sawDegraded);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kLocalDegraded), static_cast<int>(r.tier));

  // Remotes return -> back up the ladder, still no step.
  for (t = 1860; t <= 2400; t += 60) {
    f.update(kA, jitter(20.0f, i), Occupancy::kUnknown, t);
    f.update(kB, jitter(24.0f, i), Occupancy::kUnknown, t);
    f.update(kL, jitter(19.0f, i), Occupancy::kUnknown, t);
    ++i;
    r = f.fusedTemp(t);
    TEST_ASSERT_TRUE(std::fabs(r.value - prev) <= dettson::kFusionSlewCPerMin + kSlewEps);
    prev = r.value;
  }
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kFusedRemotes), static_cast<int>(r.tier));
  TEST_ASSERT_FALSE(r.degraded);
}

static void test_local_vs_fusion_disagreement_alarm() {
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  f.registerSensor(kB);
  f.registerSensor(kL, /*isLocal=*/true);
  f.update(kA, 20.0f, Occupancy::kUnknown, 0);
  f.update(kB, 24.0f, Occupancy::kUnknown, 0);
  f.update(kL, 28.5f, Occupancy::kUnknown, 0);  // |28.5 - 22| > kDs18b20DisagreeAlarmC
  FusedTemp r = f.fusedTemp(0);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kFusedRemotes), static_cast<int>(r.tier));
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmLocalDisagree);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 22.0f, r.value);  // fusion value unchanged by alarm
}

static void test_all_bad_invalid_then_recovery_reseeds() {
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  f.registerSensor(kL, /*isLocal=*/true);
  f.update(kA, 22.0f, Occupancy::kUnknown, 0);  // local never reports
  FusedTemp r = f.fusedTemp(0);
  TEST_ASSERT_TRUE(r.valid);

  r = f.fusedTemp(1000);  // A now stale, local absent -> nothing left
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kNone), static_cast<int>(r.tier));
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmAllBad);
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmStale);

  // Recovery re-seeds from truth (caller was at no-demand; CompressorGuard
  // owns the min-off on the demand path).
  f.update(kA, 21.0f, Occupancy::kUnknown, 1060);
  r = f.fusedTemp(1060);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kSingleRemote), static_cast<int>(r.tier));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f, r.value);
}

// ---------- Config clamps ----------

static void test_max_age_clamped_to_documented_range() {
  {  // below the 180 s floor -> clamped up to 180
    SensorFusion f = makeFusion();
    f.registerSensor(kA, false, true, /*maxAgeS=*/60);
    f.update(kA, 21.0f, Occupancy::kUnknown, 0);
    TEST_ASSERT_TRUE(f.fusedTemp(170).valid);    // would be stale if 60 stood
    TEST_ASSERT_FALSE(f.fusedTemp(200).valid);   // past the clamped 180
  }
  {  // above the 900 s cap -> clamped down to 900
    SensorFusion f = makeFusion();
    f.registerSensor(kB, false, true, /*maxAgeS=*/5000);
    f.update(kB, 21.0f, Occupancy::kUnknown, 0);
    TEST_ASSERT_TRUE(f.fusedTemp(800).valid);
    TEST_ASSERT_FALSE(f.fusedTemp(1000).valid);  // past the clamped 900
  }
}

static void test_non_participating_sensor_ignored() {
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  f.registerSensor(kB, false, /*participating=*/false);
  f.update(kA, 20.0f, Occupancy::kUnknown, 0);
  f.update(kB, 30.0f, Occupancy::kUnknown, 0);
  FusedTemp r = f.fusedTemp(0);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kSingleRemote), static_cast<int>(r.tier));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, r.value);
  TEST_ASSERT_EQUAL_UINT16(0, r.alarms);  // out-of-profile sensor raises nothing
}

// ---------- Calibration offsets (issue #49, gap G6) ----------

static void test_offset_clamped_at_plus_minus_5() {
  {
    SensorFusion f = makeFusion();
    f.registerSensor(kA);
    TEST_ASSERT_TRUE(f.setSensorOffsetC(kA, 7.0f));  // accepted, clamped to +5
    f.update(kA, 20.0f, Occupancy::kUnknown, 0);
    FusedTemp r = f.fusedTemp(0);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, r.value);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, dettson::kSensorOffsetMaxC,
                             f.status(kA, 0).offsetC);
  }
  {
    SensorFusion f = makeFusion();
    f.registerSensor(kA);
    TEST_ASSERT_TRUE(f.setSensorOffsetC(kA, -7.0f));  // clamped to -5
    f.update(kA, 20.0f, Occupancy::kUnknown, 0);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 15.0f, f.fusedTemp(0).value);
  }
  {
    SensorFusion f = makeFusion();
    f.registerSensor(kA);
    TEST_ASSERT_FALSE(f.setSensorOffsetC(kA, std::nanf("")));  // non-finite
    TEST_ASSERT_FALSE(f.setSensorOffsetC(99, 1.0f));           // unregistered
  }
}

static void test_offset_correction_precedes_health_gates() {
  {  // out-of-range raw, in-range corrected -> accepted
    SensorFusion f = makeFusion();
    f.registerSensor(kA);
    f.setSensorOffsetC(kA, -3.0f);
    f.update(kA, 42.0f, Occupancy::kUnknown, 0);  // raw above kSensorRangeMaxC
    FusedTemp r = f.fusedTemp(0);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_FALSE(r.alarms & dettson::kAlarmRange);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 39.0f, r.value);
  }
  {  // in-range raw, out-of-range corrected -> rejected
    SensorFusion f = makeFusion();
    f.registerSensor(kA);
    f.setSensorOffsetC(kA, 3.0f);
    f.update(kA, 38.0f, Occupancy::kUnknown, 0);  // corrected 41 > 40
    FusedTemp r = f.fusedTemp(0);
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmRange);
  }
  {  // outlier gate sees corrected values: raw outlier saved by its offset
    SensorFusion f = makeFusion();
    f.registerSensor(kA);
    f.registerSensor(kB);
    f.registerSensor(kC);
    f.setSensorOffsetC(kC, -5.0f);
    f.update(kA, 20.0f, Occupancy::kUnknown, 0);
    f.update(kB, 20.5f, Occupancy::kUnknown, 0);
    f.update(kC, 28.0f, Occupancy::kUnknown, 0);  // corrected 23: within 4 C of median
    FusedTemp r = f.fusedTemp(0);
    TEST_ASSERT_FALSE(r.alarms & dettson::kAlarmOutlier);
    TEST_ASSERT_TRUE(f.status(kC, 0).live);
  }
}

static void test_offset_change_routes_through_slew_no_step() {
  SensorFusion f;
  f.setStuckWindowS(600);  // real stuck logic: the edit must not trip it
  f.registerSensor(kA);
  f.registerSensor(kB);
  int i = 0;
  float prev = 0.0f;
  for (uint32_t t = 0; t <= 600; t += 60) {
    f.update(kA, jitter(20.0f, i), Occupancy::kUnknown, t);
    f.update(kB, jitter(24.0f, i), Occupancy::kUnknown, t);
    ++i;
    prev = f.fusedTemp(t).value;
  }
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 22.0f, prev);

  TEST_ASSERT_TRUE(f.setSensorOffsetC(kA, 2.0f));  // fused target 22 -> 23
  FusedTemp r{};
  for (uint32_t t = 660; t <= 3600; t += 60) {
    f.update(kA, jitter(20.0f, i), Occupancy::kUnknown, t);
    f.update(kB, jitter(24.0f, i), Occupancy::kUnknown, t);
    ++i;
    r = f.fusedTemp(t);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_FALSE(r.alarms & dettson::kAlarmStuck);
    TEST_ASSERT_TRUE_MESSAGE(
        std::fabs(r.value - prev) <= dettson::kFusionSlewCPerMin + kSlewEps,
        "offset change stepped the output past the slew limit");
    prev = r.value;
  }
  TEST_ASSERT_FLOAT_WITHIN(0.15f, 23.0f, prev);
}

static void test_offset_edit_does_not_mask_stuck_detector() {
  SensorFusion f;
  f.setStuckWindowS(600);
  f.registerSensor(kA);
  f.registerSensor(kB);
  int i = 0;
  FusedTemp r{};
  for (uint32_t t = 0; t <= 1200; t += 60) {
    f.update(kA, jitter(20.0f, i++), Occupancy::kUnknown, t);
    f.update(kB, 24.0f, Occupancy::kUnknown, t);  // frozen raw value
    if (t == 300) f.setSensorOffsetC(kB, 1.0f);   // mid-window edit
    r = f.fusedTemp(t);
  }
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmStuck);  // detected on schedule
  TEST_ASSERT_FALSE(f.status(kB, 1200).live);
  TEST_ASSERT_TRUE(f.status(kA, 1200).live);
}

static void test_fallback_local_sensor_offset_applied() {
  SensorFusion f = makeFusion();
  f.registerSensor(kL, /*isLocal=*/true);
  TEST_ASSERT_TRUE(f.setSensorOffsetC(kL, -1.5f));
  f.update(kL, 19.0f, Occupancy::kUnknown, 0);
  FusedTemp r = f.fusedTemp(0);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_TRUE(r.degraded);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kLocalDegraded),
                    static_cast<int>(r.tier));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 17.5f, r.value);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_boot_state_invalid_no_demand);
  RUN_TEST(test_registration_limits_and_duplicates);
  RUN_TEST(test_basic_fusion_seed);
  RUN_TEST(test_occupancy_weight_ramp_continuity);
  RUN_TEST(test_stale_dropout_slew_and_no_step_rejoin);
  RUN_TEST(test_range_gate_and_nan_rejection);
  RUN_TEST(test_stuck_value_exclusion);
  RUN_TEST(test_outlier_excluded_with_alarm);
  RUN_TEST(test_two_sensors_no_false_outlier_kill);
  RUN_TEST(test_ladder_to_local_degraded_and_back);
  RUN_TEST(test_local_vs_fusion_disagreement_alarm);
  RUN_TEST(test_all_bad_invalid_then_recovery_reseeds);
  RUN_TEST(test_max_age_clamped_to_documented_range);
  RUN_TEST(test_non_participating_sensor_ignored);
  RUN_TEST(test_offset_clamped_at_plus_minus_5);
  RUN_TEST(test_offset_correction_precedes_health_gates);
  RUN_TEST(test_offset_change_routes_through_slew_no_step);
  RUN_TEST(test_offset_edit_does_not_mask_stuck_detector);
  RUN_TEST(test_fallback_local_sensor_offset_applied);
  return UNITY_END();
}
