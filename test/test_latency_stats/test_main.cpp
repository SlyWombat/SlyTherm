// LatencyStats unit tests (issue #28): the percentile/min/max/mean math the
// TX-turnaround jitter verdict is read from. Feeds known sample sets and
// asserts the reported percentiles land on the expected bucket edges, plus the
// degenerate cases (empty / single / all-equal / overflow).
#include <unity.h>

#include "LatencyStats.h"

using slytherm::LatencyStats;

void setUp() {}
void tearDown() {}

static void addN(LatencyStats& s, uint32_t us, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) s.add(us);
}

// Empty accumulator: everything reads 0, no divide-by-zero.
static void test_empty() {
  LatencyStats s;
  TEST_ASSERT_EQUAL_UINT32(0, s.count());
  TEST_ASSERT_EQUAL_UINT32(0, s.min());
  TEST_ASSERT_EQUAL_UINT32(0, s.max());
  TEST_ASSERT_EQUAL_UINT32(0, s.mean());
  TEST_ASSERT_EQUAL_UINT32(0, s.percentile(0.5f));
  TEST_ASSERT_EQUAL_UINT32(0, s.percentile(0.99f));
}

// Single sample: min=max=mean=value; every percentile clamps to it.
static void test_single() {
  LatencyStats s;
  s.add(42);
  TEST_ASSERT_EQUAL_UINT32(1, s.count());
  TEST_ASSERT_EQUAL_UINT32(42, s.min());
  TEST_ASSERT_EQUAL_UINT32(42, s.max());
  TEST_ASSERT_EQUAL_UINT32(42, s.mean());
  // Bucket (25,50] upper edge is 50 but the clamp to exact max pulls it to 42.
  TEST_ASSERT_EQUAL_UINT32(42, s.percentile(0.5f));
  TEST_ASSERT_EQUAL_UINT32(42, s.percentile(1.0f));
}

// All-equal samples: percentiles all equal that value (clamped to exact max).
static void test_all_equal() {
  LatencyStats s;
  addN(s, 60, 1000);
  TEST_ASSERT_EQUAL_UINT32(1000, s.count());
  TEST_ASSERT_EQUAL_UINT32(60, s.min());
  TEST_ASSERT_EQUAL_UINT32(60, s.max());
  TEST_ASSERT_EQUAL_UINT32(60, s.mean());
  TEST_ASSERT_EQUAL_UINT32(60, s.percentile(0.50f));
  TEST_ASSERT_EQUAL_UINT32(60, s.percentile(0.99f));
  TEST_ASSERT_EQUAL_UINT32(60, s.percentile(0.999f));
}

// A known distribution: 90 x 80us, 9 x 900us, 1 x 40000us.
// count=100. Ranks: p50->rank50 (in the 80us group), p95->rank95 (in the
// 900us group), p99->rank99 (900us group), p999->rank100 (the 40000us tail).
static void test_percentile_buckets() {
  LatencyStats s;
  addN(s, 80, 90);      // bucket (75,100]  upper edge 100
  addN(s, 900, 9);      // bucket (750,1000] upper edge 1000
  addN(s, 40000, 1);    // bucket (20000,50000] upper edge 50000 -> clamp max 40000
  TEST_ASSERT_EQUAL_UINT32(100, s.count());
  TEST_ASSERT_EQUAL_UINT32(80, s.min());
  TEST_ASSERT_EQUAL_UINT32(40000, s.max());

  // p50 falls in the 80us group -> bucket edge 100.
  TEST_ASSERT_EQUAL_UINT32(100, s.percentile(0.50f));
  // p90 = rank 90, still the last of the 80us group -> edge 100.
  TEST_ASSERT_EQUAL_UINT32(100, s.percentile(0.90f));
  // p95 = rank 95 -> inside the 900us group -> edge 1000.
  TEST_ASSERT_EQUAL_UINT32(1000, s.percentile(0.95f));
  // p99 = rank 99 -> last of the 900us group -> edge 1000.
  TEST_ASSERT_EQUAL_UINT32(1000, s.percentile(0.99f));
  // p999 = rank 100 -> the tail sample -> overflow-of-group bucket clamped to
  // the exact max (40000), never the bucket's nominal 50000 edge.
  TEST_ASSERT_EQUAL_UINT32(40000, s.percentile(0.999f));
  TEST_ASSERT_EQUAL_UINT32(40000, s.percentile(1.0f));
}

// Values above the last finite edge (1 s) land in the overflow bucket and are
// reported at the exact max.
static void test_overflow_bucket() {
  LatencyStats s;
  addN(s, 100, 99);
  s.add(3000000);  // 3 s: beyond the 1 s top edge
  TEST_ASSERT_EQUAL_UINT32(3000000, s.max());
  TEST_ASSERT_EQUAL_UINT32(3000000, s.percentile(1.0f));
  // The bulk percentile is unaffected by the single outlier.
  TEST_ASSERT_EQUAL_UINT32(100, s.percentile(0.50f));
}

// Boundary semantics: a sample exactly on an edge belongs to that edge's
// bucket (edge is inclusive upper bound).
static void test_edge_inclusive() {
  LatencyStats s;
  addN(s, 100, 100);  // exactly the (75,100] upper edge
  TEST_ASSERT_EQUAL_UINT32(100, s.percentile(0.50f));
  TEST_ASSERT_EQUAL_UINT32(100, s.max());
}

// reset() returns to the empty state and stats recompute cleanly.
static void test_reset() {
  LatencyStats s;
  addN(s, 500, 50);
  s.reset();
  TEST_ASSERT_EQUAL_UINT32(0, s.count());
  TEST_ASSERT_EQUAL_UINT32(0, s.percentile(0.99f));
  s.add(70);
  TEST_ASSERT_EQUAL_UINT32(1, s.count());
  TEST_ASSERT_EQUAL_UINT32(70, s.max());
}

// mean() rounds to nearest and never divides by zero.
static void test_mean_rounding() {
  LatencyStats s;
  s.add(10);
  s.add(11);  // (10+11)/2 = 10.5 -> rounds to 11
  TEST_ASSERT_EQUAL_UINT32(11, s.mean());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_empty);
  RUN_TEST(test_single);
  RUN_TEST(test_all_equal);
  RUN_TEST(test_percentile_buckets);
  RUN_TEST(test_overflow_bucket);
  RUN_TEST(test_edge_inclusive);
  RUN_TEST(test_reset);
  RUN_TEST(test_mean_rounding);
  return UNITY_END();
}
