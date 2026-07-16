#pragma once
// SensorCalibration — durable per-sensor temperature offset (the user's HA-set
// calibration), keyed by the stable wire id and NVS-persisted.
//
// The bug this fixes (#164): offsets set via the HA number entity lived only in
// RAM, and handleSensorRoster overwrote them from the RETAINED roster topic
// (whose offset is always the 0.0 default) on every reboot/reconnect — silently
// reverting the user's calibration. This is the same disease as #155, so the fix
// is the same shape as SensorParticipation: a separate NVS store keyed by wire
// id; a roster replay APPLIES the persisted offset instead of clobbering it.
//
// Model: default 0.0 for any id never set. `has()` distinguishes "user set an
// explicit offset" from "never set", so a roster MAY still supply a non-zero
// default for an id the user has not overridden (the glue uses has() to pick).
//
// Sibling of SensorParticipation — same wire-id pattern, separate blob + NVS key
// ("sofs"), so neither store's format change can disturb the other. Pure C++17.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dettson {

class SensorCalibration {
 public:
  static constexpr size_t kMaxEntries = 8;         // == SensorFusion::kMaxSensors
  static constexpr size_t kIdLen = 24;             // == main_thermostat kSensorNameLen
  static constexpr uint32_t kMagic = 0x53434C42u;  // 'SCLB'
  static constexpr uint16_t kVersion = 1;

  struct PersistBlob {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t count = 0;
    struct Entry {
      char id[kIdLen] = {};
      float offsetC = 0.0f;
    } entries[kMaxEntries] = {};
  };

  // Did the user set an explicit offset for this id? (vs. default/unset.)
  bool has(const char* id) const { return find(id) >= 0; }

  // Effective offset: the stored value, or 0.0 for any id never set.
  float offsetFor(const char* id) const {
    const int i = find(id);
    return i < 0 ? 0.0f : entries_[i].offsetC;
  }

  // Record an explicit offset, keyed by wire id. Returns true iff it changed
  // (persist-on-change). Storing 0.0 for an unknown id is a no-op (already the
  // default); any value for a known id updates it.
  bool set(const char* id, float offsetC) {
    if (!id || !id[0]) return false;
    const int i = find(id);
    if (i >= 0) {
      if (entries_[i].offsetC == offsetC) return false;
      entries_[i].offsetC = offsetC;
      return true;
    }
    if (offsetC == 0.0f) return false;         // matches default; nothing to persist
    if (count_ >= kMaxEntries) return false;   // full (>= fusion max: never in practice)
    std::strncpy(entries_[count_].id, id, kIdLen - 1);
    entries_[count_].id[kIdLen - 1] = '\0';
    entries_[count_].offsetC = offsetC;
    ++count_;
    return true;
  }

  void toBlob(PersistBlob& b) const {
    b = PersistBlob{};
    b.magic = kMagic;
    b.version = kVersion;
    b.count = static_cast<uint16_t>(count_);
    for (size_t i = 0; i < count_; ++i) {
      std::strncpy(b.entries[i].id, entries_[i].id, kIdLen - 1);
      b.entries[i].id[kIdLen - 1] = '\0';
      b.entries[i].offsetC = entries_[i].offsetC;
    }
  }

  // Restore from an NVS blob. Fails open to "no offsets" (all default 0) on
  // magic/version mismatch — a corrupt/absent blob never injects a bogus offset.
  bool fromBlob(const PersistBlob& b) {
    count_ = 0;
    if (b.magic != kMagic || b.version != kVersion) return false;
    const size_t n = b.count > kMaxEntries ? kMaxEntries : b.count;
    for (size_t i = 0; i < n; ++i) {
      if (!b.entries[i].id[0]) continue;
      std::strncpy(entries_[count_].id, b.entries[i].id, kIdLen - 1);
      entries_[count_].id[kIdLen - 1] = '\0';
      entries_[count_].offsetC = b.entries[i].offsetC;
      ++count_;
    }
    return true;
  }

 private:
  int find(const char* id) const {
    if (!id) return -1;
    for (size_t i = 0; i < count_; ++i)
      if (std::strncmp(entries_[i].id, id, kIdLen) == 0) return static_cast<int>(i);
    return -1;
  }

  struct E { char id[kIdLen] = {}; float offsetC = 0.0f; };
  E entries_[kMaxEntries] = {};
  size_t count_ = 0;
};

// Apply the persisted offset onto one rebuilt roster slot: the user's stored
// value wins; otherwise the roster's own default is kept. `SlotT` must expose a
// writable `.offsetC`. Mirrors applyRosterMember so main.cpp and the test share
// the one rule (reverting to `slot.offsetC = rosterDefault` is what the test
// catches).
template <typename SlotT>
inline void applyRosterOffset(SlotT& slot, const char* wireId, float rosterDefault,
                              const SensorCalibration& store) {
  slot.offsetC = store.has(wireId) ? store.offsetFor(wireId) : rosterDefault;
}

}  // namespace dettson
