#pragma once
// OccupancyLearner — learns the home's typical occupancy pattern and proposes a
// SUGGESTED schedule (#177, competitive P4). Deliberately advisory: it never
// changes control on its own — the firmware surfaces the suggestion for the
// owner to review/accept (the auto-apply UX is a documented follow-on).
//
// Model: one occupied/away sample per (day-type, hour), EMA'd across days so the
// bucket settles to "fraction of days occupied at this hour". Two day-types
// (weekday/weekend) because home patterns differ. suggestSchedule() classifies
// each hour occupied/away against a threshold (with hysteresis so a single noisy
// hour doesn't flip it) and returns the transition hours — the natural
// wake/leave/return/sleep boundaries.
//
// Persistence: a compact POD blob (fractions quantized to uint8) written raw to
// NVS, mirroring SensorParticipation/CopLearner. Pure C++17 (unit-tested).

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dettson {

class OccupancyLearner {
 public:
  static constexpr uint8_t  kDayTypes = 2;   // 0 = weekday, 1 = weekend
  static constexpr uint8_t  kHours    = 24;
  static constexpr uint32_t kMagic    = 0x4F434350u;  // 'OCCP'
  static constexpr uint16_t kVersion  = 1;
  // EMA weight for a new daily sample (~1/8 -> settles over roughly a week).
  static constexpr float    kAlpha    = 0.125f;

  struct PersistBlob {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t samples = 0;                         // total daily samples fed
    uint8_t  frac[kDayTypes][kHours] = {};        // occupied fraction, 0..255
  };

  struct Transition { uint8_t hour; bool occupied; };
  struct Suggestion {
    Transition t[kHours];
    uint8_t    count = 0;
    bool       confident = false;                 // enough samples to trust
  };

  // Feed ONE daily sample: was the home occupied during `hour` on this day?
  // Call once per hour boundary (the firmware aggregates within the hour).
  void observe(bool weekend, uint8_t hour, bool occupied) {
    if (hour >= kHours) return;
    float& f = frac_[weekend ? 1 : 0][hour];
    f += ((occupied ? 1.0f : 0.0f) - f) * kAlpha;
    if (samples_ < 0xFFFF) ++samples_;
  }

  float occupiedFraction(bool weekend, uint8_t hour) const {
    return hour < kHours ? frac_[weekend ? 1 : 0][hour] : 0.0f;
  }
  uint16_t samples() const { return samples_; }

  // Suggested schedule for a day-type: the hours where the occupied/away
  // classification changes. `threshold` is the occupied cutoff; `hyst` widens it
  // so an hour must clearly cross to flip (debounces noisy buckets). The first
  // transition establishes the starting state at hour 0.
  Suggestion suggestSchedule(bool weekend, float threshold = 0.5f,
                             float hyst = 0.15f) const {
    Suggestion s;
    // Need a meaningful history before proposing anything (>= ~5 days/bucket).
    s.confident = samples_ >= static_cast<uint16_t>(5u * kHours);
    const float* row = frac_[weekend ? 1 : 0];
    bool state = row[0] >= threshold;
    s.t[s.count++] = {0, state};
    for (uint8_t h = 1; h < kHours; ++h) {
      const bool up = row[h] >= threshold + hyst;
      const bool dn = row[h] <= threshold - hyst;
      const bool next = up ? true : dn ? false : state;  // hysteresis band holds
      if (next != state) { state = next; s.t[s.count++] = {h, state}; }
    }
    return s;
  }

  void toBlob(PersistBlob& b) const {
    b = PersistBlob{};
    b.magic = kMagic; b.version = kVersion; b.samples = samples_;
    for (uint8_t d = 0; d < kDayTypes; ++d)
      for (uint8_t h = 0; h < kHours; ++h) {
        float v = frac_[d][h]; v = v < 0 ? 0 : v > 1 ? 1 : v;
        b.frac[d][h] = static_cast<uint8_t>(v * 255.0f + 0.5f);
      }
  }

  // Fails open to "no history" on magic/version mismatch — never invents a
  // pattern from a corrupt/absent blob.
  bool fromBlob(const PersistBlob& b) {
    reset();
    if (b.magic != kMagic || b.version != kVersion) return false;
    samples_ = b.samples;
    for (uint8_t d = 0; d < kDayTypes; ++d)
      for (uint8_t h = 0; h < kHours; ++h)
        frac_[d][h] = b.frac[d][h] / 255.0f;
    return true;
  }

  void reset() { std::memset(frac_, 0, sizeof(frac_)); samples_ = 0; }

 private:
  float    frac_[kDayTypes][kHours] = {};
  uint16_t samples_ = 0;
};

}  // namespace dettson
