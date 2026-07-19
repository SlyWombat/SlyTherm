// UiModel tests: deadband clamp+push parity with ModeStateMachine's rule,
// bounded intent queue (drop-oldest + flag), dirty-flag correctness, alarm
// list bounding, and invalid-input rejection (docs/04 §1c UI isolation).
#include <unity.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "DettsonConfig.h"
#include "UiModel.h"

using namespace dettson;
using namespace dettson::ui;

void setUp() {}
void tearDown() {}

static UiModel freshModel() {
  UiModel m;
  m.clearDirty();
  return m;
}

// ---------- boot state ----------

static void test_boot_state_is_safe_and_fully_dirty() {
  UiModel m;
  const DisplayState& s = m.state();
  TEST_ASSERT_EQUAL(static_cast<int>(UserMode::kOff), static_cast<int>(s.mode));
  TEST_ASSERT_EQUAL(static_cast<int>(HvacAction::kIdle), static_cast<int>(s.action));
  TEST_ASSERT_EQUAL_UINT8(kEquipNone, s.activeEquipment);
  TEST_ASSERT_FALSE(s.fusedTempValid);
  TEST_ASSERT_EQUAL_FLOAT(kFallbackHeatSetpointC, s.heatSetpointC);
  TEST_ASSERT_EQUAL_FLOAT(kFallbackCoolSetpointC, s.coolSetpointC);
  TEST_ASSERT_EQUAL_UINT16(kDirtyAll, m.dirty());  // first render draws all
  UiIntent dummy;
  TEST_ASSERT_FALSE(m.popIntent(dummy));           // boot = no pending intents
}

// ---------- deadband clamp+push (pure function parity) ----------

static void test_deadband_push_cool_up_when_heat_moves() {
  // heat raised into the band: cool gets pushed (Ecobee-style), heat wins.
  Setpoints sp = applyDeadbandClampPush({22.0f, 23.0f}, SetpointSide::kHeat,
                                        kMinSetpointDeltaC);
  TEST_ASSERT_EQUAL_FLOAT(22.0f, sp.heatC);
  TEST_ASSERT_EQUAL_FLOAT(22.0f + kMinSetpointDeltaC, sp.coolC);
}

static void test_deadband_push_heat_down_when_cool_moves() {
  Setpoints sp = applyDeadbandClampPush({21.0f, 21.5f}, SetpointSide::kCool,
                                        kMinSetpointDeltaC);
  TEST_ASSERT_EQUAL_FLOAT(21.5f, sp.coolC);
  TEST_ASSERT_EQUAL_FLOAT(21.5f - kMinSetpointDeltaC, sp.heatC);
}

static void test_deadband_noop_when_already_satisfied() {
  Setpoints sp = applyDeadbandClampPush({18.0f, 26.0f}, SetpointSide::kHeat,
                                        kMinSetpointDeltaC);
  TEST_ASSERT_EQUAL_FLOAT(18.0f, sp.heatC);
  TEST_ASSERT_EQUAL_FLOAT(26.0f, sp.coolC);
}

static void test_deadband_min_delta_clamped_to_floor() {
  // A configured delta below the hard floor (and garbage like NaN) must be
  // clamped UP to kMinSetpointDeltaFloorC (docs/05 defaults table).
  Setpoints sp = applyDeadbandClampPush({22.0f, 22.0f}, SetpointSide::kHeat, 0.2f);
  TEST_ASSERT_EQUAL_FLOAT(22.0f + kMinSetpointDeltaFloorC, sp.coolC);

  sp = applyDeadbandClampPush({22.0f, 22.0f}, SetpointSide::kHeat, NAN);
  TEST_ASSERT_EQUAL_FLOAT(22.0f + kMinSetpointDeltaFloorC, sp.coolC);
}

static void test_deadband_crossed_setpoints_repaired() {
  Setpoints sp = applyDeadbandClampPush({25.0f, 20.0f}, SetpointSide::kCool,
                                        kMinSetpointDeltaC);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, sp.coolC);
  TEST_ASSERT_EQUAL_FLOAT(20.0f - kMinSetpointDeltaC, sp.heatC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, kMinSetpointDeltaC, sp.coolC - sp.heatC);
}

// ---------- adjustSetpoint through the model ----------

static void test_adjust_heat_pushes_cool_and_enqueues_both() {
  UiModel m = freshModel();
  m.setSetpoints(20.0f, 20.0f + kMinSetpointDeltaC);  // exactly at the band
  m.clearDirty();

  m.adjustSetpoint(SetpointSide::kHeat, +1.0f);
  TEST_ASSERT_EQUAL_FLOAT(21.0f, m.state().heatSetpointC);
  TEST_ASSERT_EQUAL_FLOAT(21.0f + kMinSetpointDeltaC, m.state().coolSetpointC);
  TEST_ASSERT_TRUE((m.dirty() & kDirtySetpoints) != 0);

  UiIntent intent;
  TEST_ASSERT_TRUE(m.popIntent(intent));
  TEST_ASSERT_EQUAL(static_cast<int>(IntentType::kSetSetpoints),
                    static_cast<int>(intent.type));
  TEST_ASSERT_EQUAL_FLOAT(21.0f, intent.heatC);
  TEST_ASSERT_EQUAL_FLOAT(21.0f + kMinSetpointDeltaC, intent.coolC);
}

static void test_adjust_setpoint_respects_ui_range() {
  UiModelConfig cfg;
  UiModel m(cfg);
  m.setSetpoints(20.0f, 26.0f);
  m.adjustSetpoint(SetpointSide::kHeat, +100.0f);  // slam to the top
  // Moved setpoint capped so the pushed one still fits inside the UI range.
  TEST_ASSERT_EQUAL_FLOAT(cfg.setpointMaxC - kMinSetpointDeltaC,
                          m.state().heatSetpointC);
  TEST_ASSERT_TRUE(m.state().coolSetpointC <= cfg.setpointMaxC);
  TEST_ASSERT_TRUE(m.state().coolSetpointC - m.state().heatSetpointC >=
                   kMinSetpointDeltaC - 0.001f);

  m.adjustSetpoint(SetpointSide::kCool, -100.0f);  // slam to the bottom
  TEST_ASSERT_EQUAL_FLOAT(cfg.setpointMinC + kMinSetpointDeltaC,
                          m.state().coolSetpointC);
  TEST_ASSERT_TRUE(m.state().heatSetpointC >= cfg.setpointMinC);
}

static void test_nonfinite_delta_rejected_no_intent_no_change() {
  UiModel m = freshModel();
  m.setSetpoints(20.0f, 25.0f);
  m.clearDirty();
  while (true) { UiIntent d; if (!m.popIntent(d)) break; }

  m.adjustSetpoint(SetpointSide::kHeat, NAN);
  m.adjustSetpoint(SetpointSide::kCool, INFINITY);

  TEST_ASSERT_EQUAL_FLOAT(20.0f, m.state().heatSetpointC);
  TEST_ASSERT_EQUAL_FLOAT(25.0f, m.state().coolSetpointC);
  TEST_ASSERT_EQUAL_UINT16(0, m.dirty());
  TEST_ASSERT_EQUAL_UINT32(2, m.rejectedCommandCount());
  UiIntent dummy;
  TEST_ASSERT_FALSE(m.popIntent(dummy));
}

// ---------- intent queue bounding ----------

static void test_intent_queue_overflow_drops_oldest_and_flags() {
  UiModel m = freshModel();
  // Enqueue cap + 3 distinguishable mode intents.
  const UserMode seq[] = {UserMode::kOff, UserMode::kHeat, UserMode::kCool,
                          UserMode::kAuto, UserMode::kEmergencyHeat};
  const size_t total = kIntentQueueCap + 3;
  for (size_t i = 0; i < total; ++i) m.setMode(seq[i % 5]);

  TEST_ASSERT_EQUAL_UINT32(kIntentQueueCap, m.pendingIntents());
  TEST_ASSERT_TRUE(m.intentOverflowed());
  TEST_ASSERT_EQUAL_UINT32(3, m.droppedIntentCount());

  // The 3 oldest were dropped: first popped is the 4th enqueued, FIFO after.
  UiIntent intent;
  for (size_t i = 3; i < total; ++i) {
    TEST_ASSERT_TRUE(m.popIntent(intent));
    TEST_ASSERT_EQUAL(static_cast<int>(IntentType::kSetMode),
                      static_cast<int>(intent.type));
    TEST_ASSERT_EQUAL(static_cast<int>(seq[i % 5]), static_cast<int>(intent.mode));
  }
  TEST_ASSERT_FALSE(m.popIntent(intent));
  TEST_ASSERT_FALSE(m.intentOverflowed());  // drained -> episode cleared
}

static void test_preset_intent_passthrough() {
  UiModel m = freshModel();
  m.setPreset(Preset::kSleep);
  UiIntent intent;
  TEST_ASSERT_TRUE(m.popIntent(intent));
  TEST_ASSERT_EQUAL(static_cast<int>(IntentType::kSetPreset),
                    static_cast<int>(intent.type));
  TEST_ASSERT_EQUAL(static_cast<int>(Preset::kSleep),
                    static_cast<int>(intent.preset));
}

// ---------- dirty flags ----------

static void test_dirty_set_only_on_change_and_per_group() {
  UiModel m = freshModel();

  m.setFusedTemp(21.5f, true);
  TEST_ASSERT_EQUAL_UINT16(kDirtyTemp, m.dirty());
  m.clearDirty(kDirtyTemp);

  m.setFusedTemp(21.5f, true);  // identical: must NOT re-dirty
  TEST_ASSERT_EQUAL_UINT16(0, m.dirty());

  m.setGasModulationPct(kGasFloorPct);
  m.setLinkHealth(true, true, false);
  TEST_ASSERT_EQUAL_UINT16(kDirtyGasMod | kDirtyHealth, m.dirty());

  m.clearDirty(kDirtyGasMod);  // selective clear leaves the other group
  TEST_ASSERT_EQUAL_UINT16(kDirtyHealth, m.dirty());
}

static void test_dirty_sensors_only_on_actual_row_change() {
  UiModel m = freshModel();
  SensorRow rows[2] = {};
  std::strncpy(rows[0].name, "living", kSensorNameLen - 1);
  rows[0].tempC = 21.0f; rows[0].occupied = true; rows[0].ageS = 12;
  rows[0].participating = true; rows[0].healthy = true;
  std::strncpy(rows[1].name, "bedroom", kSensorNameLen - 1);
  rows[1].tempC = 19.5f; rows[1].healthy = true;

  m.setSensorRows(rows, 2);
  TEST_ASSERT_EQUAL_UINT16(kDirtySensors, m.dirty());
  TEST_ASSERT_EQUAL_UINT8(2, m.state().sensorCount);
  m.clearDirty();

  m.setSensorRows(rows, 2);  // unchanged rows: no dirty
  TEST_ASSERT_EQUAL_UINT16(0, m.dirty());

  rows[1].ageS = 400;        // staleness tick must re-render
  m.setSensorRows(rows, 2);
  TEST_ASSERT_EQUAL_UINT16(kDirtySensors, m.dirty());
}

static void test_generation_counter_bumps_on_change() {
  UiModel m = freshModel();
  uint32_t g0 = m.generation();
  m.setCompressorLockoutRemain(123);
  TEST_ASSERT_TRUE(m.generation() > g0);
  uint32_t g1 = m.generation();
  m.setCompressorLockoutRemain(123);  // no change, no bump
  TEST_ASSERT_EQUAL_UINT32(g1, m.generation());
}

// ---------- alarm list bounding ----------

static void test_alarm_list_bounded_drops_oldest_and_flags() {
  UiModel m = freshModel();
  char text[16];
  for (size_t i = 0; i < kMaxAlarms + 2; ++i) {
    std::snprintf(text, sizeof(text), "alarm-%u", static_cast<unsigned>(i));
    m.pushAlarm(text, static_cast<uint16_t>(i));
  }
  const DisplayState& s = m.state();
  TEST_ASSERT_EQUAL_UINT8(kMaxAlarms, s.alarmCount);
  TEST_ASSERT_TRUE(s.alarmsDropped);
  TEST_ASSERT_EQUAL_UINT16(2, s.alarms[0].code);  // two oldest gone
  TEST_ASSERT_EQUAL_STRING("alarm-2", s.alarms[0].text);
  TEST_ASSERT_EQUAL_UINT16(kMaxAlarms + 1, s.alarms[kMaxAlarms - 1].code);
  TEST_ASSERT_TRUE((m.dirty() & kDirtyAlarms) != 0);

  m.clearAlarms();
  TEST_ASSERT_EQUAL_UINT8(0, m.state().alarmCount);
  TEST_ASSERT_FALSE(m.state().alarmsDropped);
}

static void test_alarm_text_truncated_not_overflowed() {
  UiModel m = freshModel();
  char longText[kAlarmTextLen * 2];
  std::memset(longText, 'x', sizeof(longText) - 1);
  longText[sizeof(longText) - 1] = '\0';
  m.pushAlarm(longText, 7);
  TEST_ASSERT_EQUAL_UINT32(kAlarmTextLen - 1, std::strlen(m.state().alarms[0].text));
  m.pushAlarm(nullptr, 8);  // null text must not crash; stored empty
  TEST_ASSERT_EQUAL_STRING("", m.state().alarms[1].text);
}

// ---------- runtime min-delta tuning ----------

static void test_runtime_min_delta_floor_clamped() {
  UiModel m = freshModel();
  m.setMinSetpointDelta(0.1f);  // below floor -> floor applies
  m.setSetpoints(21.0f, 21.0f + kMinSetpointDeltaFloorC);
  m.adjustSetpoint(SetpointSide::kHeat, +0.5f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, kMinSetpointDeltaFloorC,
                           m.state().coolSetpointC - m.state().heatSetpointC);
}

// ---------- screen lock (issue #45) ----------

static const uint8_t kUserPin[kPinLen] = {1, 2, 3, 4};
static const uint8_t kInstPin[kPinLen] = {9, 8, 7, 6};
static const uint8_t kWrongPin[kPinLen] = {0, 0, 0, 0};

static UiModel lockedModel(LockLevel level = LockLevel::kSettingsOnly) {
  UiModel m;
  m.setUserPin(kUserPin, 0xAABBCCDDu);
  m.setInstallerPin(kInstPin, 0x11223344u);
  m.setLockLevel(level);
  m.lockNow(1000);
  m.clearDirty();
  return m;
}

static PinResult enterPin(UiModel& m, const uint8_t* pin, uint32_t nowS,
                          PinContext ctx = PinContext::kUnlock) {
  m.beginPinEntry(ctx, nowS);
  PinResult r = PinResult::kIdle;
  for (size_t i = 0; i < kPinLen; ++i) r = m.enterPinDigit(pin[i], nowS);
  return r;
}

static void drain(UiModel& m) {
  UiIntent d;
  while (m.popIntent(d)) {}
}

static void test_lock_disabled_without_pin() {
  UiModel m;
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kUnlocked),
                    static_cast<int>(m.lockState()));
  TEST_ASSERT_FALSE(m.lockNow(100));                  // no user PIN -> no lock
  TEST_ASSERT_FALSE(m.setInstallerLockout(true, 100));  // no installer code
  m.tick(100 + kUiAutoRelockS + 1);                   // relock with no PIN: no-op
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kUnlocked),
                    static_cast<int>(m.lockState()));
}

static void test_settings_only_lock_blocks_settings_not_setpoints() {
  UiModel m = lockedModel(LockLevel::kSettingsOnly);
  drain(m);

  m.setMode(UserMode::kHeat);  // settings class: blocked
  TEST_ASSERT_EQUAL(static_cast<int>(UserMode::kOff),
                    static_cast<int>(m.state().mode));  // no local echo either
  m.ackAlarms();               // ack class: blocked (visibility stays exempt)
  TEST_ASSERT_EQUAL_UINT32(2, m.lockBlockedCommandCount());
  TEST_ASSERT_TRUE((m.dirty() & kDirtyLock) != 0);
  UiIntent intent;
  TEST_ASSERT_FALSE(m.popIntent(intent));

  m.adjustSetpoint(SetpointSide::kHeat, +0.5f);  // setpoint class: allowed
  m.setPreset(Preset::kAway);                    // preset = setpoint class
  TEST_ASSERT_TRUE(m.popIntent(intent));
  TEST_ASSERT_EQUAL(static_cast<int>(IntentType::kSetSetpoints),
                    static_cast<int>(intent.type));
  TEST_ASSERT_TRUE(m.popIntent(intent));
  TEST_ASSERT_EQUAL(static_cast<int>(IntentType::kSetPreset),
                    static_cast<int>(intent.type));
}

static void test_settings_and_setpoints_lock_blocks_all_change_intents() {
  UiModel m = lockedModel(LockLevel::kSettingsAndSetpoints);
  drain(m);
  m.adjustSetpoint(SetpointSide::kHeat, +0.5f);
  m.setPreset(Preset::kAway);
  m.setMode(UserMode::kCool);
  m.ackAlarms();
  TEST_ASSERT_EQUAL_UINT32(4, m.lockBlockedCommandCount());
  UiIntent intent;
  TEST_ASSERT_FALSE(m.popIntent(intent));
  TEST_ASSERT_EQUAL_FLOAT(kFallbackHeatSetpointC, m.state().heatSetpointC);
}

static void test_lock_never_hides_alarms_temp_or_status() {
  // SAFETY RULE (docs/04 §1c + issue #45): even the strongest lock only
  // blocks change intents — render-side state keeps flowing and dirtying.
  UiModel m = lockedModel(LockLevel::kSettingsAndSetpoints);
  m.setInstallerLockout(true, 1000);
  m.clearDirty();

  m.pushAlarm("HP fault E1", 0xE1);
  m.setFusedTemp(21.2f, true);
  m.setHvacAction(HvacAction::kHeating);
  TEST_ASSERT_EQUAL_UINT8(1, m.state().alarmCount);
  TEST_ASSERT_EQUAL_STRING("HP fault E1", m.state().alarms[0].text);
  TEST_ASSERT_EQUAL_FLOAT(21.2f, m.state().fusedTempC);
  TEST_ASSERT_EQUAL(static_cast<int>(HvacAction::kHeating),
                    static_cast<int>(m.state().action));
  TEST_ASSERT_TRUE((m.dirty() & kDirtyAlarms) != 0);
  TEST_ASSERT_TRUE((m.dirty() & kDirtyTemp) != 0);
  TEST_ASSERT_TRUE((m.dirty() & kDirtyAction) != 0);

  drain(m);
  m.ackAlarms();  // ...but acknowledgement stays locked
  UiIntent intent;
  TEST_ASSERT_FALSE(m.popIntent(intent));
  TEST_ASSERT_EQUAL_UINT8(1, m.state().alarmCount);
}

static void test_ack_alarms_enqueues_intent_when_unlocked() {
  UiModel m = freshModel();
  m.pushAlarm("x", 1);
  m.ackAlarms();
  UiIntent intent;
  TEST_ASSERT_TRUE(m.popIntent(intent));
  TEST_ASSERT_EQUAL(static_cast<int>(IntentType::kAckAlarms),
                    static_cast<int>(intent.type));
}

static void test_pin_unlock_roundtrip() {
  UiModel m = lockedModel();
  TEST_ASSERT_EQUAL(static_cast<int>(PinResult::kAccepted),
                    static_cast<int>(enterPin(m, kUserPin, 2000)));
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kUnlocked),
                    static_cast<int>(m.lockState()));
  TEST_ASSERT_EQUAL_UINT8(0, m.pinDigitsEntered());  // digits wiped
  drain(m);
  m.setMode(UserMode::kHeat);  // settings work again
  UiIntent intent;
  TEST_ASSERT_TRUE(m.popIntent(intent));
  TEST_ASSERT_EQUAL(static_cast<int>(IntentType::kSetMode),
                    static_cast<int>(intent.type));
}

static void test_wrong_pin_attempts_then_backoff() {
  UiModel m = lockedModel();
  const uint32_t t = 5000;
  for (uint8_t i = 0; i < kUiPinMaxAttempts - 1; ++i) {
    TEST_ASSERT_EQUAL(static_cast<int>(PinResult::kRejected),
                      static_cast<int>(enterPin(m, kWrongPin, t)));
  }
  TEST_ASSERT_EQUAL_UINT8(1, m.attemptsRemaining());
  TEST_ASSERT_EQUAL(static_cast<int>(PinResult::kBackoff),
                    static_cast<int>(enterPin(m, kWrongPin, t)));
  TEST_ASSERT_EQUAL_UINT32(kUiPinBackoffS, m.backoffRemainS(t));

  // Even the CORRECT PIN is refused during backoff.
  TEST_ASSERT_EQUAL(static_cast<int>(PinResult::kBackoff),
                    static_cast<int>(enterPin(m, kUserPin, t + 1)));
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kUserLocked),
                    static_cast<int>(m.lockState()));

  m.tick(t + kUiPinBackoffS);  // backoff expires -> attempts reset
  TEST_ASSERT_EQUAL_UINT32(0, m.backoffRemainS(t + kUiPinBackoffS));
  TEST_ASSERT_EQUAL_UINT8(kUiPinMaxAttempts, m.attemptsRemaining());
  TEST_ASSERT_EQUAL(static_cast<int>(PinResult::kAccepted),
                    static_cast<int>(enterPin(m, kUserPin, t + kUiPinBackoffS)));
}

static void test_auto_relock_after_inactivity() {
  UiModel m = lockedModel();
  enterPin(m, kUserPin, 2000);
  m.clearDirty();

  m.tick(2000 + kUiAutoRelockS - 1);  // not yet
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kUnlocked),
                    static_cast<int>(m.lockState()));

  m.touchActivity(2000 + kUiAutoRelockS - 1);  // interaction defers relock
  m.tick(2000 + kUiAutoRelockS);
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kUnlocked),
                    static_cast<int>(m.lockState()));

  m.tick(2000 + kUiAutoRelockS - 1 + kUiAutoRelockS);
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kUserLocked),
                    static_cast<int>(m.lockState()));
  TEST_ASSERT_TRUE((m.dirty() & kDirtyLock) != 0);
}

static void test_installer_lockout_only_installer_code_unlocks() {
  UiModel m = lockedModel();
  TEST_ASSERT_TRUE(m.setInstallerLockout(true, 1000));
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kInstallerLocked),
                    static_cast<int>(m.lockState()));

  // The user PIN is NOT accepted while installer-locked.
  TEST_ASSERT_EQUAL(static_cast<int>(PinResult::kRejected),
                    static_cast<int>(enterPin(m, kUserPin, 2000)));
  TEST_ASSERT_EQUAL(static_cast<int>(PinResult::kAccepted),
                    static_cast<int>(enterPin(m, kInstPin, 2001)));
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kUnlocked),
                    static_cast<int>(m.lockState()));

  // Lockout persists: auto-relock returns to INSTALLER lock, not user lock.
  m.tick(2001 + kUiAutoRelockS);
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kInstallerLocked),
                    static_cast<int>(m.lockState()));

  TEST_ASSERT_TRUE(m.setInstallerLockout(false, 3000));
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kUnlocked),
                    static_cast<int>(m.lockState()));
  m.tick(3000 + kUiAutoRelockS);  // back to the ordinary user lock
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kUserLocked),
                    static_cast<int>(m.lockState()));
}

static void test_installer_settings_gate_and_code_independence() {
  UiModel m;
  m.setUserPin(kUserPin, 1);
  m.setInstallerPin(kInstPin, 2);
  TEST_ASSERT_FALSE(m.installerAccess());

  // The user PIN never opens installer pages.
  TEST_ASSERT_EQUAL(static_cast<int>(PinResult::kRejected),
                    static_cast<int>(enterPin(m, kUserPin, 100,
                                              PinContext::kInstallerSettings)));
  TEST_ASSERT_FALSE(m.installerAccess());

  TEST_ASSERT_EQUAL(static_cast<int>(PinResult::kAccepted),
                    static_cast<int>(enterPin(m, kInstPin, 200,
                                              PinContext::kInstallerSettings)));
  TEST_ASSERT_TRUE(m.installerAccess());

  m.tick(200 + kUiAutoRelockS);  // access expires with the relock clock
  TEST_ASSERT_FALSE(m.installerAccess());
}

static void test_installer_code_is_master_key_for_user_lock() {
  UiModel m = lockedModel();
  TEST_ASSERT_EQUAL(static_cast<int>(PinResult::kAccepted),
                    static_cast<int>(enterPin(m, kInstPin, 2000)));
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kUnlocked),
                    static_cast<int>(m.lockState()));
  TEST_ASSERT_FALSE(m.installerAccess());  // unlock != installer pages access
}

static void test_clear_user_pin_recovery_path() {
  // The HA lock_clear path: no PIN required (HA access = admin, docs/06).
  UiModel m = lockedModel();
  m.clearUserPin();
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kUnlocked),
                    static_cast<int>(m.lockState()));
  TEST_ASSERT_FALSE(m.userPinSet());
  TEST_ASSERT_FALSE(m.lockNow(100));            // lock disabled until a new PIN
  m.tick(100 + kUiAutoRelockS + 1);             // and no auto-relock either
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kUnlocked),
                    static_cast<int>(m.lockState()));
  TEST_ASSERT_TRUE(m.installerPinSet());        // installer code untouched

  // Clearing the user PIN must NOT release an installer lockout.
  UiModel m2 = lockedModel();
  m2.setInstallerLockout(true, 1000);
  m2.clearUserPin();
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kInstallerLocked),
                    static_cast<int>(m2.lockState()));
}

static void test_lock_blob_roundtrip_hashes_survive() {
  UiModel a;
  a.setUserPin(kUserPin, 0xDEADBEEFu);
  a.setInstallerPin(kInstPin, 0x600DCAFEu);
  a.setLockLevel(LockLevel::kSettingsAndSetpoints);
  UiModel::LockPersistBlob blob;
  a.saveLock(&blob);

  UiModel b;
  TEST_ASSERT_TRUE(b.restoreLock(&blob, 5000));
  // Boot = locked (a reboot is not a lock bypass).
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kUserLocked),
                    static_cast<int>(b.lockState()));
  TEST_ASSERT_EQUAL(static_cast<int>(LockLevel::kSettingsAndSetpoints),
                    static_cast<int>(b.lockLevel()));
  TEST_ASSERT_EQUAL(static_cast<int>(PinResult::kRejected),
                    static_cast<int>(enterPin(b, kWrongPin, 5001)));
  TEST_ASSERT_EQUAL(static_cast<int>(PinResult::kAccepted),
                    static_cast<int>(enterPin(b, kUserPin, 5002)));

  // Installer lockout persists through the blob too.
  a.setInstallerLockout(true, 6000);
  a.saveLock(&blob);
  UiModel c;
  TEST_ASSERT_TRUE(c.restoreLock(&blob, 7000));
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kInstallerLocked),
                    static_cast<int>(c.lockState()));
  TEST_ASSERT_EQUAL(static_cast<int>(PinResult::kAccepted),
                    static_cast<int>(enterPin(c, kInstPin, 7001)));
}

static void test_lock_blob_corrupt_or_missing_fails_open() {
  UiModel a = lockedModel();
  UiModel::LockPersistBlob blob;
  a.saveLock(&blob);
  blob.userHash ^= 1;  // corrupt -> CRC mismatch

  UiModel b;
  TEST_ASSERT_FALSE(b.restoreLock(&blob, 100));
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kUnlocked),
                    static_cast<int>(b.lockState()));
  TEST_ASSERT_FALSE(b.userPinSet());
  TEST_ASSERT_FALSE(b.restoreLock(nullptr, 100));
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kUnlocked),
                    static_cast<int>(b.lockState()));
}

static void test_pin_entry_cancel_and_nondigit_ignored() {
  UiModel m = lockedModel();
  m.beginPinEntry(PinContext::kUnlock, 2000);
  m.enterPinDigit(1, 2000);
  m.enterPinDigit(2, 2000);
  TEST_ASSERT_EQUAL_UINT8(2, m.pinDigitsEntered());
  TEST_ASSERT_EQUAL(static_cast<int>(PinResult::kPending),
                    static_cast<int>(m.enterPinDigit(12, 2000)));  // not 0-9
  TEST_ASSERT_EQUAL_UINT8(2, m.pinDigitsEntered());
  m.cancelPinEntry();
  TEST_ASSERT_EQUAL_UINT8(0, m.pinDigitsEntered());
  TEST_ASSERT_EQUAL(static_cast<int>(PinResult::kIdle),
                    static_cast<int>(m.enterPinDigit(3, 2000)));  // no context
  TEST_ASSERT_EQUAL(static_cast<int>(LockState::kUserLocked),
                    static_cast<int>(m.lockState()));
}

// ---------- compressor-held-off predicate (min-OFF "Cooling/Heating soon") ----------

static void test_comp_hold_idle_when_nothing_pending() {
  // No pending call on either side -> plain Idle, even if a rest is running.
  CompressorHold h = evalCompressorHold(false, false, false, 120, 300);
  TEST_ASSERT_FALSE(h.held);
}

static void test_comp_hold_cool_binds_on_larger_of_the_two_min_offs() {
  // Cool pending: guard 90 s vs cool-shaper 300 s -> the shaper is binding.
  CompressorHold h = evalCompressorHold(true, false, false, 90, 300);
  TEST_ASSERT_TRUE(h.held);
  TEST_ASSERT_EQUAL(static_cast<int>(SetpointSide::kCool), static_cast<int>(h.side));
  TEST_ASSERT_EQUAL_UINT32(300, h.remainS);
}

static void test_comp_hold_cool_uses_guard_when_it_is_larger() {
  // Cool pending: guard 150 s, cool-shaper already served -> guard is binding.
  CompressorHold h = evalCompressorHold(true, false, false, 150, 0);
  TEST_ASSERT_TRUE(h.held);
  TEST_ASSERT_EQUAL(static_cast<int>(SetpointSide::kCool), static_cast<int>(h.side));
  TEST_ASSERT_EQUAL_UINT32(150, h.remainS);
}

static void test_comp_hold_heat_is_guard_only() {
  // Heat pending: the cool-shaper timer is irrelevant (HP relay shaper has no
  // demand-level min-OFF); only the guard governs the heat side.
  CompressorHold h = evalCompressorHold(false, true, false, 120, 999);
  TEST_ASSERT_TRUE(h.held);
  TEST_ASSERT_EQUAL(static_cast<int>(SetpointSide::kHeat), static_cast<int>(h.side));
  TEST_ASSERT_EQUAL_UINT32(120, h.remainS);
}

static void test_comp_hold_not_held_when_no_timer_remaining() {
  // Pending but both min-OFFs served -> blocked by duty/starts hygiene, not a
  // min-OFF: fall back to plain Idle (requirement 4).
  CompressorHold h = evalCompressorHold(true, false, false, 0, 0);
  TEST_ASSERT_FALSE(h.held);
}

static void test_comp_hold_lockout_suppresses_soon() {
  // Reset-loop / OAT LOCKOUT is not a min-OFF wait: never say "soon" (the guard
  // anchor would otherwise imply a short rest against a forever-latch).
  CompressorHold h = evalCompressorHold(true, false, true, 120, 300);
  TEST_ASSERT_FALSE(h.held);
}

static void test_comp_hold_setter_survives_model_path() {
  UiModel m = freshModel();
  m.setCompressorHold(true, SetpointSide::kCool, 240);
  TEST_ASSERT_TRUE(m.state().compressorHeldOff);
  TEST_ASSERT_EQUAL(static_cast<int>(SetpointSide::kCool),
                    static_cast<int>(m.state().compressorHeldSide));
  TEST_ASSERT_EQUAL_UINT32(240, m.state().compressorHeldRemainS);
  m.setCompressorHold(false, SetpointSide::kHeat, 0);  // satisfied again -> plain Idle
  TEST_ASSERT_FALSE(m.state().compressorHeldOff);
}

// ---------- #181 intent observer (camera-remote audit capture) ----------

namespace {
struct ObsCapture {
  int calls = 0;
  IntentType lastType = IntentType::kAckAlarms;
  char lastPreset[kUiPresetNameLen] = {};
};
void obsCb(const UiIntent& it, const char* presetName, void* ctx) {
  auto* c = static_cast<ObsCapture*>(ctx);
  ++c->calls;
  c->lastType = it.type;
  strncpy(c->lastPreset, presetName, sizeof(c->lastPreset) - 1);
}
}  // namespace

static void test_intent_observer_fires_post_lock_with_preset_name() {
  UiModel m = freshModel();
  ObsCapture cap;
  m.setIntentObserver(obsCb, &cap);

  // Fires for a change intent, with the roster name resolved for presets.
  DisplayState::PresetView pv[2] = {};
  strncpy(pv[0].name, "home", sizeof(pv[0].name));
  strncpy(pv[1].name, "away", sizeof(pv[1].name));
  m.setPresets(pv, 2);
  m.setPreset(static_cast<Preset>(1));
  TEST_ASSERT_EQUAL_INT(1, cap.calls);
  TEST_ASSERT_EQUAL(static_cast<int>(IntentType::kSetPreset),
                    static_cast<int>(cap.lastType));
  TEST_ASSERT_EQUAL_STRING("away", cap.lastPreset);

  // Non-preset intents pass "" (no stale name leaks through).
  m.adjustSetpoint(SetpointSide::kHeat, 0.5f);
  TEST_ASSERT_EQUAL_INT(2, cap.calls);
  TEST_ASSERT_EQUAL(static_cast<int>(IntentType::kSetSetpoints),
                    static_cast<int>(cap.lastType));
  TEST_ASSERT_EQUAL_STRING("", cap.lastPreset);

  // A lock-blocked change never reaches the observer (post-lock choke point).
  const uint8_t pin[kPinLen] = {1, 2, 3, 4};
  m.setUserPin(pin, 0xA5A5A5A5u);
  m.setLockLevel(LockLevel::kSettingsAndSetpoints);
  TEST_ASSERT_TRUE(m.lockNow(1000));
  m.setPreset(static_cast<Preset>(0));
  TEST_ASSERT_EQUAL_INT(2, cap.calls);
}

static void test_intent_observer_null_by_default_and_out_of_range_preset() {
  UiModel m = freshModel();  // no observer set: mutators must not crash
  m.setMode(UserMode::kHeat);
  ObsCapture cap;
  m.setIntentObserver(obsCb, &cap);
  m.setPreset(static_cast<Preset>(7));  // out of roster range -> "" name
  TEST_ASSERT_EQUAL_INT(1, cap.calls);
  TEST_ASSERT_EQUAL_STRING("", cap.lastPreset);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_boot_state_is_safe_and_fully_dirty);
  RUN_TEST(test_deadband_push_cool_up_when_heat_moves);
  RUN_TEST(test_deadband_push_heat_down_when_cool_moves);
  RUN_TEST(test_deadband_noop_when_already_satisfied);
  RUN_TEST(test_deadband_min_delta_clamped_to_floor);
  RUN_TEST(test_deadband_crossed_setpoints_repaired);
  RUN_TEST(test_adjust_heat_pushes_cool_and_enqueues_both);
  RUN_TEST(test_adjust_setpoint_respects_ui_range);
  RUN_TEST(test_nonfinite_delta_rejected_no_intent_no_change);
  RUN_TEST(test_intent_queue_overflow_drops_oldest_and_flags);
  RUN_TEST(test_preset_intent_passthrough);
  RUN_TEST(test_dirty_set_only_on_change_and_per_group);
  RUN_TEST(test_dirty_sensors_only_on_actual_row_change);
  RUN_TEST(test_generation_counter_bumps_on_change);
  RUN_TEST(test_alarm_list_bounded_drops_oldest_and_flags);
  RUN_TEST(test_alarm_text_truncated_not_overflowed);
  RUN_TEST(test_runtime_min_delta_floor_clamped);
  RUN_TEST(test_lock_disabled_without_pin);
  RUN_TEST(test_settings_only_lock_blocks_settings_not_setpoints);
  RUN_TEST(test_settings_and_setpoints_lock_blocks_all_change_intents);
  RUN_TEST(test_lock_never_hides_alarms_temp_or_status);
  RUN_TEST(test_ack_alarms_enqueues_intent_when_unlocked);
  RUN_TEST(test_pin_unlock_roundtrip);
  RUN_TEST(test_wrong_pin_attempts_then_backoff);
  RUN_TEST(test_auto_relock_after_inactivity);
  RUN_TEST(test_installer_lockout_only_installer_code_unlocks);
  RUN_TEST(test_installer_settings_gate_and_code_independence);
  RUN_TEST(test_installer_code_is_master_key_for_user_lock);
  RUN_TEST(test_clear_user_pin_recovery_path);
  RUN_TEST(test_lock_blob_roundtrip_hashes_survive);
  RUN_TEST(test_lock_blob_corrupt_or_missing_fails_open);
  RUN_TEST(test_pin_entry_cancel_and_nondigit_ignored);
  RUN_TEST(test_comp_hold_idle_when_nothing_pending);
  RUN_TEST(test_comp_hold_cool_binds_on_larger_of_the_two_min_offs);
  RUN_TEST(test_comp_hold_cool_uses_guard_when_it_is_larger);
  RUN_TEST(test_comp_hold_heat_is_guard_only);
  RUN_TEST(test_comp_hold_not_held_when_no_timer_remaining);
  RUN_TEST(test_comp_hold_lockout_suppresses_soon);
  RUN_TEST(test_comp_hold_setter_survives_model_path);
  RUN_TEST(test_intent_observer_fires_post_lock_with_preset_name);
  RUN_TEST(test_intent_observer_null_by_default_and_out_of_range_preset);
  return UNITY_END();
}
