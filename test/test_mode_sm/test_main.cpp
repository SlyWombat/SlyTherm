// ModeStateMachine unit tests: deadband push (Ecobee-style), call
// hysteresis no-chatter, AUTO changeover sustain+dwell+compressor gates,
// EMERGENCY_HEAT gas-only, invalid-input fail-to-no-demand (docs/04 §2),
// preset roster + hold lifecycles (docs/07 gap G4).
#include <unity.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "DettsonConfig.h"
#include "ModeStateMachine.h"

using dettson::Call;
using dettson::CallType;
using dettson::HoldType;
using dettson::ModeStateMachine;
using dettson::PresetDef;
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

// ---------- preset roster (G4) ----------

static PresetDef mkPreset(const char* name, float heatC, float coolC) {
  PresetDef d{};
  std::strncpy(d.name, name, sizeof d.name - 1);
  d.heatC = heatC;
  d.coolC = coolC;
  return d;
}

static ModeStateMachine makeRosterSm() {
  ModeStateMachine sm;
  PresetDef defs[] = {
      mkPreset("home", 21.0f, 25.0f),
      mkPreset("away", 16.0f, 28.0f),
      mkPreset("sleep", 19.0f, 24.0f),
  };
  TEST_ASSERT_EQUAL(3, sm.setPresetRoster(defs, 3));
  return sm;
}

static void test_preset_apply_sets_pair_and_active_name() {
  ModeStateMachine sm = makeRosterSm();
  auto r = sm.applyPreset("home", 100);
  TEST_ASSERT_TRUE(r.applied);
  TEST_ASSERT_FALSE(r.blockedByHold);
  TEST_ASSERT_TRUE(r.setpoints.accepted);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f, sm.heatSetpoint());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, sm.coolSetpoint());
  TEST_ASSERT_EQUAL_STRING("home", sm.activePreset());
  // A preset application is not a manual change: no hold created.
  TEST_ASSERT_EQUAL(HoldType::kNone, sm.activeHoldType());
}

static void test_preset_unknown_name_rejected() {
  ModeStateMachine sm = makeRosterSm();
  const float h = sm.heatSetpoint(), c = sm.coolSetpoint();
  auto r = sm.applyPreset("vacation", 100);
  TEST_ASSERT_FALSE(r.applied);
  TEST_ASSERT_FALSE(r.blockedByHold);
  TEST_ASSERT_FALSE(sm.applyPreset(nullptr, 100).applied);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, h, sm.heatSetpoint());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, c, sm.coolSetpoint());
  TEST_ASSERT_EQUAL_STRING("", sm.activePreset());
}

static void test_preset_apply_honors_deadband_cool_wins() {
  ModeStateMachine sm = makeRosterSm();
  PresetDef defs[] = {mkPreset("tight", 24.0f, 25.0f)};  // violates 2.8 delta
  sm.setPresetRoster(defs, 1);
  auto r = sm.applyPreset("tight", 50);
  TEST_ASSERT_TRUE(r.applied);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, sm.coolSetpoint());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f - dettson::kMinSetpointDeltaC,
                           sm.heatSetpoint());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, sm.heatSetpoint(), r.setpoints.heatC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, sm.coolSetpoint(), r.setpoints.coolC);
}

static void test_roster_skips_invalid_and_duplicate_entries_and_caps() {
  ModeStateMachine sm;
  PresetDef defs[12];
  defs[0] = mkPreset("", 20.0f, 25.0f);          // empty name
  defs[1] = mkPreset("nanheat", NAN, 25.0f);     // NaN setpoint
  defs[2] = mkPreset("hot", 20.0f, 50.0f);       // out of [5,40]
  defs[3] = mkPreset("home", 21.0f, 25.0f);
  defs[4] = mkPreset("home", 18.0f, 26.0f);      // duplicate name
  for (int i = 5; i < 12; ++i) {
    char name[8];
    std::snprintf(name, sizeof name, "p%d", i);
    defs[i] = mkPreset(name, 20.0f, 25.0f);
  }
  // Valid: home + p5..p11 = 8 -> exactly at the cap.
  TEST_ASSERT_EQUAL(dettson::kMaxPresets, sm.setPresetRoster(defs, 12));
  TEST_ASSERT_NOT_NULL(sm.presetByName("home"));
  TEST_ASSERT_NULL(sm.presetByName(""));
  TEST_ASSERT_NULL(sm.presetByName("hot"));
  // Duplicate kept the first definition.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f, sm.presetByName("home")->heatC);
}

static void test_roster_replace_clears_missing_active_preset() {
  ModeStateMachine sm = makeRosterSm();
  sm.applyPreset("home", 10);
  PresetDef defs[] = {mkPreset("eco", 17.0f, 28.0f)};
  sm.setPresetRoster(defs, 1);
  TEST_ASSERT_EQUAL_STRING("", sm.activePreset());     // name no longer exists
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f, sm.heatSetpoint());  // setpoints kept
}

// ---------- EM HEAT toggle (docs/07 G15: the HA switch path) ----------

static void test_em_heat_toggle_engages_and_restores_prior_mode() {
  ModeStateMachine sm;
  sm.setHeatSetpoint(20.0f);
  sm.setMode(UserMode::kHeat, 0);
  sm.setEmergencyHeat(true, 10);
  TEST_ASSERT_TRUE(sm.emergencyHeat());
  TEST_ASSERT_EQUAL(UserMode::kEmergencyHeat, sm.mode());
  Call c = sm.update(18.0f, true, 20);
  TEST_ASSERT_EQUAL(CallType::kHeat, c.type);
  TEST_ASSERT_TRUE(c.gasOnly);
  sm.setEmergencyHeat(false, 30);
  TEST_ASSERT_FALSE(sm.emergencyHeat());
  TEST_ASSERT_EQUAL(UserMode::kHeat, sm.mode());  // prior mode restored
}

static void test_em_heat_toggle_off_when_not_engaged_is_noop() {
  ModeStateMachine sm;
  sm.setMode(UserMode::kCool, 0);
  sm.setEmergencyHeat(false, 10);
  TEST_ASSERT_EQUAL(UserMode::kCool, sm.mode());
}

static void test_em_heat_direct_setmode_then_toggle_off_restores() {
  ModeStateMachine sm;
  sm.setMode(UserMode::kAuto, 0);
  sm.setMode(UserMode::kEmergencyHeat, 10);  // wall-UI path
  TEST_ASSERT_TRUE(sm.emergencyHeat());
  sm.setEmergencyHeat(false, 20);            // HA switch OFF
  TEST_ASSERT_EQUAL(UserMode::kAuto, sm.mode());
}

static void test_em_heat_coexists_with_comfort_presets() {
  // Mutual exclusion is structural: EM HEAT pins the equipment choice while
  // comfort presets keep managing setpoints — a preset never disengages it
  // and engaging it never clears the active preset's setpoints.
  ModeStateMachine sm = makeRosterSm();
  sm.setMode(UserMode::kHeat, 0);
  sm.setEmergencyHeat(true, 10);
  auto r = sm.applyPreset("home", 20);  // ends the until-next-preset hold
  TEST_ASSERT_TRUE(r.applied);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f, sm.heatSetpoint());
  TEST_ASSERT_TRUE(sm.emergencyHeat());  // mode untouched by the preset
  TEST_ASSERT_EQUAL_STRING("home", sm.activePreset());
  Call c = sm.update(18.0f, true, 30);
  TEST_ASSERT_TRUE(c.gasOnly);
}

// ---------- holds (G4 Ecobee semantics) ----------

static void test_manual_setpoint_creates_four_hour_hold() {
  // #91: an on-device manual setpoint change always creates a 4 h hold (the
  // Home pill counts it down) — no schedule exists on-device, so the old
  // until-next-preset default made manual changes effectively open-ended.
  ModeStateMachine sm = makeRosterSm();
  sm.applyPreset("home", 100);
  auto r = sm.setHeatSetpoint(19.0f, 200);  // manual overload
  TEST_ASSERT_TRUE(r.accepted);
  TEST_ASSERT_EQUAL(HoldType::kFourHours, sm.activeHoldType());
  TEST_ASSERT_EQUAL_STRING("", sm.activePreset());  // manual change clears it
  TEST_ASSERT_EQUAL_UINT32(dettson::kHoldLongS, sm.holdRemainingS(200));
  // Presets are blocked while the 4 h clock runs...
  TEST_ASSERT_TRUE(sm.applyPreset("sleep", 300).blockedByHold);
  // ...and apply again once it expires (expiry applied inside applyPreset).
  auto pr = sm.applyPreset("sleep", 200 + dettson::kHoldLongS);
  TEST_ASSERT_TRUE(pr.applied);
  TEST_ASSERT_EQUAL(HoldType::kNone, sm.activeHoldType());
  TEST_ASSERT_EQUAL_STRING("sleep", sm.activePreset());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 19.0f, sm.heatSetpoint());
}

static void test_unknown_preset_does_not_end_hold() {
  ModeStateMachine sm = makeRosterSm();
  sm.setCoolSetpoint(26.0f, 100);  // manual change -> 4 h hold (#91)
  TEST_ASSERT_EQUAL(HoldType::kFourHours, sm.activeHoldType());
  sm.applyPreset("bogus", 200);
  TEST_ASSERT_EQUAL(HoldType::kFourHours, sm.activeHoldType());
}

static void test_mode_change_creates_hold() {
  ModeStateMachine sm = makeRosterSm();
  sm.applyPreset("home", 50);
  sm.setMode(UserMode::kHeat, 100);
  TEST_ASSERT_EQUAL(HoldType::kUntilNextPreset, sm.activeHoldType());
  sm.setMode(UserMode::kHeat, 200);  // no-op: same mode keeps state
  TEST_ASSERT_EQUAL(HoldType::kUntilNextPreset, sm.activeHoldType());
}

static void test_two_hour_hold_blocks_presets_then_expires_mid_tick() {
  ModeStateMachine sm;
  PresetDef defs[] = {mkPreset("home", 21.0f, 25.0f)};
  sm.setPresetRoster(defs, 1);
  sm.setHeatSetpoint(19.0f);                  // time-less write: no hold created
  sm.startHold(HoldType::kTwoHours, 1000);    // #81 chooser picks 2 h
  TEST_ASSERT_EQUAL(HoldType::kTwoHours, sm.activeHoldType());
  TEST_ASSERT_EQUAL_UINT32(dettson::kHoldShortS, sm.holdRemainingS(1000));
  TEST_ASSERT_EQUAL_UINT32(3600, sm.holdRemainingS(4600));
  // Blocked while the clock runs; the blocked preset changes nothing.
  auto r = sm.applyPreset("home", 5000);
  TEST_ASSERT_FALSE(r.applied);
  TEST_ASSERT_TRUE(r.blockedByHold);
  TEST_ASSERT_EQUAL_STRING("", sm.activePreset());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 19.0f, sm.heatSetpoint());
  // One second before the deadline (1000 + 7200): still blocked.
  TEST_ASSERT_TRUE(sm.applyPreset("home", 8199).blockedByHold);
  // Expiry lands mid-tick: a plain update() past the deadline clears it.
  sm.update(21.0f, true, 8205);
  TEST_ASSERT_EQUAL(HoldType::kNone, sm.activeHoldType());
  TEST_ASSERT_EQUAL_UINT32(0, sm.holdRemainingS(8205));
  TEST_ASSERT_TRUE(sm.applyPreset("home", 8210).applied);
}

static void test_four_hour_hold_expires_inside_apply_preset() {
  ModeStateMachine sm = makeRosterSm();
  sm.startHold(HoldType::kFourHours, 1000);
  TEST_ASSERT_EQUAL_UINT32(dettson::kHoldLongS, sm.holdRemainingS(1000));
  TEST_ASSERT_TRUE(sm.applyPreset("home", 15399).blockedByHold);  // 14399 s in
  // applyPreset itself applies expiry — no update() needed in between.
  TEST_ASSERT_TRUE(sm.applyPreset("home", 15400).applied);
}

static void test_indefinite_hold_ends_only_on_clear() {
  ModeStateMachine sm = makeRosterSm();
  sm.startHold(HoldType::kIndefinite, 0);
  TEST_ASSERT_EQUAL_UINT32(0, sm.holdRemainingS(0));
  TEST_ASSERT_TRUE(sm.applyPreset("home", 1000000).blockedByHold);
  sm.update(21.0f, true, 2000000);  // no clock expiry
  TEST_ASSERT_EQUAL(HoldType::kIndefinite, sm.activeHoldType());
  sm.clearHold();
  TEST_ASSERT_EQUAL(HoldType::kNone, sm.activeHoldType());
  TEST_ASSERT_TRUE(sm.applyPreset("home", 2000010).applied);
}

static void test_start_hold_none_clears() {
  ModeStateMachine sm = makeRosterSm();
  sm.startHold(HoldType::kTwoHours, 0);
  sm.startHold(HoldType::kNone, 10);
  TEST_ASSERT_EQUAL(HoldType::kNone, sm.activeHoldType());
}

static void test_default_hold_type_none_gates_mode_changes_only() {
  // #91: defaultHoldType governs MODE changes; manual SETPOINT changes always
  // take the fixed 4 h hold regardless (no on-device schedule to fall back to).
  ModeStateMachine::Config cfg;
  cfg.defaultHoldType = HoldType::kNone;
  ModeStateMachine sm(cfg);
  PresetDef defs[] = {mkPreset("home", 21.0f, 25.0f)};
  sm.setPresetRoster(defs, 1);
  sm.setMode(UserMode::kHeat, 50);
  TEST_ASSERT_EQUAL(HoldType::kNone, sm.activeHoldType());  // mode change held nothing
  TEST_ASSERT_TRUE(sm.applyPreset("home", 60).applied);
  sm.setHeatSetpoint(19.0f, 100);
  TEST_ASSERT_EQUAL(HoldType::kFourHours, sm.activeHoldType());
}

// ---------- hold persistence across reboot/OTA (#151) ----------

static void test_hold_blob_none_restores_nothing() {
  ModeStateMachine a;  // no hold active
  ModeStateMachine::HoldPersistBlob b;
  a.saveHold(&b, 100, 0);
  ModeStateMachine sm;
  TEST_ASSERT_FALSE(sm.restoreHold(b, 5000));
  TEST_ASSERT_EQUAL(HoldType::kNone, sm.activeHoldType());
}

static void test_hold_blob_roundtrip_indefinite() {
  ModeStateMachine a = makeRosterSm();
  a.startHold(HoldType::kIndefinite, 100);
  ModeStateMachine::HoldPersistBlob b;
  a.saveHold(&b, 200, 1700000000u);
  ModeStateMachine sm = makeRosterSm();
  TEST_ASSERT_TRUE(sm.restoreHold(b, 999999));  // long outage: indefinite is as-is
  TEST_ASSERT_EQUAL(HoldType::kIndefinite, sm.activeHoldType());
  TEST_ASSERT_TRUE(sm.applyPreset("home", 1000000).blockedByHold);
}

static void test_hold_blob_until_next_preset_restores_and_ends_on_preset() {
  ModeStateMachine a = makeRosterSm();
  a.startHold(HoldType::kUntilNextPreset, 100);
  ModeStateMachine::HoldPersistBlob b;
  a.saveHold(&b, 100, 0);
  ModeStateMachine sm = makeRosterSm();
  TEST_ASSERT_TRUE(sm.restoreHold(b, 500));
  TEST_ASSERT_EQUAL(HoldType::kUntilNextPreset, sm.activeHoldType());
  TEST_ASSERT_TRUE(sm.applyPreset("home", 600).applied);  // preset still ends it
  TEST_ASSERT_EQUAL(HoldType::kNone, sm.activeHoldType());
}

static void test_hold_blob_timed_decrements_by_monotonic_elapsed() {
  ModeStateMachine a;
  a.startHold(HoldType::kFourHours, 1000);        // ends at 15400
  ModeStateMachine::HoldPersistBlob b;
  a.saveHold(&b, 2000, 0);                        // remain 13400 at save
  ModeStateMachine sm;
  TEST_ASSERT_TRUE(sm.restoreHold(b, 2500));      // clock resumed 500 s past save
  TEST_ASSERT_EQUAL(HoldType::kFourHours, sm.activeHoldType());
  TEST_ASSERT_EQUAL_UINT32(12900, sm.holdRemainingS(2500));
}

static void test_hold_blob_timed_expired_during_outage() {
  ModeStateMachine a;
  a.startHold(HoldType::kTwoHours, 0);            // remain 7200
  ModeStateMachine::HoldPersistBlob b;
  a.saveHold(&b, 0, 0);
  ModeStateMachine sm;
  TEST_ASSERT_FALSE(sm.restoreHold(b, 7200));     // served out exactly
  TEST_ASSERT_EQUAL(HoldType::kNone, sm.activeHoldType());
}

static void test_hold_blob_clock_backwards_resumes_full_remainder() {
  // The monotonic base persists every 60 s, so a reboot can resume BEHIND
  // the save stamp: elapsed clamps to zero — full saved remainder, never an
  // underflowed near-infinite hold.
  ModeStateMachine a;
  a.startHold(HoldType::kTwoHours, 1000);
  ModeStateMachine::HoldPersistBlob b;
  a.saveHold(&b, 1000, 0);                        // remain 7200 saved at 1000
  ModeStateMachine sm;
  TEST_ASSERT_TRUE(sm.restoreHold(b, 950));       // resumed 50 s behind
  TEST_ASSERT_EQUAL_UINT32(7200, sm.holdRemainingS(950));
}

static void test_hold_blob_corrupt_rejected() {
  ModeStateMachine a;
  a.startHold(HoldType::kIndefinite, 0);
  ModeStateMachine::HoldPersistBlob good;
  a.saveHold(&good, 0, 0);

  ModeStateMachine sm;
  ModeStateMachine::HoldPersistBlob b = good;
  // Payload flip -> stale crc. (Not 0xFF: Fletcher mod 255 can't see a
  // 0x00<->0xFF byte swap — the CompressorGuard blob shares this property.)
  b.remainS ^= 0x5Au;
  TEST_ASSERT_FALSE(sm.restoreHold(b, 100));
  b = good;
  b.type = 200;                                   // out-of-range type -> stale crc
  TEST_ASSERT_FALSE(sm.restoreHold(b, 100));
  b = good;
  b.magic = 0xDEADBEEFu;
  TEST_ASSERT_FALSE(sm.restoreHold(b, 100));
  b = good;
  b.version = static_cast<uint16_t>(ModeStateMachine::kHoldBlobVersion + 1);
  TEST_ASSERT_FALSE(sm.restoreHold(b, 100));
  TEST_ASSERT_EQUAL(HoldType::kNone, sm.activeHoldType());  // fail open = no hold
  TEST_ASSERT_TRUE(sm.restoreHold(good, 100));    // the pristine blob still works
  TEST_ASSERT_EQUAL(HoldType::kIndefinite, sm.activeHoldType());
}

static void test_hold_epoch_correction_charges_outage() {
  ModeStateMachine a;
  a.startHold(HoldType::kTwoHours, 1000);         // remain 7200
  ModeStateMachine::HoldPersistBlob b;
  a.saveHold(&b, 1000, 1700000000u);
  ModeStateMachine sm;
  TEST_ASSERT_TRUE(sm.restoreHold(b, 1005));      // mono saw only 5 s of the outage
  TEST_ASSERT_EQUAL_UINT32(7195, sm.holdRemainingS(1005));
  // NTP syncs: 3600 s of wall clock actually passed since the save.
  TEST_ASSERT_TRUE(sm.applyHoldEpochCorrection(1700000000u + 3600u, 1005));
  TEST_ASSERT_EQUAL_UINT32(3600, sm.holdRemainingS(1005));
  // One-shot: a later sync changes nothing.
  TEST_ASSERT_FALSE(sm.applyHoldEpochCorrection(1700000000u + 7000u, 1005));
  TEST_ASSERT_EQUAL_UINT32(3600, sm.holdRemainingS(1005));
}

static void test_hold_epoch_correction_expires_hold() {
  ModeStateMachine a;
  a.startHold(HoldType::kTwoHours, 0);
  ModeStateMachine::HoldPersistBlob b;
  a.saveHold(&b, 0, 1700000000u);
  ModeStateMachine sm;
  TEST_ASSERT_TRUE(sm.restoreHold(b, 10));
  TEST_ASSERT_TRUE(sm.applyHoldEpochCorrection(1700000000u + 8000u, 10));
  TEST_ASSERT_EQUAL(HoldType::kNone, sm.activeHoldType());
}

static void test_hold_epoch_correction_never_extends() {
  ModeStateMachine a;
  a.startHold(HoldType::kTwoHours, 1000);
  ModeStateMachine::HoldPersistBlob b;
  a.saveHold(&b, 1000, 1700000000u);              // remain 7200
  ModeStateMachine sm;
  TEST_ASSERT_TRUE(sm.restoreHold(b, 2000));      // mono charged 1000 s -> 6200
  // Skewed NTP claims only 500 s passed: 6700 > 6200 would EXTEND — refused.
  TEST_ASSERT_FALSE(sm.applyHoldEpochCorrection(1700000000u + 500u, 2000));
  TEST_ASSERT_EQUAL_UINT32(6200, sm.holdRemainingS(2000));
}

static void test_hold_epoch_correction_cancelled_by_user_change() {
  ModeStateMachine a;
  a.startHold(HoldType::kTwoHours, 0);
  ModeStateMachine::HoldPersistBlob b;
  a.saveHold(&b, 0, 1700000000u);
  ModeStateMachine sm;
  TEST_ASSERT_TRUE(sm.restoreHold(b, 5));
  sm.startHold(HoldType::kIndefinite, 10);        // user re-decided post-boot
  TEST_ASSERT_FALSE(sm.applyHoldEpochCorrection(1700000000u + 9000u, 10));
  TEST_ASSERT_EQUAL(HoldType::kIndefinite, sm.activeHoldType());
}

static void test_hold_epoch_correction_needs_saved_epoch() {
  ModeStateMachine a;
  a.startHold(HoldType::kTwoHours, 0);
  ModeStateMachine::HoldPersistBlob b;
  a.saveHold(&b, 0, 0);                           // clock was unsynced at save
  ModeStateMachine sm;
  TEST_ASSERT_TRUE(sm.restoreHold(b, 100));
  TEST_ASSERT_FALSE(sm.applyHoldEpochCorrection(1700000000u, 100));
  TEST_ASSERT_EQUAL_UINT32(7100, sm.holdRemainingS(100));  // mono resume kept
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
  RUN_TEST(test_em_heat_toggle_engages_and_restores_prior_mode);
  RUN_TEST(test_em_heat_toggle_off_when_not_engaged_is_noop);
  RUN_TEST(test_em_heat_direct_setmode_then_toggle_off_restores);
  RUN_TEST(test_em_heat_coexists_with_comfort_presets);
  RUN_TEST(test_invalid_temp_drops_call_and_alarms);
  RUN_TEST(test_invalid_temp_resets_auto_sustain);
  RUN_TEST(test_mode_change_and_off_drop_call);
  RUN_TEST(test_preset_apply_sets_pair_and_active_name);
  RUN_TEST(test_preset_unknown_name_rejected);
  RUN_TEST(test_preset_apply_honors_deadband_cool_wins);
  RUN_TEST(test_roster_skips_invalid_and_duplicate_entries_and_caps);
  RUN_TEST(test_roster_replace_clears_missing_active_preset);
  RUN_TEST(test_manual_setpoint_creates_four_hour_hold);
  RUN_TEST(test_unknown_preset_does_not_end_hold);
  RUN_TEST(test_mode_change_creates_hold);
  RUN_TEST(test_two_hour_hold_blocks_presets_then_expires_mid_tick);
  RUN_TEST(test_four_hour_hold_expires_inside_apply_preset);
  RUN_TEST(test_indefinite_hold_ends_only_on_clear);
  RUN_TEST(test_start_hold_none_clears);
  RUN_TEST(test_default_hold_type_none_gates_mode_changes_only);
  RUN_TEST(test_hold_blob_none_restores_nothing);
  RUN_TEST(test_hold_blob_roundtrip_indefinite);
  RUN_TEST(test_hold_blob_until_next_preset_restores_and_ends_on_preset);
  RUN_TEST(test_hold_blob_timed_decrements_by_monotonic_elapsed);
  RUN_TEST(test_hold_blob_timed_expired_during_outage);
  RUN_TEST(test_hold_blob_clock_backwards_resumes_full_remainder);
  RUN_TEST(test_hold_blob_corrupt_rejected);
  RUN_TEST(test_hold_epoch_correction_charges_outage);
  RUN_TEST(test_hold_epoch_correction_expires_hold);
  RUN_TEST(test_hold_epoch_correction_never_extends);
  RUN_TEST(test_hold_epoch_correction_cancelled_by_user_change);
  RUN_TEST(test_hold_epoch_correction_needs_saved_epoch);
  return UNITY_END();
}
