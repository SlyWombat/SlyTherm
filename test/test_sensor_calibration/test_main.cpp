// Sensor calibration (offset) persistence — decouples the user's HA-set
// temperature offset from roster membership so a RETAINED-roster replay
// (reboot / MQTT reconnect) can no longer clobber it back to 0.0 (#164).
//
// The regression at the heart of this suite: handleSensorRoster used to do
// `s.offsetC = e.offsetC` (the roster's 0.0 default) on every replay. These
// tests exercise the shared merge helper (applyRosterOffset) so a revert to
// that clobber fails here.
#include <unity.h>

#include "SensorCalibration.h"

using dettson::SensorCalibration;

void setUp() {}
void tearDown() {}

namespace {
// Mirrors the one field main_thermostat's SensorEntry exposes to the merge.
struct Slot { float offsetC = 0.0f; };
}  // namespace

static void test_default_offset_is_zero_for_unknown_sensor() {
  SensorCalibration c;
  TEST_ASSERT_FALSE(c.has("living"));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, c.offsetFor("living"));
}

static void test_set_persists_and_reads_back() {
  SensorCalibration c;
  TEST_ASSERT_TRUE(c.set("living", -0.8f));
  TEST_ASSERT_TRUE(c.has("living"));
  TEST_ASSERT_EQUAL_FLOAT(-0.8f, c.offsetFor("living"));
}

static void test_set_zero_for_unknown_is_noop() {
  SensorCalibration c;
  TEST_ASSERT_FALSE(c.set("living", 0.0f));  // matches default; nothing stored
  TEST_ASSERT_FALSE(c.has("living"));
}

static void test_update_existing_offset_changes_and_zero_is_retained() {
  SensorCalibration c;
  c.set("living", -0.8f);
  TEST_ASSERT_TRUE(c.set("living", 0.3f));    // changed
  TEST_ASSERT_EQUAL_FLOAT(0.3f, c.offsetFor("living"));
  TEST_ASSERT_FALSE(c.set("living", 0.3f));   // idempotent
  // An explicit zeroing of a previously-set sensor is a real change and stays
  // stored (so a replay keeps the deliberate 0, not the roster default).
  TEST_ASSERT_TRUE(c.set("living", 0.0f));
  TEST_ASSERT_TRUE(c.has("living"));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, c.offsetFor("living"));
}

static void test_blob_roundtrip_preserves_offset() {
  SensorCalibration a;
  a.set("living", -0.8f);
  a.set("bedroom", 1.25f);
  SensorCalibration::PersistBlob blob;
  a.toBlob(blob);
  SensorCalibration b;
  TEST_ASSERT_TRUE(b.fromBlob(blob));
  TEST_ASSERT_EQUAL_FLOAT(-0.8f, b.offsetFor("living"));
  TEST_ASSERT_EQUAL_FLOAT(1.25f, b.offsetFor("bedroom"));
  TEST_ASSERT_FALSE(b.has("kitchen"));
}

static void test_blob_rejects_bad_magic_fails_open() {
  SensorCalibration::PersistBlob blob;
  blob.magic = 0xDEADBEEF;
  SensorCalibration c;
  TEST_ASSERT_FALSE(c.fromBlob(blob));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, c.offsetFor("living"));  // no bogus offset injected
}

// THE #164 regression: a roster replay must keep the user's stored offset and
// only fall back to the roster default for sensors the user never calibrated.
static void test_roster_replay_keeps_user_offset() {
  SensorCalibration c;
  c.set("living", -0.8f);  // user set this via HA

  // Roster replay: the retained roster carries offset 0.0 for every entry.
  Slot living, bedroom;
  dettson::applyRosterOffset(living,  "living",  0.0f, c);  // user override wins
  dettson::applyRosterOffset(bedroom, "bedroom", 0.0f, c);  // never set -> roster default

  TEST_ASSERT_EQUAL_FLOAT(-0.8f, living.offsetC);   // NOT clobbered to 0
  TEST_ASSERT_EQUAL_FLOAT(0.0f,  bedroom.offsetC);

  // A roster that DOES carry a default for an uncalibrated sensor is honored.
  Slot kitchen;
  dettson::applyRosterOffset(kitchen, "kitchen", 0.5f, c);
  TEST_ASSERT_EQUAL_FLOAT(0.5f, kitchen.offsetC);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_default_offset_is_zero_for_unknown_sensor);
  RUN_TEST(test_set_persists_and_reads_back);
  RUN_TEST(test_set_zero_for_unknown_is_noop);
  RUN_TEST(test_update_existing_offset_changes_and_zero_is_retained);
  RUN_TEST(test_blob_roundtrip_preserves_offset);
  RUN_TEST(test_blob_rejects_bad_magic_fails_open);
  RUN_TEST(test_roster_replay_keeps_user_offset);
  return UNITY_END();
}
