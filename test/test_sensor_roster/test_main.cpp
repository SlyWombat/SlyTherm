// #155: sensor roster resolution — a slot resolves by wire id OR friendly name,
// so the Sensors-tab participation toggle (which passes the display name) is no
// longer a silent no-op for friendly-named sensors.
#include <unity.h>

#include "SensorRoster.h"

void setUp() {}
void tearDown() {}

// Mirrors main_thermostat's SensorEntry fields used by resolution.
namespace {
constexpr size_t kNameLen = 24;
struct Slot {
  bool used = false;
  char name[kNameLen] = {};  // wire id, e.g. "living"
  char disp[kNameLen] = {};  // #85 friendly label, e.g. "Living Room"
  bool inRoster = false;
};

Slot makeSlot(const char* id, const char* friendly, bool inRoster) {
  Slot s;
  s.used = true;
  std::strncpy(s.name, id, kNameLen - 1);
  std::strncpy(s.disp, friendly, kNameLen - 1);
  s.inRoster = inRoster;
  return s;
}
}  // namespace

// slotMatches: the matcher at the heart of the fix.
static void test_matches_by_id() {
  TEST_ASSERT_TRUE(slyroster::slotMatches("living", "Living Room", "living", kNameLen));
}
static void test_matches_by_friendly_name() {
  TEST_ASSERT_TRUE(slyroster::slotMatches("living", "Living Room", "Living Room", kNameLen));
}
static void test_no_false_match() {
  TEST_ASSERT_FALSE(slyroster::slotMatches("living", "Living Room", "basement", kNameLen));
}
static void test_empty_disp_falls_back_to_id() {
  // id-only slots keep old behavior: id matches, the (absent) friendly does not.
  TEST_ASSERT_TRUE(slyroster::slotMatches("living", "", "living", kNameLen));
  TEST_ASSERT_FALSE(slyroster::slotMatches("living", "", "Living Room", kNameLen));
}

// findSlot: the exact resolution findSensor runs over the table.
static void test_findslot_resolves_by_id_and_friendly_name() {
  Slot table[3];
  table[0] = Slot{};  // slot 0 is the local sensor; resolution starts at 1
  table[1] = makeSlot("living", "Living Room", false);
  table[2] = makeSlot("basement", "Basement", false);

  // By wire id (the cmd/sensor/<id>/... path).
  TEST_ASSERT_EQUAL_UINT8(1, slyroster::findSlot(table, 1, 3, "living", kNameLen));
  TEST_ASSERT_EQUAL_UINT8(2, slyroster::findSlot(table, 1, 3, "basement", kNameLen));
  // By friendly name (the Sensors-tab panel path — the #155 bug).
  TEST_ASSERT_EQUAL_UINT8(1, slyroster::findSlot(table, 1, 3, "Living Room", kNameLen));
  TEST_ASSERT_EQUAL_UINT8(2, slyroster::findSlot(table, 1, 3, "Basement", kNameLen));
  // Unknown resolves to the not-found sentinel.
  TEST_ASSERT_EQUAL_UINT8(slyroster::kNotFound,
                          slyroster::findSlot(table, 1, 3, "garage", kNameLen));
}

// End-to-end of the toggle: resolve a friendly-named sensor, flip inRoster.
static void test_friendly_named_sensor_toggles_in_roster() {
  Slot table[2];
  table[0] = Slot{};
  table[1] = makeSlot("living", "Living Room", false);

  uint8_t idx = slyroster::findSlot(table, 1, 2, "Living Room", kNameLen);
  TEST_ASSERT_NOT_EQUAL_UINT8(slyroster::kNotFound, idx);  // pre-fix: was kNotFound -> no-op
  table[idx].inRoster = !table[idx].inRoster;
  TEST_ASSERT_TRUE(table[1].inRoster);
  // and back off
  idx = slyroster::findSlot(table, 1, 2, "Living Room", kNameLen);
  table[idx].inRoster = !table[idx].inRoster;
  TEST_ASSERT_FALSE(table[1].inRoster);
}

// Unused slots never resolve even if their stale name would match.
static void test_unused_slot_skipped() {
  Slot table[2];
  table[0] = Slot{};
  table[1] = makeSlot("living", "Living Room", true);
  table[1].used = false;
  TEST_ASSERT_EQUAL_UINT8(slyroster::kNotFound,
                          slyroster::findSlot(table, 1, 2, "living", kNameLen));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_matches_by_id);
  RUN_TEST(test_matches_by_friendly_name);
  RUN_TEST(test_no_false_match);
  RUN_TEST(test_empty_disp_falls_back_to_id);
  RUN_TEST(test_findslot_resolves_by_id_and_friendly_name);
  RUN_TEST(test_friendly_named_sensor_toggles_in_roster);
  RUN_TEST(test_unused_slot_skipped);
  return UNITY_END();
}
