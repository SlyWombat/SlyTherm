// LatencyStats.h — fixed-memory latency histogram for the CT-485 TX-turnaround
// jitter probe (issue #28, Phase 3 bench gate).
//
// A rolling accumulator over microsecond latency samples: exact count / min /
// max / mean plus a fixed, log-ish bucket histogram from which p50/p95/p99/p999
// are read. No heap, no Arduino, no floating clock — pure C++17, so the whole
// thing is host-unit-testable (that is where the #28 verdict is read from).
//
// Design notes:
//   * min/max/mean are kept EXACT (max is the worst-case number the gate cares
//     about; never bucket-rounded).
//   * Percentiles come from the histogram. Bucket edges are hand-chosen and
//     log-ish: fine (25 us) at the low end where the compute turnaround lives,
//     coarsening through the ms range, spanning to 1 s with a final overflow
//     bucket. A percentile returns its bucket's UPPER edge (conservative — it
//     never under-reports latency, the right bias for a pass/fail gate),
//     clamped to the exact [min,max] so it is never absurd for tiny samples.
//   * add() is a short linear scan over the edge table (~24 entries); the probe
//     feeds it only on token grants (~once / 3.2 s), so cost is irrelevant.
//
// Pure C++17, no dependencies.

#pragma once
#include <cstddef>
#include <cstdint>

namespace slytherm {

class LatencyStats {
 public:
  // Bucket upper edges in microseconds. Bucket i covers (edge[i-1], edge[i]];
  // bucket 0 is [0, edge[0]]. A sample above the last edge lands in the final
  // overflow bucket. Fine at the low end (the tens-of-us compute regime),
  // log-ish through the ms range, out to 1 s.
  static constexpr uint32_t kEdges[] = {
      25,     50,     75,     100,    150,    200,    300,    400,
      500,    750,    1000,   1500,   2000,   3000,   5000,   7500,
      10000,  20000,  50000,  100000, 250000, 500000, 1000000,
  };
  static constexpr size_t kEdgeCount   = sizeof(kEdges) / sizeof(kEdges[0]);
  static constexpr size_t kBucketCount = kEdgeCount + 1;  // + overflow

  void add(uint32_t us) {
    if (count_ == 0 || us < min_) min_ = us;
    if (count_ == 0 || us > max_) max_ = us;
    count_++;
    sum_ += us;
    buckets_[bucketOf(us)]++;
  }

  void reset() {
    count_ = 0;
    min_ = max_ = 0;
    sum_ = 0;
    for (auto& b : buckets_) b = 0;
  }

  uint32_t count() const { return count_; }
  uint32_t min() const { return count_ ? min_ : 0; }
  uint32_t max() const { return count_ ? max_ : 0; }
  uint32_t mean() const {
    return count_ ? static_cast<uint32_t>((sum_ + count_ / 2) / count_) : 0;
  }

  // q in [0,1]. Nearest-rank percentile from the histogram, returned as the
  // enclosing bucket's UPPER edge (conservative), clamped into [min,max].
  // Empty -> 0.
  uint32_t percentile(float q) const {
    if (count_ == 0) return 0;
    if (q < 0.0f) q = 0.0f;
    if (q > 1.0f) q = 1.0f;
    // Rank of the target sample, 1-based (ceil), clamped to [1,count].
    uint32_t rank = static_cast<uint32_t>(q * static_cast<float>(count_) + 0.9999f);
    if (rank < 1) rank = 1;
    if (rank > count_) rank = count_;
    uint32_t cum = 0;
    for (size_t i = 0; i < kBucketCount; i++) {
      cum += buckets_[i];
      if (cum >= rank) return clamp(upperEdge(i));
    }
    return max_;  // unreachable (cum ends at count_ >= rank)
  }

 private:
  static size_t bucketOf(uint32_t us) {
    for (size_t i = 0; i < kEdgeCount; i++)
      if (us <= kEdges[i]) return i;
    return kEdgeCount;  // overflow bucket
  }
  // Upper edge of bucket i; the overflow bucket has no finite edge -> max_.
  uint32_t upperEdge(size_t i) const {
    return i < kEdgeCount ? kEdges[i] : max_;
  }
  uint32_t clamp(uint32_t v) const {
    if (v < min_) return min_;
    if (v > max_) return max_;
    return v;
  }

  uint32_t buckets_[kBucketCount] = {};
  uint32_t count_ = 0;
  uint32_t min_ = 0;
  uint32_t max_ = 0;
  uint64_t sum_ = 0;
};

}  // namespace slytherm
