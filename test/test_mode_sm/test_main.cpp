// ModeStateMachine unit tests: deadband push (Ecobee-style), call
// hysteresis no-chatter, AUTO changeover sustain+dwell+compressor gates,
// EMERGENCY_HEAT gas-only, invalid-input fail-to-no-demand (docs/04 §2).
#include <unity.h>

#include <cmath>

#include "DettsonConfig.h"
#include "ModeStateMachine.h"

using dettson::Call;
using dettson::CallType;
using dettson::ModeStateMachine;
using dettson::UserMode;

void setUp() {}
void tearDown() {}

// ---------- boot state ----------

static void test_boot_state_no_demand() {
  ModeStateMachine sm;
  TEST_ASSERT_EQUAL(UserMode::kOff, sm.mode());
  TEST_ASSERT_EQUAL(CallType::kNone, sm.call().type);
  Call c = sm.update(15.0f, true, 0);  // cold room, but mode OFF
  TEST_ASSERT_EQUAL(CallType::kNone, c.type);
  TEST_ASSERT_FALSE(sm.inputFaultAlarm());
}

// ---------- deadband push ----------

static void test_deadband_cool_write_pushes_heat_down() {
  ModeStateMachine sm;  // boot: heat 18, cool 27, delta 2.8
  auto r = sm.setCoolSetpoint(19.0f);
  TEST_ASSERT_TRUE(r.accepted);
  TEST_ASSERT_TRUE(r.coolChanged);
  TEST_ASSERT_TRUE(r.heatChanged);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 19.0f, r.coolC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 19.0f - dettson::kMinSetpointDeltaC, r.heatC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, r.heatC, sm.heatSetpoint());
}

static void test_deadband_heat_write_pushes_cool_up() {
  ModeStateMachine sm;
  auto r = sm.setHeatSetpoint(25.0f);  // cool at 27 < 25 + 2.8
  TEST_ASSERT_TRUE(r.accepted);
  TEST_ASSERT_TRUE(r.heatChanged);
  TEST_ASSERT_TRUE(r.coolChanged);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, r.heatC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 27.8f, r.coolC);
}

static void test_deadband_nonviolating_write_pushes_nothing() {
  ModeStateMachine sm;
  auto r = sm.setHeatSetpoint(20.0f);  // 27 - 20 = 7 >= 2.8
  TEST_ASSERT_TRUE(r.accepted);
  TEST_ASSERT_TRUE(r.heatChanged);
  TEST_ASSERT_FALSE(r.coolChanged);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 27.0f, sm.coolSetpoint());
}

static void test_deadband_push_clamped_at_range_rail() {
  ModeStateMachine sm;
  auto r = sm.setHeatSetpoint(39.0f);  // push would put cool at 41.8 > 40
  TEST_ASSERT_TRUE(r.accepted);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 40.0f, r.coolC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 40.0f - dettson::kMinSetpointDeltaC, r.heatC);
}

static void test_invalid_setpoint_writes_rejected() {
  ModeStateMachine sm;
  const float h = sm.heatSetpoint(), c = sm.coolSetpoint();
  auto r1 = sm.setHeatSetpoint(NAN);
  auto r2 = sm.setHeatSetpoint(50.0f);
  auto r3 = sm.setCoolSetpoint(-5.0f);
  TEST_ASSERT_FALSE(r1.accepted);
  TEST_ASSERT_FALSE(r2.accepted);
  TEST_ASSERT_FALSE(r3.accepted);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, h, sm.heatSetpoint());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, c, sm.coolSetpoint());
}

static void test_min_delta_clamped_to_hard_floor() {
  ModeStateMachine sm;
  float applied = sm.setMinSetpointDelta(0.5f);  // below 1.1 floor
  TEST_ASSERT_FLOAT_WITHIN(0.001f, dettson::kMinSetpointDeltaFloorC, applied);
  // With the floor delta, setpoints may sit 1.1 apart but no closer.
  sm.setHeatSetpoint(21.0f);
  auto r = sm.setCoolSetpoint(21.5f);
  TEST_ASSERT_TRUE(r.accepted);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.5f, r.coolC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.5f - dettson::kMinSetpointDeltaFloorC, r.heatC);
}

static void test_widening_delta_reenforces_pair() {
  ModeStateMachine sm;
  sm.setMinSetpointDelta(1.1f);
  sm.setHeatSetpoint(22.0f);
  sm.setCoolSetpoint(23.1f);
  sm.setMinSetpointDelta(5.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 22.0f, sm.heatSetpoint());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 27.0f, sm.coolSetpoint());
}

// ---------- hysteresis ----------

static void test_heat_hysteresis_no_chatter() {
  ModeStateMachine sm;
  sm.setMode(UserMode::kHeat, 0);
  sm.setHeatSetpoint(20.0f);  // enter <= 19.45, exit >= 20.0
  TEST_ASSERT_EQUAL(CallType::kNone, sm.update(19.6f, true, 10).type);
  Call c = sm.update(19.4f, true, 20);
  TEST_ASSERT_EQUAL(CallType::kHeat, c.type);
  TEST_ASSERT_FALSE(c.gasOnly);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.6f, c.errorC);
  // Inside the band the call must hold (no chatter)...
  TEST_ASSERT_EQUAL(CallType::kHeat, sm.update(19.7f, true, 30).type);
  TEST_ASSERT_EQUAL(CallType::kHeat, sm.update(19.9f, true, 40).type);
  // ...exit only at setpoint...
  TEST_ASSERT_EQUAL(CallType::kNone, sm.update(20.0f, true, 50).type);
  // ...and re-enter only past the full differential.
  TEST_ASSERT_EQUAL(CallType::kNone, sm.update(19.5f, true, 60).type);
  TEST_ASSERT_EQUAL(CallType::kHeat, sm.update(19.44f, true, 70).type);
}

static void test_cool_hysteresis_no_chatter() {
  ModeStateMachine sm;
  sm.setMode(UserMode::kCool, 0);
  sm.setCoolSetpoint(24.0f);  // enter >= 24.55, exit <= 24.0
  TEST_ASSERT_EQUAL(CallType::kNone, sm.update(24.4f, true, 10).type);
  Call c = sm.update(24.6f, true, 20);
  TEST_ASSERT_EQUAL(CallType::kCool, c.type);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.6f, c.errorC);
  TEST_ASSERT_EQUAL(CallType::kCool, sm.update(24.2f, true, 30).type);
  TEST_ASSERT_EQUAL(CallType::kNone, sm.update(24.0f, true, 40).type);
  TEST_ASSERT_EQUAL(CallType::kNone, sm.update(24.4f, true, 50).type);
}

// ---------- AUTO changeover ----------

static ModeStateMachine makeAutoSm() {
  ModeStateMachine sm;
  sm.setHeatSetpoint(20.0f);
  sm.setCoolSetpoint(24.0f);
  sm.setMode(UserMode::kAuto, 0);
  return sm;
}

static void test_auto_first_call_immediate() {
  ModeStateMachine sm = makeAutoSm();
  // No prior opposite call -> hysteresis alone starts the call.
  TEST_ASSERT_EQUAL(CallType::kHeat, sm.update(19.0f, true, 10).type);
}

static void test_auto_same_direction_recall_not_gated() {
  ModeStateMachine sm = makeAutoSm();
  sm.update(19.0f, true, 10);                                   // heat call
  TEST_ASSERT_EQUAL(CallType::kNone, sm.update(20.0f, true, 100).type);  // satisfied
  // Heat again right away: not a changeover, no dwell/sustain needed.
  TEST_ASSERT_EQUAL(CallType::kHeat, sm.update(19.4f, true, 110).type);
}

static void test_auto_changeover_requires_sustain_and_dwell() {
  ModeStateMachine sm = makeAutoSm();
  sm.update(19.0f, true, 10);                  // heat call
  sm.update(21.0f, true, 100);                 // heat ends at t=100
  // Cool trigger appears at t=110 and persists.
  TEST_ASSERT_EQUAL(CallType::kNone, sm.update(24.6f, true, 110).type);
  // Sustain met (610 s >= 600) but dwell since heat end is 610 < 1800.
  TEST_ASSERT_EQUAL(CallType::kNone, sm.update(24.6f, true, 720).type);
  // Both met at t=2000 (sustain 1890, dwell 1900).
  TEST_ASSERT_EQUAL(CallType::kCool, sm.update(24.6f, true, 2000).type);
}

static void test_auto_changeover_sustain_resets_on_blip() {
  ModeStateMachine sm = makeAutoSm();
  sm.update(19.0f, true, 10);
  sm.update(21.0f, true, 100);                  // heat ends t=100
  sm.update(24.6f, true, 200);                  // trigger armed t=200
  sm.update(23.9f, true, 700);                  // blip below trigger -> disarm
  sm.update(24.6f, true, 800);                  // re-armed t=800
  // t=1300: 1100 s since first trigger, but only 500 since re-arm.
  TEST_ASSERT_EQUAL(CallType::kNone, sm.update(24.6f, true, 1300).type);
  // t=2100: sustain 1300 >= 600, dwell 2000 >= 1800.
  TEST_ASSERT_EQUAL(CallType::kCool, sm.update(24.6f, true, 2100).type);
}

static void test_auto_changeover_blocked_by_compressor_guard() {
  ModeStateMachine sm = makeAutoSm();
  sm.update(19.0f, true, 10);
  sm.update(21.0f, true, 100);
  sm.update(24.6f, true, 110);
  // Gates met, but the guard says no.
  TEST_ASSERT_EQUAL(CallType::kNone, sm.update(24.6f, true, 2000, false).type);
  // Guard clears -> swap proceeds (sustain clock survived the guard denial).
  TEST_ASSERT_EQUAL(CallType::kCool, sm.update(24.6f, true, 2010, true).type);
}

static void test_auto_changeover_cool_to_heat_gated_too() {
  ModeStateMachine sm = makeAutoSm();
  sm.update(24.6f, true, 10);                   // first call: cool, immediate
  TEST_ASSERT_EQUAL(CallType::kCool, sm.call().type);
  sm.update(24.0f, true, 100);                  // cool ends t=100
  TEST_ASSERT_EQUAL(CallType::kNone, sm.update(19.0f, true, 110).type);
  TEST_ASSERT_EQUAL(CallType::kNone, sm.update(19.0f, true, 720).type);   // dwell unmet
  TEST_ASSERT_EQUAL(CallType::kHeat, sm.update(19.0f, true, 2000).type);
}

// ---------- EMERGENCY_HEAT ----------

static void test_emergency_heat_is_gas_only() {
  ModeStateMachine sm;
  sm.setHeatSetpoint(20.0f);
  sm.setMode(UserMode::kEmergencyHeat, 0);
  Call c = sm.update(18.0f, true, 10);
  TEST_ASSERT_EQUAL(CallType::kHeat, c.type);
  TEST_ASSERT_TRUE(c.gasOnly);  // compressor never requested in EM-HEAT
  // Stays gas-only for the life of the call.
  c = sm.update(19.0f, true, 20);
  TEST_ASSERT_TRUE(c.gasOnly);
  // Normal HEAT mode does not set the flag.
  sm.setMode(UserMode::kHeat, 30);
  c = sm.update(18.0f, true, 40);
  TEST_ASSERT_EQUAL(CallType::kHeat, c.type);
  TEST_ASSERT_FALSE(c.gasOnly);
}

// ---------- invalid input -> fail to no-demand ----------

static void test_invalid_temp_drops_call_and_alarms() {
  ModeStateMachine sm;
  sm.setHeatSetpoint(20.0f);
  sm.setMode(UserMode::kHeat, 0);
  TEST_ASSERT_EQUAL(CallType::kHeat, sm.update(18.0f, true, 10).type);
  Call c = sm.update(18.0f, false, 20);  // fused temp invalid
  TEST_ASSERT_EQUAL(CallType::kNone, c.type);
  TEST_ASSERT_TRUE(sm.inputFaultAlarm());
  // NaN with tempValid=true is equally invalid.
  c = sm.update(NAN, true, 30);
  TEST_ASSERT_EQUAL(CallType::kNone, c.type);
  TEST_ASSERT_TRUE(sm.inputFaultAlarm());
  // Recovery: valid input again -> alarm clears, call re-enters by hysteresis.
  c = sm.update(18.0f, true, 40);
  TEST_ASSERT_EQUAL(CallType::kHeat, c.type);
  TEST_ASSERT_FALSE(sm.inputFaultAlarm());
}

static void test_invalid_temp_resets_auto_sustain() {
  ModeStateMachine sm = makeAutoSm();
  sm.update(19.0f, true, 10);
  sm.update(21.0f, true, 100);                  // heat ends t=100
  sm.update(24.6f, true, 200);                  // cool trigger armed t=200
  sm.update(24.6f, false, 1500);                // fault mid-sustain
  TEST_ASSERT_TRUE(sm.inputFaultAlarm());
  // Recovery re-arms at t=1900: stale sustain credit must not carry over.
  TEST_ASSERT_EQUAL(CallType::kNone, sm.update(24.6f, true, 1900).type);
  TEST_ASSERT_EQUAL(CallType::kNone, sm.update(24.6f, true, 2400).type);  // 500 s < 600
  TEST_ASSERT_EQUAL(CallType::kCool, sm.update(24.6f, true, 2500).type);
}

static void test_mode_change_and_off_drop_call() {
  ModeStateMachine sm;
  sm.setHeatSetpoint(20.0f);
  sm.setMode(UserMode::kHeat, 0);
  TEST_ASSERT_EQUAL(CallType::kHeat, sm.update(18.0f, true, 10).type);
  sm.setMode(UserMode::kOff, 20);
  TEST_ASSERT_EQUAL(CallType::kNone, sm.call().type);  // ended immediately
  TEST_ASSERT_EQUAL(CallType::kNone, sm.update(18.0f, true, 30).type);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_boot_state_no_demand);
  RUN_TEST(test_deadband_cool_write_pushes_heat_down);
  RUN_TEST(test_deadband_heat_write_pushes_cool_up);
  RUN_TEST(test_deadband_nonviolating_write_pushes_nothing);
  RUN_TEST(test_deadband_push_clamped_at_range_rail);
  RUN_TEST(test_invalid_setpoint_writes_rejected);
  RUN_TEST(test_min_delta_clamped_to_hard_floor);
  RUN_TEST(test_widening_delta_reenforces_pair);
  RUN_TEST(test_heat_hysteresis_no_chatter);
  RUN_TEST(test_cool_hysteresis_no_chatter);
  RUN_TEST(test_auto_first_call_immediate);
  RUN_TEST(test_auto_same_direction_recall_not_gated);
  RUN_TEST(test_auto_changeover_requires_sustain_and_dwell);
  RUN_TEST(test_auto_changeover_sustain_resets_on_blip);
  RUN_TEST(test_auto_changeover_blocked_by_compressor_guard);
  RUN_TEST(test_auto_changeover_cool_to_heat_gated_too);
  RUN_TEST(test_emergency_heat_is_gas_only);
  RUN_TEST(test_invalid_temp_drops_call_and_alarms);
  RUN_TEST(test_invalid_temp_resets_auto_sustain);
  RUN_TEST(test_mode_change_and_off_drop_call);
  return UNITY_END();
}
