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
  // #153 redesign: the wedged-driver signature — one sensor bit-identical
  // while its peer genuinely drifts (0.05 C/min, i.e. the house is moving).
  SensorFusion f;  // real stuck logic wanted here
  f.setStuckWindowS(600);
  f.registerSensor(kA);
  f.registerSensor(kB);
  f.update(kA, 20.0f, Occupancy::kUnknown, 0);
  f.update(kB, 24.0f, Occupancy::kUnknown, 0);
  float prev = f.fusedTemp(0).value;

  FusedTemp r{};
  for (uint32_t t = 60; t <= 1200; t += 60) {
    // Alive peer drifting: +0.05 C/min -> 0.5 C past kB's flat start by 600 s.
    f.update(kA, 20.0f + 0.05f * static_cast<float>(t) / 60.0f,
             Occupancy::kUnknown, t);
    f.update(kB, 24.0f, Occupancy::kUnknown, t);  // frozen value
    r = f.fusedTemp(t);
    TEST_ASSERT_TRUE(std::fabs(r.value - prev) <= dettson::kFusionSlewCPerMin + kSlewEps);
    prev = r.value;
  }
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmStuck);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kSingleRemote), static_cast<int>(r.tier));
  TEST_ASSERT_FALSE(f.status(kB, 1200).live);
  TEST_ASSERT_TRUE(f.status(kA, 1200).live);
}

static void test_flat_house_is_not_stuck() {
  // The #153 field case (slylog 2026-07-10 overnight): EVERY participant sits
  // bit-identical for hours because a regulated space drifting <0.1 C/h below
  // the sensors' 0.1 C quantization step is EXPECTED to repeat exactly.
  // Mutual flatness = stable house — trust must be retained. Under the old
  // zero-variance rule everything here went stuck at t=3601 -> kAlarmAllBad.
  SensorFusion f;  // real defaults: window 3600 s, delta 0.5 C, ceiling 36 h
  f.registerSensor(kA);
  f.registerSensor(kB);
  f.registerSensor(kC);
  FusedTemp r{};
  for (uint32_t t = 0; t <= 6u * 3600u; t += 60) {  // 6 h, all frozen
    f.update(kA, 21.5f, Occupancy::kUnknown, t);
    f.update(kB, 21.3f, Occupancy::kUnknown, t);
    f.update(kC, 20.3f, Occupancy::kUnknown, t);
    r = f.fusedTemp(t);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_FALSE(r.coasting);
    TEST_ASSERT_FALSE(r.alarms & dettson::kAlarmStuck);
    TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kFusedRemotes),
                      static_cast<int>(r.tier));
  }
  TEST_ASSERT_TRUE(f.status(kA, 6u * 3600u).live);
  TEST_ASSERT_TRUE(f.status(kB, 6u * 3600u).live);
  TEST_ASSERT_TRUE(f.status(kC, 6u * 3600u).live);
}

static void test_wedged_sensor_caught_within_window_when_peers_move() {
  // The detector's real purpose, preserved: a frozen driver repeating a stale
  // value while the actual space moves 0.5 C+ must be declared stuck within
  // the suspect window.
  SensorFusion f;
  f.setStuckWindowS(600);
  f.registerSensor(kA);
  f.registerSensor(kB);
  f.registerSensor(kC);
  FusedTemp r{};
  for (uint32_t t = 0; t <= 600; t += 60) {
    const float drift = 0.1f * static_cast<float>(t) / 60.0f;  // +1.0 C over 10 min
    f.update(kA, 21.5f + drift, Occupancy::kUnknown, t);
    f.update(kB, 21.3f, Occupancy::kUnknown, t);  // wedged at its boot value
    f.update(kC, 21.4f + drift, Occupancy::kUnknown, t);
    r = f.fusedTemp(t);
    TEST_ASSERT_FALSE(r.alarms & dettson::kAlarmStuck);  // window not yet expired
  }
  f.update(kA, 22.6f, Occupancy::kUnknown, 660);
  f.update(kB, 21.3f, Occupancy::kUnknown, 660);
  f.update(kC, 22.5f, Occupancy::kUnknown, 660);
  r = f.fusedTemp(660);  // first evaluation past the window: peers moved 1.0 C
  TEST_ASSERT_TRUE(r.valid);  // fusion rides the two live peers
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmStuck);
  TEST_ASSERT_FALSE(f.status(kB, 660).live);
  TEST_ASSERT_TRUE(f.status(kA, 660).live);
  TEST_ASSERT_TRUE(f.status(kC, 660).live);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kFusedRemotes),
                    static_cast<int>(r.tier));
}

static void test_single_moving_peer_is_not_disagreement() {
  // Majority rule (field: lone basement 0.6 C excursions while dining sat
  // legitimately bit-flat for 16 h): ONE drifting room out of several must
  // not condemn a flat-but-healthy sensor.
  SensorFusion f;
  f.setStuckWindowS(600);
  f.registerSensor(kA);
  f.registerSensor(kB);
  f.registerSensor(kC);
  // Priming round so every sensor's flat-run snapshot sees BOTH peers (a
  // sensor's first-ever update can only snapshot peers that already reported).
  f.update(kA, 20.0f, Occupancy::kUnknown, 0);
  f.update(kB, 21.3f, Occupancy::kUnknown, 0);
  f.update(kC, 21.1f, Occupancy::kUnknown, 0);
  FusedTemp r{};
  for (uint32_t t = 30; t <= 1830; t += 60) {
    const float drift = t <= 630 ? 0.1f * static_cast<float>(t - 30) / 60.0f : 1.0f;
    f.update(kA, 20.1f + drift, Occupancy::kUnknown, t);  // the one mover
    f.update(kB, 21.4f, Occupancy::kUnknown, t);          // flat, healthy
    f.update(kC, 21.2f, Occupancy::kUnknown, t);          // flat, healthy
    r = f.fusedTemp(t);
    TEST_ASSERT_TRUE(r.valid);
    // kB/kC: 1 of 2 comparable peers moved -> no strict majority -> live.
    TEST_ASSERT_FALSE(r.alarms & dettson::kAlarmStuck);
  }
  TEST_ASSERT_TRUE(f.status(kB, 1830).live);
  TEST_ASSERT_TRUE(f.status(kC, 1830).live);
  TEST_ASSERT_TRUE(f.status(kA, 1830).live);
}

static void test_solo_sensor_plain_ceiling_and_clamps() {
  // Single-participant operation: no peers to consult, so only the absolute
  // ceiling applies. Field justification (slylog room_temps 07-09..11): the
  // longest bit-flat run by a healthy sensor was 16 h 15 m (dining), so the
  // shipped 36 h default is ~2x the observed maximum.
  TEST_ASSERT_TRUE(dettson::kStuckCeilingS >= 2u * 58470u);  // 2 x 16h14m30s
  SensorFusion f;
  f.setCoastMaxS(0);        // observe the hard verdict, not the grace
  f.setStuckCeilingS(10);   // clamped up to kStuckWindowMinS (300)
  f.setStuckPeerDeltaC(0.01f);  // clamped up to 0.2 (irrelevant: no peers)
  f.registerSensor(kA);
  FusedTemp r{};
  for (uint32_t t = 0; t <= 300; t += 60) {
    f.update(kA, 21.5f, Occupancy::kUnknown, t);  // frozen, fresh, peerless
    r = f.fusedTemp(t);
    TEST_ASSERT_TRUE(r.valid);  // within the (clamped) ceiling: trusted
    TEST_ASSERT_FALSE(r.alarms & dettson::kAlarmStuck);
  }
  f.update(kA, 21.5f, Occupancy::kUnknown, 360);
  r = f.fusedTemp(360);  // past the 300 s ceiling
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmStuck);
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmAllBad);
  f.update(kA, 21.3f, Occupancy::kUnknown, 420);  // a real 0.1 C tick
  r = f.fusedTemp(420);
  TEST_ASSERT_TRUE(r.valid);  // instant recovery on change
}

// ---------- #153 field fixture: slylog room_temps, 2026-07-09/10 overnight ----------
//
// Every distinct-value transition of the four field sensors from 20:00:00 EDT
// 2026-07-09 (t=0) to 08:30:00 the next morning, pulled from the SlyLog DB
// (kdocker2 slylog.room_temps). Sensors publish every 30 s; between the listed
// transitions the values repeat bit-identically — the expected signature of
// 0.1 C-quantized sensors in a regulated space.
//
// Under the removed zero-variance rule this night produced three real fusion
// dropouts (shadow_demands fused_temp_c=0), 61 min total:
//   W1 03:17:31-04:06:31 (t 26251-29191)  bedroom lastChange 02:17:30 + 3600
//   W2 05:53:31-05:59:31 (t 35611-35971)  bedroom lastChange 04:53:00 + 3600
//   W3 06:59:31-07:05:31 (t 39571-39931)  bedroom lastChange 05:59:00 + 3600
// (basement/living/dining had been flat for hours and were already excluded;
// bedroom was the last live participant each time). During ALL THREE windows
// every sensor was bit-flat — mutual flatness, i.e. a stable house, not a
// fault. The redesigned detector must ride through the whole night valid.

namespace fieldnight {
struct Step { uint32_t t; float c; };
constexpr Step kBasement[] = {{0, 20.1f},     {1110, 20.3f},  {2190, 20.5f},
                              {3390, 20.7f},  {8070, 20.5f},  {10020, 20.7f},
                              {13530, 20.5f}, {17670, 20.3f}};
constexpr Step kBedroom[]  = {{0, 22.3f},     {3630, 22.1f},  {7140, 21.9f},
                              {7650, 21.7f},  {9690, 21.9f},  {12450, 21.7f},
                              {12990, 21.5f}, {14730, 21.7f}, {16770, 21.5f},
                              {17310, 21.3f}, {18480, 21.5f}, {21510, 21.7f},
                              {22650, 21.5f}, {29190, 21.3f}, {31980, 21.5f},
                              {35940, 21.3f}, {39930, 21.5f}, {42990, 21.3f},
                              {44070, 21.5f}};
constexpr Step kDining[]   = {{0, 21.4f}};  // bit-flat the entire night (healthy:
                                            // it ticked again at 12:14 on 07-10)
constexpr Step kLiving[]   = {{0, 21.2f},     {3510, 21.1f},  {6630, 21.2f},
                              {15480, 21.3f}, {17760, 21.2f}, {21390, 21.3f},
                              {30750, 21.2f}};

template <size_t N>
float valueAt(const Step (&s)[N], uint32_t t) {
  float v = s[0].c;
  for (const Step& st : s)
    if (st.t <= t) v = st.c;
  return v;
}
inline bool inWindow(uint32_t t) {  // the three former dropout windows
  return (t >= 26251 && t < 29191) || (t >= 35611 && t < 35971) ||
         (t >= 39571 && t < 39931);
}
}  // namespace fieldnight

static void test_field_replay_overnight_no_dropouts() {
  using namespace fieldnight;
  constexpr uint8_t kIdBasement = 1, kIdBedroom = 2, kIdDining = 3, kIdLiving = 4;
  SensorFusion f;  // SHIPPED defaults: window 3600 s, delta 0.5 C, ceiling 36 h
  f.registerSensor(kIdBasement);
  f.registerSensor(kIdBedroom);
  f.registerSensor(kIdDining);
  f.registerSensor(kIdLiving);
  uint32_t invalidS = 0, coastS = 0;
  for (uint32_t t = 0; t <= 45000; t += 30) {  // 12.5 h at the 30 s field cadence
    f.update(kIdBasement, valueAt(kBasement, t), Occupancy::kUnknown, t);
    f.update(kIdBedroom, valueAt(kBedroom, t), Occupancy::kUnknown, t);
    f.update(kIdDining, valueAt(kDining, t), Occupancy::kUnknown, t);
    f.update(kIdLiving, valueAt(kLiving, t), Occupancy::kUnknown, t);
    const FusedTemp r = f.fusedTemp(t);
    if (!r.valid) invalidS += 30;
    if (r.coasting) coastS += 30;
    // The night must never even need the coast grace, let alone go invalid.
    TEST_ASSERT_TRUE_MESSAGE(r.valid, "fusion went invalid on the field night");
    TEST_ASSERT_FALSE_MESSAGE(r.coasting, "fusion needed the coast grace");
    TEST_ASSERT_FALSE(r.alarms & dettson::kAlarmAllBad);
    TEST_ASSERT_TRUE(r.value > 20.0f && r.value < 23.0f);
    if (inWindow(t)) {
      // Inside the three previously-dropped windows: everyone was mutually
      // flat, so nobody may be called stuck — least of all the bedroom, whose
      // window expiry used to be the trigger.
      TEST_ASSERT_FALSE(f.status(kIdBedroom, t).faults & dettson::kAlarmStuck);
      TEST_ASSERT_FALSE(r.alarms & dettson::kAlarmStuck);
      TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kFusedRemotes),
                        static_cast<int>(r.tier));
    }
  }
  TEST_ASSERT_EQUAL_UINT32(0, invalidS);  // was 3660 s under the old policy
  TEST_ASSERT_EQUAL_UINT32(0, coastS);
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
    // Past the clamped 180 the sensor is stale; the #153 grace bridges the
    // young gap, so staleness surfaces as coasting + the stale alarm here.
    FusedTemp r = f.fusedTemp(200);
    TEST_ASSERT_TRUE(r.coasting);
    TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmStale);
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
  FusedTemp r{};
  for (uint32_t t = 0; t <= 1200; t += 60) {
    // Peer genuinely drifting (the wedged signature needs peer movement now).
    f.update(kA, 20.0f + 0.05f * static_cast<float>(t) / 60.0f,
             Occupancy::kUnknown, t);
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

// ---------- Presence: sticky HA-last-seen home/away (issue #88) ----------

static void test_presence_present_within_window() {
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  f.registerSensor(kB);
  const uint32_t now = 100000;
  // last_seen expressed in the monotonic timebase: A 1 h ago, B 10 min ago.
  f.updatePresence(kA, false, now - 3600, /*hasLastSeen=*/true, now);
  f.updatePresence(kB, true, now - 600, /*hasLastSeen=*/true, now);
  const dettson::PresenceState p = f.presence(now);
  TEST_ASSERT_TRUE(p.anyReporting);
  TEST_ASSERT_TRUE(p.everSeen);
  TEST_ASSERT_TRUE(p.present);
  TEST_ASSERT_EQUAL_UINT8(kB, p.dominantId);          // follows most-recently-seen room
  TEST_ASSERT_UINT32_WITHIN(2, 600, p.lastSeenAgeS);
}

static void test_presence_away_past_window() {
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  const uint32_t now = 100000;
  f.updatePresence(kA, false, now - (3u * 3600u + 60u), true, now);  // 3 h 1 min ago
  const dettson::PresenceState p = f.presence(now);
  TEST_ASSERT_TRUE(p.anyReporting);
  TEST_ASSERT_TRUE(p.everSeen);
  TEST_ASSERT_FALSE(p.present);                        // past the 3 h away window
}

static void test_presence_sticky_until_3h_after_last_seen() {
  // The core #88 fix: presence persists for 3 h from last_seen even though no
  // further motion/presence message arrives (it does NOT decay in minutes).
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  const uint32_t t0 = 50000;
  f.updatePresence(kA, true, t0, true, t0);           // seen once at t0
  TEST_ASSERT_TRUE(f.presence(t0 + 2u * 3600u + 3540u).present);   // 2 h 59 m: present
  TEST_ASSERT_FALSE(f.presence(t0 + 3u * 3600u + 60u).present);    // 3 h 1 m: away
}

static void test_presence_seed_from_retained_no_wallclock() {
  // Boot with people home: retained occupied=true but the timestamp can't be
  // placed yet (no wall clock) -> counts as "seen now" -> Present immediately.
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  const uint32_t boot = 30;
  f.updatePresence(kA, true, 0, /*hasLastSeen=*/false, boot);
  const dettson::PresenceState p = f.presence(boot);
  TEST_ASSERT_TRUE(p.present);
  TEST_ASSERT_EQUAL_UINT8(kA, p.dominantId);
}

static void test_presence_no_sensor_reporting_vs_nobody_home() {
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  const uint32_t now = 100000;
  const dettson::PresenceState p0 = f.presence(now);  // nothing reported yet
  TEST_ASSERT_FALSE(p0.anyReporting);                 // UI: "no room sensor reporting"
  TEST_ASSERT_FALSE(p0.everSeen);
  TEST_ASSERT_FALSE(p0.present);
  f.updatePresence(kA, false, now - 4u * 3600u, true, now);  // vacant, seen 4 h ago
  const dettson::PresenceState p1 = f.presence(now);
  TEST_ASSERT_TRUE(p1.anyReporting);                  // UI: "Nobody home"
  TEST_ASSERT_TRUE(p1.everSeen);
  TEST_ASSERT_FALSE(p1.present);
}

static void test_presence_out_of_order_last_seen_ignored() {
  // A late-arriving older retained message must not walk the ledger backwards.
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  const uint32_t now = 100000;
  f.updatePresence(kA, true, now - 60, true, now);    // seen 1 min ago
  f.updatePresence(kA, false, now - 7200, true, now); // stale retained arrives late
  const dettson::PresenceState p = f.presence(now);
  TEST_ASSERT_UINT32_WITHIN(2, 60, p.lastSeenAgeS);   // kept the newer timestamp
  TEST_ASSERT_TRUE(p.present);
}

static void test_presence_excludes_local_sensor() {
  SensorFusion f = makeFusion();
  f.registerSensor(kL, /*isLocal=*/true);
  const uint32_t now = 100000;
  f.updatePresence(kL, true, now, true, now);
  const dettson::PresenceState p = f.presence(now);
  TEST_ASSERT_FALSE(p.anyReporting);                  // local DS18B20 has no presence
  TEST_ASSERT_FALSE(p.present);
}

// ---------- Coast-on-last-good grace (#153) ----------

static void test_coast_bridges_short_gap() {
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  f.update(kA, 21.0f, Occupancy::kUnknown, 0);
  TEST_ASSERT_TRUE(f.fusedTemp(0).valid);
  FusedTemp r = f.fusedTemp(250);            // still fresh (age < 300)
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_FALSE(r.coasting);

  r = f.fusedTemp(320);  // all sources stale; 70 s since the last good eval
  TEST_ASSERT_TRUE(r.valid);                 // bridged, not a safety stop
  TEST_ASSERT_TRUE(r.coasting);
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmCoasting);
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmStale);   // faults stay visible
  TEST_ASSERT_FALSE(r.alarms & dettson::kAlarmAllBad);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f, r.value);    // frozen at last-good
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kSingleRemote),
                    static_cast<int>(r.tier));         // tier carried

  f.update(kA, 21.2f, Occupancy::kUnknown, 360);       // sensor returns
  r = f.fusedTemp(360);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_FALSE(r.coasting);
  TEST_ASSERT_FLOAT_WITHIN(0.3f, 21.0f, r.value);      // continuous, no step
}

static void test_coast_hard_fail_beyond_grace() {
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  f.update(kA, 21.0f, Occupancy::kUnknown, 0);
  TEST_ASSERT_TRUE(f.fusedTemp(250).valid);  // last good eval at 250

  FusedTemp r = f.fusedTemp(250 + dettson::kFusionCoastMaxS + 10);
  TEST_ASSERT_FALSE(r.valid);                // a REAL prolonged failure
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmAllBad);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kNone), static_cast<int>(r.tier));

  // Recovery re-seeds from truth (smoothing state was reset at the hard fail).
  f.update(kA, 25.0f, Occupancy::kUnknown, 500);
  r = f.fusedTemp(500);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_FALSE(r.coasting);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, r.value);
}

static void test_coast_no_grace_at_boot() {
  SensorFusion f = makeFusion();
  f.registerSensor(kA);                      // registered, never updated
  FusedTemp r = f.fusedTemp(50);
  TEST_ASSERT_FALSE(r.valid);                // no last-good to coast on
  TEST_ASSERT_FALSE(r.coasting);
}

static void test_coast_disabled_by_zero() {
  SensorFusion f = makeFusion();
  f.setCoastMaxS(0);
  f.registerSensor(kA);
  f.update(kA, 21.0f, Occupancy::kUnknown, 0);
  TEST_ASSERT_TRUE(f.fusedTemp(250).valid);
  FusedTemp r = f.fusedTemp(320);            // 70 s gap: bridged by default, not here
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_FALSE(r.coasting);
}

static void test_coast_plausibility_band() {
  // The gate every coast candidate must pass — the sensor range band. Every
  // live sample already passed it, so in fusedTemp() this is defense-in-depth
  // against state corruption; the pure helper is asserted directly.
  TEST_ASSERT_TRUE(SensorFusion::coastPlausible(21.0f));
  TEST_ASSERT_TRUE(SensorFusion::coastPlausible(dettson::kSensorRangeMinC));
  TEST_ASSERT_TRUE(SensorFusion::coastPlausible(dettson::kSensorRangeMaxC));
  TEST_ASSERT_FALSE(SensorFusion::coastPlausible(dettson::kSensorRangeMinC - 0.1f));
  TEST_ASSERT_FALSE(SensorFusion::coastPlausible(dettson::kSensorRangeMaxC + 0.1f));
  TEST_ASSERT_FALSE(SensorFusion::coastPlausible(std::nanf("")));
  TEST_ASSERT_FALSE(SensorFusion::coastPlausible(INFINITY));
}

static void test_coast_bridges_stuck_trip() {
  // A solo participant that repeats one 0.1 C-resolution value past the
  // absolute ceiling (the redesigned #153 detector's only peerless trigger):
  // the sensor keeps publishing, fusion goes all-bad anyway — the coast
  // grace bridges the head of the gap.
  SensorFusion f;                            // real stuck logic wanted here
  f.setStuckCeilingS(600);                   // peerless: ceiling is the trigger
  f.registerSensor(kA);
  FusedTemp r{};
  for (uint32_t t = 0; t <= 600; t += 60) {
    f.update(kA, 21.5f, Occupancy::kUnknown, t);  // frozen value, fresh sensor
    r = f.fusedTemp(t);
    TEST_ASSERT_TRUE(r.valid);
  }
  f.update(kA, 21.5f, Occupancy::kUnknown, 660);
  r = f.fusedTemp(660);                      // stuck ceiling expired at 600
  TEST_ASSERT_TRUE(r.valid);                 // grace bridges the trip
  TEST_ASSERT_TRUE(r.coasting);
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmStuck);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.5f, r.value);

  f.update(kA, 21.5f, Occupancy::kUnknown, 780);
  r = f.fusedTemp(780);                      // 180 s past the last good eval
  TEST_ASSERT_FALSE(r.valid);                // grace bounded: hard-invalid
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmAllBad);

  f.update(kA, 21.3f, Occupancy::kUnknown, 840);  // the 0.1 C tick returns
  r = f.fusedTemp(840);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_FALSE(r.coasting);
}

static void test_coast_carries_degraded_lockout() {
  // A coast off the local-degraded rung must keep degraded=true so the
  // caller's cooling lockout holds through the bridged gap.
  SensorFusion f = makeFusion();
  f.registerSensor(kA);
  f.registerSensor(kL, /*isLocal=*/true);
  f.update(kA, 21.0f, Occupancy::kUnknown, 0);   // remote dies after this
  int i = 0;
  FusedTemp r{};
  for (uint32_t t = 0; t <= 900; t += 60) {
    f.update(kL, jitter(20.0f, i++), Occupancy::kUnknown, t);
    r = f.fusedTemp(t);
  }
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kLocalDegraded),
                    static_cast<int>(r.tier));    // riding the local fallback
  TEST_ASSERT_TRUE(f.fusedTemp(1190).valid);      // local age 290: last good eval
  r = f.fusedTemp(1210);                          // local stale; 20 s young gap
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_TRUE(r.coasting);
  TEST_ASSERT_TRUE(r.degraded);                   // lockout preserved
  TEST_ASSERT_TRUE(r.alarms & dettson::kAlarmDegraded);
  TEST_ASSERT_EQUAL(static_cast<int>(SourceTier::kLocalDegraded),
                    static_cast<int>(r.tier));
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
  RUN_TEST(test_flat_house_is_not_stuck);
  RUN_TEST(test_wedged_sensor_caught_within_window_when_peers_move);
  RUN_TEST(test_single_moving_peer_is_not_disagreement);
  RUN_TEST(test_solo_sensor_plain_ceiling_and_clamps);
  RUN_TEST(test_field_replay_overnight_no_dropouts);
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
  RUN_TEST(test_presence_present_within_window);
  RUN_TEST(test_presence_away_past_window);
  RUN_TEST(test_presence_sticky_until_3h_after_last_seen);
  RUN_TEST(test_presence_seed_from_retained_no_wallclock);
  RUN_TEST(test_presence_no_sensor_reporting_vs_nobody_home);
  RUN_TEST(test_presence_out_of_order_last_seen_ignored);
  RUN_TEST(test_presence_excludes_local_sensor);
  RUN_TEST(test_coast_bridges_short_gap);
  RUN_TEST(test_coast_hard_fail_beyond_grace);
  RUN_TEST(test_coast_no_grace_at_boot);
  RUN_TEST(test_coast_disabled_by_zero);
  RUN_TEST(test_coast_plausibility_band);
  RUN_TEST(test_coast_bridges_stuck_trip);
  RUN_TEST(test_coast_carries_degraded_lockout);
  return UNITY_END();
}
