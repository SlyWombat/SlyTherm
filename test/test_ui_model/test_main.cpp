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
  return UNITY_END();
}
