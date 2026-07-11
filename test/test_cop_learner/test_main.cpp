// CopLearner tests (issue #143, docs/13 §5): per-OAT-bucket accounting for
// the record-only degree-days-per-runtime-hour proxy — edge semantics (idle
// -> active transitions attribute no dt), gap capping, regime/validity gates,
// bucket placement, proxy arithmetic, NVS blob round-trip + corruption
// rejection, and the JSON telemetry shape.
#include <unity.h>
#include <cmath>
#include <cstring>
#include <string>

#include "CopLearner.h"
#include "DettsonConfig.h"

using namespace dettson;

void setUp() {}
void tearDown() {}

static bool has(const std::string& s, const char* sub) {
  return s.find(sub) != std::string::npos;
}

static size_t bucketIndexFor(float oatC) {
  // Mirror of the documented layout: kCopBucketMinOatC + i*kCopBucketWidthC.
  float rel = (oatC - kCopBucketMinOatC) / kCopBucketWidthC;
  if (rel < 0) rel = 0;
  size_t i = static_cast<size_t>(rel);
  return i >= kCopBucketCount ? kCopBucketCount - 1 : i;
}

// Run `seconds` of steady HP heating at 1 Hz ticks. Returns the next nowS.
static uint32_t runSteady(CopLearner& cl, float indoorC, float oatC,
                          uint32_t fromS, uint32_t seconds) {
  uint32_t t = fromS;
  for (uint32_t i = 0; i <= seconds; ++i, ++t)
    cl.tick(true, indoorC, true, oatC, true, t);
  return t;
}

static void test_accumulates_into_the_right_bucket() {
  CopLearner cl;
  // 1 h at indoor 21 / OAT -12: bucket lower edge -12, runtime 3600 s,
  // degree-hours = 33 degC * 1 h.
  runSteady(cl, 21.0f, -12.0f, 1000, 3600);
  const size_t b = bucketIndexFor(-12.0f);
  const CopLearner::BucketView v = cl.bucket(b);
  TEST_ASSERT_EQUAL_UINT32(3600, v.runtimeS);
  TEST_ASSERT_EQUAL_FLOAT(-12.0f, v.oatLoC);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 33.0f, v.degreeHours);
  // Proxy: degree-days / runtime-hours = (33/24) / 1.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 33.0f / 24.0f, v.ddPerRunHour);
  TEST_ASSERT_EQUAL_UINT32(3600, cl.totalRuntimeS());
  // Neighbouring buckets untouched.
  TEST_ASSERT_EQUAL_UINT32(0, cl.bucket(b - 1).runtimeS);
  TEST_ASSERT_EQUAL_UINT32(0, cl.bucket(b + 1).runtimeS);
}

static void test_first_active_tick_is_an_edge_not_runtime() {
  CopLearner cl;
  cl.tick(false, 21.0f, true, -5.0f, true, 100);   // idle
  cl.tick(true, 21.0f, true, -5.0f, true, 700);    // HP starts: edge only
  TEST_ASSERT_EQUAL_UINT32(0, cl.totalRuntimeS()); // the 600 s idle gap never counts
  cl.tick(true, 21.0f, true, -5.0f, true, 710);    // active -> active: 10 s
  TEST_ASSERT_EQUAL_UINT32(10, cl.totalRuntimeS());
}

static void test_gap_between_active_ticks_is_capped() {
  CopLearner cl;
  cl.tick(true, 21.0f, true, -5.0f, true, 1000);
  cl.tick(true, 21.0f, true, -5.0f, true, 1001);
  TEST_ASSERT_EQUAL_UINT32(1, cl.totalRuntimeS());
  // A stalled loop / clock jump: 900 s between active ticks credits only
  // kCopTickMaxGapS, never phantom hours.
  cl.tick(true, 21.0f, true, -5.0f, true, 1901);
  TEST_ASSERT_EQUAL_UINT32(1 + kCopTickMaxGapS, cl.totalRuntimeS());
}

static void test_invalid_or_inverted_inputs_never_accumulate() {
  CopLearner cl;
  uint32_t t = 500;
  for (int i = 0; i < 100; ++i, ++t) cl.tick(true, 21.0f, false, -5.0f, true, t);
  for (int i = 0; i < 100; ++i, ++t) cl.tick(true, 21.0f, true, -5.0f, false, t);
  // Shoulder-season inversion (indoor <= OAT) is not a heating regime.
  for (int i = 0; i < 100; ++i, ++t) cl.tick(true, 21.0f, true, 25.0f, true, t);
  // NaN never passes the gates.
  for (int i = 0; i < 100; ++i, ++t) cl.tick(true, NAN, true, -5.0f, true, t);
  TEST_ASSERT_EQUAL_UINT32(0, cl.totalRuntimeS());
  // And a validity dropout mid-run is an edge on resume, not a bridge.
  cl.tick(true, 21.0f, true, -5.0f, true, t);
  cl.tick(true, 21.0f, false, -5.0f, true, t + 1);
  cl.tick(true, 21.0f, true, -5.0f, true, t + 2);   // resume: edge
  cl.tick(true, 21.0f, true, -5.0f, true, t + 3);
  TEST_ASSERT_EQUAL_UINT32(1, cl.totalRuntimeS());  // only the final 1 s step
}

static void test_out_of_range_oat_clamps_to_edge_buckets() {
  CopLearner cl;
  uint32_t t = runSteady(cl, 18.0f, -45.0f, 0, 60);    // below the span
  cl.tick(false, 18.0f, true, -45.0f, true, t);        // idle: cycle boundary
  runSteady(cl, 21.0f, 19.5f, t + 100, 60);            // above the span (indoor > oat)
  TEST_ASSERT_EQUAL_UINT32(60, cl.bucket(0).runtimeS);
  TEST_ASSERT_EQUAL_UINT32(60, cl.bucket(kCopBucketCount - 1).runtimeS);
}

static void test_persist_blob_round_trip() {
  CopLearner cl;
  uint32_t t = runSteady(cl, 21.0f, -12.0f, 0, 3600);
  cl.tick(false, 21.0f, true, -12.0f, true, t);  // idle: cycle boundary
  runSteady(cl, 20.0f, 2.0f, t + 100, 1800);

  CopLearner::PersistBlob blob;
  cl.save(&blob);
  CopLearner fresh;
  TEST_ASSERT_TRUE(fresh.restore(&blob));
  TEST_ASSERT_EQUAL_UINT32(cl.totalRuntimeS(), fresh.totalRuntimeS());
  for (size_t i = 0; i < kCopBucketCount; ++i) {
    TEST_ASSERT_EQUAL_UINT32(cl.bucket(i).runtimeS, fresh.bucket(i).runtimeS);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, cl.bucket(i).degreeHours,
                             fresh.bucket(i).degreeHours);
  }
  // Restore is an accumulation edge: the first active tick after boot must
  // not bridge from the pre-reboot clock.
  fresh.tick(true, 21.0f, true, -12.0f, true, 5);
  TEST_ASSERT_EQUAL_UINT32(cl.totalRuntimeS(), fresh.totalRuntimeS());
}

static void test_restore_rejects_bad_version_and_corrupt_blob() {
  CopLearner cl;
  runSteady(cl, 21.0f, -12.0f, 0, 600);
  CopLearner::PersistBlob blob;
  cl.save(&blob);

  CopLearner fresh;
  CopLearner::PersistBlob wrong = blob;
  wrong.version = CopLearner::kBlobVersion + 1;
  TEST_ASSERT_FALSE(fresh.restore(&wrong));
  TEST_ASSERT_EQUAL_UINT32(0, fresh.totalRuntimeS());

  CopLearner::PersistBlob corrupt = blob;
  corrupt.degreeHours[3] = NAN;
  CopLearner poisoned;
  runSteady(poisoned, 21.0f, -12.0f, 0, 600);   // has prior state...
  TEST_ASSERT_FALSE(poisoned.restore(&corrupt));
  TEST_ASSERT_EQUAL_UINT32(0, poisoned.totalRuntimeS());  // ...starts fresh
}

static void test_proxy_json_shape() {
  CopLearner cl;
  TEST_ASSERT_EQUAL_STRING("{\"bucketC\":3,\"buckets\":[]}",
                           cl.proxyJson().c_str());
  runSteady(cl, 21.0f, -12.0f, 0, 3600);
  const std::string j = cl.proxyJson();
  TEST_ASSERT_TRUE(has(j, "\"bucketC\":3"));
  TEST_ASSERT_TRUE(has(j, "{\"oat\":-12,\"runS\":3600,\"degH\":33.0,\"ddph\":1.375}"));
  // Empty buckets are omitted.
  TEST_ASSERT_FALSE(has(j, "\"runS\":0"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_accumulates_into_the_right_bucket);
  RUN_TEST(test_first_active_tick_is_an_edge_not_runtime);
  RUN_TEST(test_gap_between_active_ticks_is_capped);
  RUN_TEST(test_invalid_or_inverted_inputs_never_accumulate);
  RUN_TEST(test_out_of_range_oat_clamps_to_edge_buckets);
  RUN_TEST(test_persist_blob_round_trip);
  RUN_TEST(test_restore_rejects_bad_version_and_corrupt_blob);
  RUN_TEST(test_proxy_json_shape);
  return UNITY_END();
}
