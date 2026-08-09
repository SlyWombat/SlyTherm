// Unit tests for EquipmentHealth — the #189 equipment-effectiveness watchdog.
// The seed scenario is the 2026-08-09 field incident: cooling commanded and
// ACKed for hours, blower running, zero alarms anywhere — and the room WARMING
// the whole time because the outdoor unit sat in a protection lockout.
#include <unity.h>

#include "DettsonConfig.h"
#include "EquipmentHealth.h"

using namespace dettson;

void setUp() {}
void tearDown() {}

static EquipmentHealthInputs cool(float roomC, float setC = 21.0f) {
  EquipmentHealthInputs in;
  in.family = EquipFamily::kCool;
  in.roomC = roomC;
  in.roomValid = true;
  in.setpointC = setC;
  return in;
}

static EquipmentHealthInputs heat(float roomC, float setC = 21.0f) {
  EquipmentHealthInputs in = cool(roomC, setC);
  in.family = EquipFamily::kHeat;
  return in;
}

// ---- the 2026-08-09 incident: cooling runs, room RISES -> alarm at deadline ----
static void test_cooling_no_progress_fires_at_deadline() {
  EquipmentHealth eh;
  uint32_t t = 1000;
  TEST_ASSERT_FALSE(eh.step(cool(23.0f), t).ineffective);
  // Room creeps UP through the whole window (the real incident's shape).
  TEST_ASSERT_FALSE(eh.step(cool(23.3f), t + kEffectMinRunS - 1).ineffective);
  EquipmentHealthOutput out = eh.step(cool(23.5f), t + kEffectMinRunS);
  TEST_ASSERT_TRUE(out.ineffective);
  TEST_ASSERT_TRUE(out.family == EquipFamily::kCool);
}

// ---- real cooling progress: no alarm, and progress latches ----
static void test_cooling_progress_latches_no_alarm() {
  EquipmentHealth eh;
  uint32_t t = 1000;
  eh.step(cool(23.0f), t);
  // Drops 0.3C mid-window: progress latched...
  TEST_ASSERT_FALSE(eh.step(cool(22.7f), t + 600).ineffective);
  // ...even if afternoon heat load claws it back above the start point.
  TEST_ASSERT_FALSE(eh.step(cool(23.2f), t + kEffectMinRunS + 600).ineffective);
}

// ---- supply probe: healthy delta suppresses the trend verdict ----
static void test_supply_healthy_suppresses_trend() {
  EquipmentHealth eh;
  uint32_t t = 1000;
  EquipmentHealthInputs in = cool(23.0f);
  in.supplyValid = true;
  in.supplyC = 23.0f - kEffectCoolSupplyDeltaC - 2.0f;  // plenty cold
  eh.step(in, t);
  // Room never moves (open windows / heat wave) — equipment provably fine.
  TEST_ASSERT_FALSE(eh.step(in, t + 2 * kEffectMinRunS).ineffective);
}

// ---- supply probe: weak delta convicts at the sharper deadline ----
static void test_supply_failed_fires_early() {
  EquipmentHealth eh;
  uint32_t t = 1000;
  EquipmentHealthInputs in = cool(23.0f);
  in.supplyValid = true;
  in.supplyC = 22.0f;  // 1C below room: blower on, coil doing nothing
  eh.step(in, t);
  TEST_ASSERT_FALSE(eh.step(in, t + kEffectSupplyMinRunS - 1).ineffective);
  TEST_ASSERT_TRUE(eh.step(in, t + kEffectSupplyMinRunS).ineffective);
}

// ---- setpoint change resets the judgment window ----
static void test_setpoint_change_resets_window() {
  EquipmentHealth eh;
  uint32_t t = 1000;
  eh.step(cool(23.0f, 21.0f), t);
  uint32_t t2 = t + kEffectMinRunS - 60;
  eh.step(cool(23.0f, 19.0f), t2);  // owner cranks it down: new baseline
  TEST_ASSERT_FALSE(eh.step(cool(23.0f, 19.0f), t + kEffectMinRunS + 60).ineffective);
  TEST_ASSERT_TRUE(eh.step(cool(23.0f, 19.0f), t2 + kEffectMinRunS).ineffective);
}

// ---- idle / family change / sensor trouble reset or suspend ----
static void test_idle_family_change_and_degraded_reset() {
  EquipmentHealth eh;
  uint32_t t = 1000;
  eh.step(cool(23.0f), t);
  // Goes idle mid-window: episode over, verdict never forms.
  EquipmentHealthInputs idle;
  eh.step(idle, t + 600);
  TEST_ASSERT_FALSE(eh.step(cool(23.0f), t + kEffectMinRunS + 1).ineffective);

  // Degraded fusion suspends: no verdict even after a full window.
  EquipmentHealth eh2;
  EquipmentHealthInputs deg = cool(23.0f);
  deg.roomDegraded = true;
  eh2.step(deg, t);
  TEST_ASSERT_FALSE(eh2.step(deg, t + 2 * kEffectMinRunS).ineffective);

  // Family flip restarts the clock.
  EquipmentHealth eh3;
  eh3.step(cool(23.0f), t);
  eh3.step(heat(23.0f), t + kEffectMinRunS - 10);
  TEST_ASSERT_FALSE(eh3.step(heat(23.0f), t + kEffectMinRunS + 10).ineffective);
}

// ---- a latched verdict clears when progress finally happens ----
static void test_verdict_clears_on_progress() {
  EquipmentHealth eh;
  uint32_t t = 1000;
  eh.step(cool(23.0f), t);
  TEST_ASSERT_TRUE(eh.step(cool(23.1f), t + kEffectMinRunS).ineffective);
  // Outdoor unit recovers (the real incident's ending): room starts falling.
  TEST_ASSERT_FALSE(
      eh.step(cool(23.1f - kEffectMinProgressC - 0.05f), t + kEffectMinRunS + 900)
          .ineffective);
}

// ---- heating mirror ----
static void test_heating_mirror() {
  EquipmentHealth eh;
  uint32_t t = 1000;
  eh.step(heat(18.0f), t);
  TEST_ASSERT_TRUE(eh.step(heat(17.8f), t + kEffectMinRunS).ineffective);

  EquipmentHealth eh2;
  EquipmentHealthInputs in = heat(18.0f);
  in.supplyValid = true;
  in.supplyC = 18.0f + kEffectHeatSupplyDeltaC + 5.0f;  // hot supply = delivering
  eh2.step(in, t);
  TEST_ASSERT_FALSE(eh2.step(in, t + 2 * kEffectMinRunS).ineffective);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_cooling_no_progress_fires_at_deadline);
  RUN_TEST(test_cooling_progress_latches_no_alarm);
  RUN_TEST(test_supply_healthy_suppresses_trend);
  RUN_TEST(test_supply_failed_fires_early);
  RUN_TEST(test_setpoint_change_resets_window);
  RUN_TEST(test_idle_family_change_and_degraded_reset);
  RUN_TEST(test_verdict_clears_on_progress);
  RUN_TEST(test_heating_mirror);
  return UNITY_END();
}
