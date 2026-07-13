// Sensor participation persistence — decouples a user's "include this room"
// choice from roster membership so a RETAINED-roster replay (reboot / MQTT
// reconnect) can no longer clobber a sensor the user turned OFF.
//
// The regression at the heart of this suite: handleSensorRoster used to force
// inRoster=true for every roster entry, conflating membership with
// participation. These tests exercise the shared merge helper
// (applyRosterMember) so a revert to "force true" fails here.
#include <unity.h>

#include "SensorParticipation.h"

using dettson::SensorParticipation;

void setUp() {}
void tearDown() {}

// Mirrors the fields main_thermostat's SensorEntry exposes to the merge.
namespace {
constexpr size_t kNameLen = SensorParticipation::kIdLen;
struct Slot {
  bool used = false;
  char name[kNameLen] = {};   // wire id, e.g. "basement"
  char disp[kNameLen] = {};   // #85 friendly label, e.g. "Basement"
  bool inRoster = false;
  bool participating = true;
};

Slot makeSlot(const char* id, const char* friendly) {
  Slot s;
  s.used = true;
  std::strncpy(s.name, id, kNameLen - 1);
  std::strncpy(s.disp, friendly, kNameLen - 1);
  return s;
}

// Faithful re-enactment of handleSensorRoster's core loop: clear all roster
// membership, then for each roster id mark membership AND apply the persisted
// participation (never force true). Returns nothing; mutates the table.
template <size_t N>
void replayRoster(Slot (&table)[N], const char* const* rosterIds, size_t n,
                  const SensorParticipation& store) {
  for (size_t i = 0; i < N; ++i) table[i].inRoster = false;  // "clear all"
  for (size_t r = 0; r < n; ++r) {
    for (size_t i = 0; i < N; ++i) {
      if (table[i].used && std::strncmp(table[i].name, rosterIds[r], kNameLen) == 0) {
        dettson::applyRosterMember(table[i], table[i].name, store);
        break;
      }
    }
  }
}
}  // namespace

// ---- Store semantics ----------------------------------------------------

static void test_default_is_participating_for_unknown_sensor() {
  SensorParticipation store;
  TEST_ASSERT_TRUE(store.participating("basement"));  // never toggled -> ON
  TEST_ASSERT_TRUE(store.participating("living"));
}

static void test_set_off_persists_and_reads_back() {
  SensorParticipation store;
  TEST_ASSERT_TRUE(store.set("basement", false));   // changed default -> OFF
  TEST_ASSERT_FALSE(store.participating("basement"));
  TEST_ASSERT_TRUE(store.participating("living"));   // unrelated stays default
}

static void test_set_on_over_default_is_noop() {
  SensorParticipation store;
  // Turning ON a never-toggled sensor is not a change (already ON) -> no persist.
  TEST_ASSERT_FALSE(store.set("basement", true));
  TEST_ASSERT_TRUE(store.participating("basement"));
}

static void test_toggle_off_then_on_returns_to_participating() {
  SensorParticipation store;
  TEST_ASSERT_TRUE(store.set("basement", false));
  TEST_ASSERT_FALSE(store.participating("basement"));
  TEST_ASSERT_TRUE(store.set("basement", true));    // OFF -> ON is a change
  TEST_ASSERT_TRUE(store.participating("basement"));
  TEST_ASSERT_FALSE(store.set("basement", true));   // idempotent now
}

// ---- NVS round-trip (restore-on-boot) -----------------------------------

static void test_blob_roundtrip_preserves_off_choice() {
  SensorParticipation store;
  store.set("basement", false);
  store.set("attic", false);

  SensorParticipation::PersistBlob blob;
  store.toBlob(blob);

  // Fresh instance restores from the blob — simulates a boot.
  SensorParticipation restored;
  TEST_ASSERT_TRUE(restored.fromBlob(blob));
  TEST_ASSERT_FALSE(restored.participating("basement"));
  TEST_ASSERT_FALSE(restored.participating("attic"));
  TEST_ASSERT_TRUE(restored.participating("living"));  // untouched -> default ON
}

static void test_blob_rejects_bad_magic_fails_open() {
  SensorParticipation::PersistBlob blob;  // zero magic (absent / corrupt NVS)
  SensorParticipation store;
  TEST_ASSERT_FALSE(store.fromBlob(blob));
  TEST_ASSERT_TRUE(store.participating("basement"));  // fails open to ON
}

// ---- The regression: a roster replay must NOT clobber an OFF sensor -----

static void test_roster_replay_keeps_off_sensor_off() {
  // Boot state: basement was turned OFF and that choice is persisted/restored.
  SensorParticipation store;
  store.set("basement", false);

  Slot table[SensorParticipation::kMaxEntries];
  table[0] = makeSlot("basement", "Basement");
  table[1] = makeSlot("living", "Living Room");

  const char* roster[] = {"basement", "living"};

  // HA replays the retained roster twice (reboot, then an overnight reconnect).
  for (int replay = 0; replay < 2; ++replay) {
    replayRoster(table, roster, 2, store);
    // basement is a roster MEMBER but must stay opted OUT of fusion.
    TEST_ASSERT_TRUE(table[0].inRoster);
    TEST_ASSERT_FALSE(table[0].participating);
    TEST_ASSERT_FALSE(dettson::fusionParticipates(table[0].inRoster, table[0].participating));
    // living was never toggled -> participates.
    TEST_ASSERT_TRUE(table[1].inRoster);
    TEST_ASSERT_TRUE(dettson::fusionParticipates(table[1].inRoster, table[1].participating));
  }
}

// #155: the panel toggle hands the DISPLAY name; persistence must key by the
// resolved WIRE id, or a replay (which keys by id) misses and clobbers.
static void test_toggle_by_friendly_name_persists_under_wire_id() {
  Slot table[SensorParticipation::kMaxEntries];
  table[0] = makeSlot("basement", "Basement");
  table[1] = makeSlot("living", "Living Room");

  SensorParticipation store;
  // Simulate uiToggleSensor("Living Room"): resolve disp -> slot, persist the
  // WIRE id (table[i].name), flip participating.
  const char* toggled = "Living Room";
  int idx = -1;
  for (size_t i = 0; i < SensorParticipation::kMaxEntries; ++i)
    if (table[i].used && (std::strcmp(table[i].name, toggled) == 0 ||
                          std::strcmp(table[i].disp, toggled) == 0)) { idx = (int)i; break; }
  TEST_ASSERT_EQUAL_INT(1, idx);
  store.set(table[idx].name, false);  // keyed by "living", NOT "Living Room"

  TEST_ASSERT_FALSE(store.participating("living"));
  TEST_ASSERT_TRUE(store.participating("Living Room"));  // display name is not a key

  // A roster replay (keyed by wire id) keeps it OFF.
  const char* roster[] = {"basement", "living"};
  replayRoster(table, roster, 2, store);
  TEST_ASSERT_TRUE(table[1].inRoster);
  TEST_ASSERT_FALSE(table[1].participating);
}

// A sensor absent from the roster is never fused, regardless of its stored
// participation (can't fuse a sensor you don't know).
static void test_non_roster_sensor_stays_out() {
  SensorParticipation store;  // "attic" defaults ON in the store

  Slot table[SensorParticipation::kMaxEntries];
  table[0] = makeSlot("living", "Living Room");
  table[1] = makeSlot("attic", "Attic");

  const char* roster[] = {"living"};  // attic dropped from roster
  replayRoster(table, roster, 1, store);

  TEST_ASSERT_FALSE(table[1].inRoster);
  TEST_ASSERT_TRUE(table[1].participating);  // store still says ON...
  // ...but the fusion gate keeps it out because it's not a roster member.
  TEST_ASSERT_FALSE(dettson::fusionParticipates(table[1].inRoster, table[1].participating));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_default_is_participating_for_unknown_sensor);
  RUN_TEST(test_set_off_persists_and_reads_back);
  RUN_TEST(test_set_on_over_default_is_noop);
  RUN_TEST(test_toggle_off_then_on_returns_to_participating);
  RUN_TEST(test_blob_roundtrip_preserves_off_choice);
  RUN_TEST(test_blob_rejects_bad_magic_fails_open);
  RUN_TEST(test_roster_replay_keeps_off_sensor_off);
  RUN_TEST(test_toggle_by_friendly_name_persists_under_wire_id);
  RUN_TEST(test_non_roster_sensor_stays_out);
  return UNITY_END();
}
