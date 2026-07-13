// SafetySupervisor unit tests: pet-gating truth table (advisory vs mandatory),
// comms-loss deadman timing, boot-gate sequencing incl. the grace alarm,
// alarm bounding/ack semantics, reset-loop window accounting, and the
// demand-drop filter feeding DemandArbiter's force-zero path.
#include <unity.h>
#include <cstdio>
#include <cstring>

#include "DettsonConfig.h"
#include "SafetySupervisor.h"

using namespace dettson;
using namespace dettson::safety;

void setUp() {}
void tearDown() {}

static HealthFacts allGood() {
  HealthFacts f;
  f.sensorValid        = true;
  f.setpointFresh      = true;
  f.mqttAlive          = true;
  f.busAlive           = true;
  f.controlLoopTicking = true;
  f.demandStateSane    = true;
  return f;
}

static BootFacts bootOk() {
  BootFacts b;
  b.sensorOk        = true;
  b.setpointPresent = true;
  b.configCrcOk     = true;
  return b;
}

// Opens the boot gate and reports healthy facts at nowS.
static void bringUp(SafetySupervisor& sup, uint32_t nowS) {
  sup.updateBootGate(bootOk(), nowS);
  sup.update(allGood(), nowS);
}

// ---------- pet-gating truth table ----------

static void test_pet_all_good() {
  SafetySupervisor sup(0);
  bringUp(sup, 1);
  TEST_ASSERT_TRUE(sup.petExternalWdt());
  TEST_ASSERT_TRUE(sup.demandPermitted());
  TEST_ASSERT_FALSE(sup.healthProblem());
}

static void test_pet_unreported_facts_default_unsafe() {
  SafetySupervisor sup(0);
  // No update() yet: facts unproven -> no pet, no demand (docs/04 §3).
  TEST_ASSERT_FALSE(sup.petExternalWdt());
  TEST_ASSERT_FALSE(sup.demandPermitted());
}

static void test_pet_advisory_mqtt_loss_keeps_petting_and_heat() {
  // docs/04 §2: HA/MQTT loss must NOT cause no-heat.
  SafetySupervisor sup(0);
  bringUp(sup, 1);
  HealthFacts f = allGood();
  f.mqttAlive = false;
  sup.update(f, 2);
  TEST_ASSERT_TRUE(sup.petExternalWdt());
  TEST_ASSERT_TRUE(sup.demandPermitted());
  const AlarmEntry* a = sup.alarms().find(kAlarmMqttDown);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_TRUE(a->active);
  TEST_ASSERT_EQUAL(static_cast<int>(Severity::kAdvisory),
                    static_cast<int>(a->severity));
}

static void test_pet_advisory_setpoint_stale_keeps_petting_and_heat() {
  SafetySupervisor sup(0);
  bringUp(sup, 1);
  HealthFacts f = allGood();
  f.setpointFresh = false;
  sup.update(f, 2);
  TEST_ASSERT_TRUE(sup.petExternalWdt());
  TEST_ASSERT_TRUE(sup.demandPermitted());
  TEST_ASSERT_NOT_NULL(sup.alarms().find(kAlarmSetpointStale));
  TEST_ASSERT_FALSE(sup.alarms().anyActiveCritical());
}

static void test_pet_mandatory_sensor_invalid_stops_petting() {
  // docs/04 §2: sensor invalid MUST cause no-heat.
  SafetySupervisor sup(0);
  bringUp(sup, 1);
  HealthFacts f = allGood();
  f.sensorValid = false;
  sup.update(f, 2);
  TEST_ASSERT_FALSE(sup.petExternalWdt());
  TEST_ASSERT_TRUE(sup.demandDropRequested());
  TEST_ASSERT_FALSE(sup.demandPermitted());
  TEST_ASSERT_TRUE(sup.alarms().anyActiveCritical());
}

static void test_pet_mandatory_control_loop_stalled_stops_petting() {
  SafetySupervisor sup(0);
  bringUp(sup, 1);
  HealthFacts f = allGood();
  f.controlLoopTicking = false;
  sup.update(f, 2);
  TEST_ASSERT_FALSE(sup.petExternalWdt());
  TEST_ASSERT_FALSE(sup.demandPermitted());
}

static void test_pet_mandatory_demand_insane_stops_petting() {
  SafetySupervisor sup(0);
  bringUp(sup, 1);
  HealthFacts f = allGood();
  f.demandStateSane = false;
  sup.update(f, 2);
  TEST_ASSERT_FALSE(sup.petExternalWdt());
  TEST_ASSERT_FALSE(sup.demandPermitted());
}

static void test_pet_sensor_invalid_during_boot_gate_keeps_petting() {
  // Boot exception: gate closed blocks demand, so sensor bring-up must not
  // starve the watchdog (chip could never boot otherwise).
  SafetySupervisor sup(0);
  HealthFacts f = allGood();
  f.sensorValid = false;
  sup.update(f, 1);
  TEST_ASSERT_FALSE(sup.bootGateOpen());
  TEST_ASSERT_TRUE(sup.petExternalWdt());
  TEST_ASSERT_FALSE(sup.demandPermitted());  // but still no demand

  // Once the gate opens, the same fact stops petting.
  sup.updateBootGate(bootOk(), 2);
  sup.update(f, 3);
  TEST_ASSERT_FALSE(sup.petExternalWdt());
}

static void test_pet_recovers_when_facts_recover() {
  SafetySupervisor sup(0);
  bringUp(sup, 1);
  HealthFacts f = allGood();
  f.sensorValid = false;
  sup.update(f, 2);
  TEST_ASSERT_FALSE(sup.petExternalWdt());
  sup.update(allGood(), 3);
  TEST_ASSERT_TRUE(sup.petExternalWdt());
  TEST_ASSERT_TRUE(sup.demandPermitted());
  // Auto-recoverable (issue #72): the sensor-invalid alarm drops the moment a
  // valid sample returns — no manual ack, no stale Diag entry.
  TEST_ASSERT_NULL(sup.alarms().find(kAlarmSensorInvalid));
}

// ---------- comms-loss deadman ----------

static void test_deadman_timing() {
  SafetySupervisor sup(0);
  bringUp(sup, 100);

  HealthFacts f = allGood();
  f.busAlive = false;
  sup.update(f, 100);  // silence starts
  TEST_ASSERT_FALSE(sup.busDeadmanTripped());
  TEST_ASSERT_TRUE(sup.demandPermitted());

  sup.update(f, 100 + kBusDeadmanS - 1);
  TEST_ASSERT_FALSE(sup.busDeadmanTripped());

  sup.update(f, 100 + kBusDeadmanS);
  TEST_ASSERT_TRUE(sup.busDeadmanTripped());
  TEST_ASSERT_TRUE(sup.demandDropRequested());
  TEST_ASSERT_FALSE(sup.demandPermitted());
  // Deadman keeps petting: a reset cannot revive the bus, and silence is
  // already the equipment-side failsafe (docs/04 §1).
  TEST_ASSERT_TRUE(sup.petExternalWdt());
  const AlarmEntry* a = sup.alarms().find(kAlarmBusDeadman);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_TRUE(a->active);
  TEST_ASSERT_EQUAL(static_cast<int>(Severity::kCritical),
                    static_cast<int>(a->severity));
}

static void test_deadman_recovery_and_restart() {
  SafetySupervisor sup(0);
  bringUp(sup, 100);
  HealthFacts dead = allGood();
  dead.busAlive = false;

  sup.update(dead, 100);
  sup.update(dead, 100 + kBusDeadmanS);
  TEST_ASSERT_TRUE(sup.busDeadmanTripped());

  sup.update(allGood(), 200 + kBusDeadmanS);  // bus back
  TEST_ASSERT_FALSE(sup.busDeadmanTripped());
  TEST_ASSERT_TRUE(sup.demandPermitted());
  // autoClear: the entry is REMOVED on bus recovery (not just marked inactive)
  // so the published alarmN = alarms().count() drops — the stale-alarm fix. A
  // latched (autoClear=false) deadman would linger here as an un-acked entry.
  TEST_ASSERT_NULL(sup.alarms().find(kAlarmBusDeadman));

  // A new silence restarts the timer from its own start, not the old one.
  uint32_t t = 1000;
  sup.update(dead, t);
  sup.update(dead, t + kBusDeadmanS - 1);
  TEST_ASSERT_FALSE(sup.busDeadmanTripped());
  sup.update(dead, t + kBusDeadmanS);
  TEST_ASSERT_TRUE(sup.busDeadmanTripped());
}

static void test_deadman_intermittent_bus_never_trips() {
  SafetySupervisor sup(0);
  bringUp(sup, 0);
  HealthFacts dead = allGood();
  dead.busAlive = false;
  uint32_t t = 0;
  for (int i = 0; i < 10; ++i) {  // silence always shorter than the deadman
    sup.update(dead, t);
    t += kBusDeadmanS - 5;
    sup.update(dead, t);
    sup.update(allGood(), ++t);
  }
  TEST_ASSERT_FALSE(sup.busDeadmanTripped());
}

// ---------- boot gate ----------

static void test_boot_gate_holds_until_all_facts() {
  SafetySupervisor sup(1000);
  sup.update(allGood(), 1001);  // perfect runtime health does not open the gate
  TEST_ASSERT_FALSE(sup.bootGateOpen());
  TEST_ASSERT_FALSE(sup.demandsAllowed());
  TEST_ASSERT_FALSE(sup.demandPermitted());

  BootFacts b;  // each fact alone is insufficient
  b.sensorOk = true;
  sup.updateBootGate(b, 1002);
  TEST_ASSERT_FALSE(sup.bootGateOpen());
  b.setpointPresent = true;
  sup.updateBootGate(b, 1003);
  TEST_ASSERT_FALSE(sup.bootGateOpen());
  b.configCrcOk = true;
  sup.updateBootGate(b, 1004);
  TEST_ASSERT_TRUE(sup.bootGateOpen());
  TEST_ASSERT_TRUE(sup.demandsAllowed());
  TEST_ASSERT_TRUE(sup.demandPermitted());

  // Latched open: later boot-fact regression does not close it (runtime
  // HealthFacts own degradation after boot).
  sup.updateBootGate(BootFacts{}, 1005);
  TEST_ASSERT_TRUE(sup.bootGateOpen());
}

static void test_boot_gate_grace_alarm() {
  SafetySupervisor sup(1000);
  BootFacts b;  // never validates
  sup.updateBootGate(b, 1000 + kBootValidationGraceS - 1);
  TEST_ASSERT_FALSE(sup.bootGraceExceeded());
  TEST_ASSERT_NULL(sup.alarms().find(kAlarmBootGraceExceeded));

  sup.updateBootGate(b, 1000 + kBootValidationGraceS);
  TEST_ASSERT_TRUE(sup.bootGraceExceeded());
  const AlarmEntry* a = sup.alarms().find(kAlarmBootGraceExceeded);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_TRUE(a->active);
  TEST_ASSERT_FALSE(sup.demandsAllowed());  // alarm is visibility, gate stays shut

  // Late validation still opens the gate and clears the condition.
  sup.updateBootGate(bootOk(), 1000 + kBootValidationGraceS + 50);
  TEST_ASSERT_TRUE(sup.bootGateOpen());
  TEST_ASSERT_FALSE(sup.bootGraceExceeded());
  a = sup.alarms().find(kAlarmBootGraceExceeded);
  if (a != nullptr) TEST_ASSERT_FALSE(a->active);  // unacked stays listed, inactive
}

// ---------- alarm registry ----------

static void test_alarm_bounding_drops_oldest() {
  AlarmRegistry reg;
  char text[8];
  for (uint16_t c = 1; c <= 10; ++c) {
    std::snprintf(text, sizeof(text), "a%u", c);
    reg.raise(c, Severity::kAdvisory, text, c);
  }
  TEST_ASSERT_EQUAL_UINT8(kMaxAlarmEntries, reg.count());
  TEST_ASSERT_TRUE(reg.overflowed());
  TEST_ASSERT_NULL(reg.find(1));  // oldest two dropped
  TEST_ASSERT_NULL(reg.find(2));
  TEST_ASSERT_NOT_NULL(reg.find(3));
  TEST_ASSERT_NOT_NULL(reg.find(10));
  TEST_ASSERT_EQUAL_STRING("a10", reg.lastErrorText());
  reg.clearOverflowed();
  TEST_ASSERT_FALSE(reg.overflowed());
}

static void test_alarm_reraise_does_not_duplicate() {
  AlarmRegistry reg;
  for (int i = 0; i < 20; ++i) reg.raise(7, Severity::kCritical, "same", 10 + i);
  TEST_ASSERT_EQUAL_UINT8(1, reg.count());
  TEST_ASSERT_EQUAL_UINT32(10, reg.find(7)->raisedAtS);  // first raise kept
  TEST_ASSERT_FALSE(reg.overflowed());
}

static void test_alarm_ack_semantics() {
  AlarmRegistry reg;
  reg.raise(1, Severity::kCritical, "boom", 5);

  // Ack while active: stays listed (problem persists), marked acked.
  TEST_ASSERT_TRUE(reg.ack(1));
  TEST_ASSERT_NOT_NULL(reg.find(1));
  TEST_ASSERT_TRUE(reg.find(1)->acked);
  TEST_ASSERT_TRUE(reg.anyActive());
  TEST_ASSERT_FALSE(reg.anyUnacked());

  // Condition clears after ack: entry removed.
  reg.clearCondition(1);
  TEST_ASSERT_NULL(reg.find(1));
  TEST_ASSERT_EQUAL_UINT8(0, reg.count());

  // Clear before ack: stays listed inactive; ack then removes it.
  reg.raise(2, Severity::kAdvisory, "blip", 6);
  reg.clearCondition(2);
  TEST_ASSERT_NOT_NULL(reg.find(2));
  TEST_ASSERT_FALSE(reg.find(2)->active);
  TEST_ASSERT_FALSE(reg.anyActive());
  TEST_ASSERT_TRUE(reg.anyUnacked());
  TEST_ASSERT_TRUE(reg.ack(2));
  TEST_ASSERT_NULL(reg.find(2));

  TEST_ASSERT_FALSE(reg.ack(99));  // unknown code
}

static void test_alarm_reraise_after_clear_realerts() {
  AlarmRegistry reg;
  reg.raise(3, Severity::kCritical, "fault", 1);
  reg.ack(3);
  reg.clearCondition(3);   // gone
  reg.raise(3, Severity::kCritical, "fault", 50);
  const AlarmEntry* a = reg.find(3);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_TRUE(a->active);
  TEST_ASSERT_FALSE(a->acked);  // re-alert demands a fresh ack
  TEST_ASSERT_EQUAL_UINT32(50, a->raisedAtS);
}

// Issue #72: an auto-clear alarm drops the instant its condition resolves,
// with no ack — while a default (non-auto-clear) alarm still persists until
// acked. Registry-level raise->resolve->clear.
static void test_alarm_autoclear_drops_without_ack() {
  AlarmRegistry reg;
  reg.raise(1, Severity::kCritical, "auto", 5, /*autoClear=*/true);
  TEST_ASSERT_NOT_NULL(reg.find(1));
  TEST_ASSERT_TRUE(reg.find(1)->active);
  reg.clearCondition(1);                 // condition resolves
  TEST_ASSERT_NULL(reg.find(1));         // gone, no ack needed
  TEST_ASSERT_EQUAL_UINT8(0, reg.count());

  // Default (autoClear=false) still persists inactive until acked.
  reg.raise(2, Severity::kAdvisory, "sticky", 6);
  reg.clearCondition(2);
  TEST_ASSERT_NOT_NULL(reg.find(2));
  TEST_ASSERT_FALSE(reg.find(2)->active);
}

// Issue #72: the supervisor's own auto-recoverable conditions (MQTT, sensor)
// self-clear one cycle after the fact returns — the reported bug where Diag
// showed a stale "MQTT/HA link down" while the link was live.
static void test_supervisor_recoverable_alarms_selfclear() {
  SafetySupervisor sup(0);
  bringUp(sup, 1);
  HealthFacts f = allGood();
  f.mqttAlive = false;
  sup.update(f, 2);
  TEST_ASSERT_NOT_NULL(sup.alarms().find(kAlarmMqttDown));
  sup.update(allGood(), 3);              // link back
  TEST_ASSERT_NULL(sup.alarms().find(kAlarmMqttDown));   // cleared, no ack
  TEST_ASSERT_FALSE(sup.healthProblem());
}

// Mock with the UiModel alarm sink shape (clearAlarms + pushAlarm) — verifies
// the adapter without depending on the parallel-owned UI lib.
struct UiSinkMock {
  int      cleared = 0;
  uint8_t  n       = 0;
  uint16_t codes[16] = {};
  char     texts[16][kAlarmTextLen] = {};
  void clearAlarms() { ++cleared; n = 0; }
  void pushAlarm(const char* text, uint16_t code) {
    codes[n] = code;
    std::strncpy(texts[n], text, kAlarmTextLen - 1);
    ++n;
  }
};

static void test_alarm_ui_adapter_sync() {
  AlarmRegistry reg;
  reg.raise(kAlarmMqttDown, Severity::kAdvisory, "MQTT/HA link down", 1);
  reg.raise(kAlarmBusDeadman, Severity::kCritical, "CT-485 silent", 2);
  reg.clearCondition(kAlarmMqttDown);  // inactive-unacked still shown

  UiSinkMock ui;
  syncAlarmsToUi(reg, ui);
  TEST_ASSERT_EQUAL_INT(1, ui.cleared);
  TEST_ASSERT_EQUAL_UINT8(2, ui.n);
  TEST_ASSERT_EQUAL_UINT16(kAlarmMqttDown, ui.codes[0]);
  TEST_ASSERT_EQUAL_UINT16(kAlarmBusDeadman, ui.codes[1]);
  TEST_ASSERT_EQUAL_STRING("CT-485 silent", ui.texts[1]);
}

static void test_alarm_text_truncated_not_overrun() {
  AlarmRegistry reg;
  char longText[100];
  std::memset(longText, 'x', sizeof(longText) - 1);
  longText[sizeof(longText) - 1] = '\0';
  reg.raise(1, Severity::kAdvisory, longText, 0);
  TEST_ASSERT_EQUAL_UINT32(kAlarmTextLen - 1, std::strlen(reg.find(1)->text));
  TEST_ASSERT_EQUAL_UINT32(kAlarmTextLen - 1, std::strlen(reg.lastErrorText()));
  reg.raise(2, Severity::kAdvisory, nullptr, 0);  // tolerated
  TEST_ASSERT_EQUAL_STRING("", reg.find(2)->text);
}

// ---------- reset-loop accounting ----------

static void test_reset_loop_latches_in_window() {
  ResetLoopAccountant rl;
  rl.recordBoot(0, true);
  rl.recordBoot(600, true);
  TEST_ASSERT_FALSE(rl.latched());
  TEST_ASSERT_EQUAL_UINT8(2, rl.abnormalBootsInWindow(600));
  rl.recordBoot(1200, true);  // 3rd within 30 min
  TEST_ASSERT_TRUE(rl.latched());
}

static void test_reset_loop_spread_boots_do_not_latch() {
  ResetLoopAccountant rl;
  rl.recordBoot(0, true);
  rl.recordBoot(kResetLoopWindowS + 1, true);
  rl.recordBoot(2 * (kResetLoopWindowS + 1), true);
  TEST_ASSERT_FALSE(rl.latched());
  TEST_ASSERT_EQUAL_UINT8(1, rl.abnormalBootsInWindow(2 * (kResetLoopWindowS + 1)));
}

static void test_reset_loop_normal_boots_never_count() {
  ResetLoopAccountant rl;
  for (int i = 0; i < 10; ++i) rl.recordBoot(i * 10, false);
  TEST_ASSERT_FALSE(rl.latched());
  TEST_ASSERT_EQUAL_UINT8(0, rl.abnormalBootsInWindow(100));
}

static void test_reset_loop_persistence_roundtrip_and_manual_clear() {
  ResetLoopAccountant rl;
  rl.recordBoot(100, true);
  rl.recordBoot(200, true);
  rl.recordBoot(300, true);
  TEST_ASSERT_TRUE(rl.latched());

  uint32_t blob[ResetLoopAccountant::kMaxBootHistory];
  size_t n = rl.save(blob, ResetLoopAccountant::kMaxBootHistory);
  TEST_ASSERT_EQUAL_UINT32(3, n);

  ResetLoopAccountant rl2;
  rl2.restore(blob, n, true);   // "reboot": latch + history survive
  TEST_ASSERT_TRUE(rl2.latched());
  rl2.recordBoot(400, true);
  TEST_ASSERT_EQUAL_UINT8(4, rl2.abnormalBootsInWindow(400));

  rl2.manualClear();            // deliberate human action only
  TEST_ASSERT_FALSE(rl2.latched());
  TEST_ASSERT_EQUAL_UINT8(0, rl2.abnormalBootsInWindow(400));
}

static void test_reset_loop_latch_blocks_demand_but_keeps_petting() {
  SafetySupervisor sup(0);
  sup.resetLoop().recordBoot(0, true);
  sup.resetLoop().recordBoot(10, true);
  sup.resetLoop().recordBoot(20, true);
  TEST_ASSERT_TRUE(sup.resetLoop().latched());

  bringUp(sup, 30);  // boot validates, health perfect — latch still wins
  TEST_ASSERT_FALSE(sup.demandsAllowed());
  TEST_ASSERT_FALSE(sup.demandPermitted());
  // Keep petting: starving the WDT would just continue the reset loop.
  TEST_ASSERT_TRUE(sup.petExternalWdt());
  const AlarmEntry* a = sup.alarms().find(kAlarmResetLoopLatched);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_TRUE(a->active);

  sup.resetLoop().manualClear();
  sup.update(allGood(), 40);
  TEST_ASSERT_TRUE(sup.demandPermitted());
  TEST_ASSERT_FALSE(sup.alarms().find(kAlarmResetLoopLatched)->active);
}

// ---------- demand-drop filter (DemandArbiter integration) ----------

static void test_filter_request_zeroes_on_drop() {
  SafetySupervisor sup(0);
  DemandRequest req;
  req.gasHeatPct = 60.0f;
  req.fanPct     = 30.0f;

  // Gate closed -> everything zeroed.
  DemandRequest out = sup.filterRequest(req);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.gasHeatPct);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.fanPct);

  bringUp(sup, 1);
  out = sup.filterRequest(req);  // healthy -> passthrough
  TEST_ASSERT_EQUAL_FLOAT(60.0f, out.gasHeatPct);
  TEST_ASSERT_EQUAL_FLOAT(30.0f, out.fanPct);

  HealthFacts f = allGood();
  f.sensorValid = false;
  sup.update(f, 2);
  out = sup.filterRequest(req);  // mandatory fault -> all channels zero
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.gasHeatPct);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.fanPct);
}

// ----------

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_pet_all_good);
  RUN_TEST(test_pet_unreported_facts_default_unsafe);
  RUN_TEST(test_pet_advisory_mqtt_loss_keeps_petting_and_heat);
  RUN_TEST(test_pet_advisory_setpoint_stale_keeps_petting_and_heat);
  RUN_TEST(test_pet_mandatory_sensor_invalid_stops_petting);
  RUN_TEST(test_pet_mandatory_control_loop_stalled_stops_petting);
  RUN_TEST(test_pet_mandatory_demand_insane_stops_petting);
  RUN_TEST(test_pet_sensor_invalid_during_boot_gate_keeps_petting);
  RUN_TEST(test_pet_recovers_when_facts_recover);
  RUN_TEST(test_deadman_timing);
  RUN_TEST(test_deadman_recovery_and_restart);
  RUN_TEST(test_deadman_intermittent_bus_never_trips);
  RUN_TEST(test_boot_gate_holds_until_all_facts);
  RUN_TEST(test_boot_gate_grace_alarm);
  RUN_TEST(test_alarm_bounding_drops_oldest);
  RUN_TEST(test_alarm_reraise_does_not_duplicate);
  RUN_TEST(test_alarm_ack_semantics);
  RUN_TEST(test_alarm_reraise_after_clear_realerts);
  RUN_TEST(test_alarm_autoclear_drops_without_ack);
  RUN_TEST(test_supervisor_recoverable_alarms_selfclear);
  RUN_TEST(test_alarm_ui_adapter_sync);
  RUN_TEST(test_alarm_text_truncated_not_overrun);
  RUN_TEST(test_reset_loop_latches_in_window);
  RUN_TEST(test_reset_loop_spread_boots_do_not_latch);
  RUN_TEST(test_reset_loop_normal_boots_never_count);
  RUN_TEST(test_reset_loop_persistence_roundtrip_and_manual_clear);
  RUN_TEST(test_reset_loop_latch_blocks_demand_but_keeps_petting);
  RUN_TEST(test_filter_request_zeroes_on_drop);
  return UNITY_END();
}
