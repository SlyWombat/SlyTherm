// OutdoorTempSource tests (issue #34): rung ladder priority, staleness
// demotion/recovery, all-stale invalid, adjacent-rung disagreement alarm,
// range/NaN rejection, MQTT rung names.
#include <unity.h>
#include <cmath>
#include "OutdoorTempSource.h"
#include "DettsonConfig.h"

using namespace dettson;

void setUp() {}
void tearDown() {}

static void test_boot_no_data_invalid() {
  OutdoorTempSource src;
  OatReading r = src.read(0);
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_TRUE(r.rung == OatRung::kNone);
  TEST_ASSERT_FALSE(r.disagreeAlarm);
  TEST_ASSERT_EQUAL_STRING("none", oatRungName(r.rung));
}

static void test_priority_bus_wins() {
  OutdoorTempSource src;
  TEST_ASSERT_TRUE(src.submit(OatRung::kHaWeather, -3.0f, 100));
  TEST_ASSERT_TRUE(src.submit(OatRung::kWired, -4.0f, 100));
  TEST_ASSERT_TRUE(src.submit(OatRung::kBus, -5.0f, 100));
  OatReading r = src.read(110);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_TRUE(r.rung == OatRung::kBus);
  TEST_ASSERT_EQUAL_FLOAT(-5.0f, r.valueC);
  TEST_ASSERT_FALSE(r.disagreeAlarm);
}

static void test_staleness_demotion_ladder() {
  OutdoorTempSource src;
  src.submit(OatRung::kBus, -5.0f, 1000);
  src.submit(OatRung::kWired, -4.0f, 1000);
  src.submit(OatRung::kHaWeather, -3.0f, 1000);

  // Just inside the staleness window: bus still live.
  OatReading r = src.read(1000 + kOatStaleS - 1);
  TEST_ASSERT_TRUE(r.rung == OatRung::kBus);

  // Bus ages out (wired/ha refreshed): demote to wired.
  src.submit(OatRung::kWired, -4.0f, 1000 + kOatStaleS);
  src.submit(OatRung::kHaWeather, -3.0f, 1000 + kOatStaleS);
  r = src.read(1000 + kOatStaleS);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_TRUE(r.rung == OatRung::kWired);
  TEST_ASSERT_EQUAL_FLOAT(-4.0f, r.valueC);

  // Wired ages out too: demote to HA weather.
  src.submit(OatRung::kHaWeather, -3.5f, 1000 + 2 * kOatStaleS);
  r = src.read(1000 + 2 * kOatStaleS);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_TRUE(r.rung == OatRung::kHaWeather);
  TEST_ASSERT_EQUAL_FLOAT(-3.5f, r.valueC);

  // Everything stale: invalid (caller applies fail-cold per docs/04 §4).
  r = src.read(1000 + 3 * kOatStaleS);
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_TRUE(r.rung == OatRung::kNone);
}

static void test_recovery_promotes_back() {
  OutdoorTempSource src;
  src.submit(OatRung::kWired, -4.0f, 5000);
  OatReading r = src.read(5010);
  TEST_ASSERT_TRUE(r.rung == OatRung::kWired);

  // Bus comes (back) alive: immediately preferred.
  src.submit(OatRung::kBus, -5.0f, 5020);
  r = src.read(5030);
  TEST_ASSERT_TRUE(r.rung == OatRung::kBus);
  TEST_ASSERT_EQUAL_FLOAT(-5.0f, r.valueC);
}

static void test_disagreement_alarm_keeps_higher_rung() {
  OutdoorTempSource src;
  src.submit(OatRung::kBus, -5.0f, 100);
  src.submit(OatRung::kWired, -5.0f - kOatRungDisagreeAlarmC - 1.0f, 100);
  OatReading r = src.read(100);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_TRUE(r.disagreeAlarm);
  TEST_ASSERT_TRUE(r.rung == OatRung::kBus);  // higher-priority rung kept
  TEST_ASSERT_EQUAL_FLOAT(-5.0f, r.valueC);
}

static void test_disagreement_within_threshold_no_alarm() {
  OutdoorTempSource src;
  src.submit(OatRung::kBus, -5.0f, 100);
  src.submit(OatRung::kWired, -5.0f - kOatRungDisagreeAlarmC + 0.5f, 100);
  OatReading r = src.read(100);
  TEST_ASSERT_FALSE(r.disagreeAlarm);
}

static void test_disagreement_adjacent_live_skips_dead_rung() {
  // Bus stale -> wired and HA are the adjacent LIVE pair.
  OutdoorTempSource src;
  src.submit(OatRung::kBus, -5.0f, 0);
  src.submit(OatRung::kWired, -4.0f, 0 + kOatStaleS + 100);
  src.submit(OatRung::kHaWeather, -4.0f - kOatRungDisagreeAlarmC - 2.0f,
             0 + kOatStaleS + 100);
  OatReading r = src.read(0 + kOatStaleS + 100);
  TEST_ASSERT_TRUE(r.rung == OatRung::kWired);
  TEST_ASSERT_TRUE(r.disagreeAlarm);
  TEST_ASSERT_EQUAL_FLOAT(-4.0f, r.valueC);
}

static void test_range_and_nan_rejected() {
  OutdoorTempSource src;
  src.submit(OatRung::kWired, -4.0f, 100);
  // DS18B20 sentinels and NaN: dropped, last good sample stays in effect.
  TEST_ASSERT_FALSE(src.submit(OatRung::kWired, -127.0f, 200));
  TEST_ASSERT_FALSE(src.submit(OatRung::kWired, 85.0f, 200));
  TEST_ASSERT_FALSE(src.submit(OatRung::kWired, NAN, 200));
  TEST_ASSERT_FALSE(src.submit(OatRung::kNone, -4.0f, 200));
  OatReading r = src.read(200);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_EQUAL_FLOAT(-4.0f, r.valueC);
  // ...and the rejected samples did not refresh the timestamp: the rung
  // still ages out from the last GOOD sample.
  r = src.read(100 + kOatStaleS);
  TEST_ASSERT_FALSE(r.valid);
}

static void test_rung_names_match_topic_values() {
  // docs/06: slytherm/state/outdoor_source = bus / wired / ha / none.
  TEST_ASSERT_EQUAL_STRING("bus", oatRungName(OatRung::kBus));
  TEST_ASSERT_EQUAL_STRING("wired", oatRungName(OatRung::kWired));
  TEST_ASSERT_EQUAL_STRING("ha", oatRungName(OatRung::kHaWeather));
  TEST_ASSERT_EQUAL_STRING("none", oatRungName(OatRung::kNone));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_boot_no_data_invalid);
  RUN_TEST(test_priority_bus_wins);
  RUN_TEST(test_staleness_demotion_ladder);
  RUN_TEST(test_recovery_promotes_back);
  RUN_TEST(test_disagreement_alarm_keeps_higher_rung);
  RUN_TEST(test_disagreement_within_threshold_no_alarm);
  RUN_TEST(test_disagreement_adjacent_live_skips_dead_rung);
  RUN_TEST(test_range_and_nan_rejected);
  RUN_TEST(test_rung_names_match_topic_values);
  return UNITY_END();
}
