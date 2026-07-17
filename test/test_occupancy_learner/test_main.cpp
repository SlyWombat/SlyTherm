// OccupancyLearner (#177) — learns a typical occupancy pattern and proposes a
// SUGGESTED schedule (never auto-applies). Tests the EMA convergence, the
// hysteresis classification, weekday/weekend separation, and NVS roundtrip.
#include <unity.h>

#include "OccupancyLearner.h"

using dettson::OccupancyLearner;

void setUp() {}
void tearDown() {}

namespace {
// Feed a whole day of hourly occupied/away samples `days` times.
void feedDays(OccupancyLearner& l, bool weekend, const bool occ[24], int days) {
  for (int d = 0; d < days; ++d)
    for (uint8_t h = 0; h < 24; ++h) l.observe(weekend, h, occ[h]);
}
// Home/asleep 0-8, away at work 8-17, home 17-24.
const bool kWeekdayPattern[24] = {
  1,1,1,1,1,1,1,1, 0,0,0,0,0,0,0,0,0, 1,1,1,1,1,1,1};
}  // namespace

static void test_default_no_history() {
  OccupancyLearner l;
  TEST_ASSERT_EQUAL_UINT16(0, l.samples());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, l.occupiedFraction(false, 8));
  TEST_ASSERT_FALSE(l.suggestSchedule(false).confident);
}

static void test_ema_converges_toward_occupied_fraction() {
  OccupancyLearner l;
  for (int d = 0; d < 60; ++d) l.observe(false, 8, false);  // always away at 8am
  TEST_ASSERT_TRUE(l.occupiedFraction(false, 8) < 0.05f);
  for (int d = 0; d < 60; ++d) l.observe(false, 20, true);  // always home at 8pm
  TEST_ASSERT_TRUE(l.occupiedFraction(false, 20) > 0.95f);
}

static void test_suggested_schedule_matches_pattern() {
  OccupancyLearner l;
  feedDays(l, false, kWeekdayPattern, 30);
  const auto s = l.suggestSchedule(false);
  TEST_ASSERT_TRUE(s.confident);
  // Expect: start occupied(0) -> away(8) -> occupied(17).
  TEST_ASSERT_EQUAL_UINT8(3, s.count);
  TEST_ASSERT_EQUAL_UINT8(0, s.t[0].hour); TEST_ASSERT_TRUE(s.t[0].occupied);
  TEST_ASSERT_EQUAL_UINT8(8, s.t[1].hour); TEST_ASSERT_FALSE(s.t[1].occupied);
  TEST_ASSERT_EQUAL_UINT8(17, s.t[2].hour); TEST_ASSERT_TRUE(s.t[2].occupied);
}

static void test_weekday_and_weekend_are_separate() {
  OccupancyLearner l;
  feedDays(l, false, kWeekdayPattern, 30);        // weekday: away midday
  const bool weekendHome[24] = {                  // weekend: home all day
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
  feedDays(l, true, weekendHome, 30);
  TEST_ASSERT_TRUE(l.occupiedFraction(false, 12) < 0.1f);   // weekday noon: away
  TEST_ASSERT_TRUE(l.occupiedFraction(true, 12) > 0.9f);    // weekend noon: home
  // Weekend has no away window -> a single "occupied all day" transition.
  const auto s = l.suggestSchedule(true);
  TEST_ASSERT_EQUAL_UINT8(1, s.count);
  TEST_ASSERT_TRUE(s.t[0].occupied);
}

static void test_hysteresis_ignores_a_borderline_hour() {
  OccupancyLearner l;
  const bool allHome[24] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
  feedDays(l, false, allHome, 30);                // frac ~0.98 everywhere
  for (int i = 0; i < 5; ++i) l.observe(false, 12, false);  // pull noon into the band
  const float noon = l.occupiedFraction(false, 12);
  TEST_ASSERT_TRUE(noon > 0.35f && noon < 0.65f);           // genuinely borderline
  // A borderline hour inside the hysteresis band must not create a spurious
  // away/return pair — the day stays classified occupied throughout.
  const auto s = l.suggestSchedule(false);
  TEST_ASSERT_EQUAL_UINT8(1, s.count);
}

static void test_blob_roundtrip_preserves_pattern() {
  OccupancyLearner a;
  feedDays(a, false, kWeekdayPattern, 30);
  OccupancyLearner::PersistBlob blob;
  a.toBlob(blob);
  OccupancyLearner b;
  TEST_ASSERT_TRUE(b.fromBlob(blob));
  TEST_ASSERT_EQUAL_UINT16(a.samples(), b.samples());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, a.occupiedFraction(false, 12),
                                  b.occupiedFraction(false, 12));
  TEST_ASSERT_EQUAL_UINT8(a.suggestSchedule(false).count,
                          b.suggestSchedule(false).count);
}

static void test_blob_rejects_bad_magic_fails_open() {
  OccupancyLearner::PersistBlob blob;
  blob.magic = 0xDEADBEEF;
  OccupancyLearner l;
  TEST_ASSERT_FALSE(l.fromBlob(blob));
  TEST_ASSERT_EQUAL_UINT16(0, l.samples());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_default_no_history);
  RUN_TEST(test_ema_converges_toward_occupied_fraction);
  RUN_TEST(test_suggested_schedule_matches_pattern);
  RUN_TEST(test_weekday_and_weekend_are_separate);
  RUN_TEST(test_hysteresis_ignores_a_borderline_hour);
  RUN_TEST(test_blob_roundtrip_preserves_pattern);
  RUN_TEST(test_blob_rejects_bad_magic_fails_open);
  return UNITY_END();
}
