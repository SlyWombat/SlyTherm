#pragma once
// SensorParticipation — durable per-sensor participation (the user's "include
// this room in the fused control temperature" choice), decoupled from roster
// membership.
//
// The bug this fixes: the roster (`slytherm/config/sensors`) is a RETAINED MQTT
// topic that HA republishes on every reboot AND every reconnect. The old code
// conflated "is in the roster" with "is participating" in a single `inRoster`
// flag, so a roster replay force-enabled every sensor and clobbered a user's
// OFF. Participation is now a separate, NVS-persisted choice keyed by the
// stable wire id (basement/living/…); a roster replay APPLIES the persisted
// choice instead of forcing true.
//
// Model:
//   - Default = participating (ON) for any id never explicitly toggled — this
//     preserves the opt-in-by-default behavior; only an explicit OFF (or a
//     later explicit ON over a stored OFF) is recorded.
//   - Keyed by WIRE ID, never the #85 friendly display label. The panel toggle
//     receives the display name and must resolve it to the wire id BEFORE
//     persisting, or a replay (which keys by id) would miss and clobber (#155).
//
// Fusion gate: a sensor fuses iff it is in the roster AND participating — you
// cannot fuse a sensor you do not know, and the user can opt a known one out.
// `fusionParticipates()` and `applyRoster()` are the one authoritative copy of
// that rule, shared by the firmware and the unit tests so they cannot drift.
//
// Persistence: a compact POD blob (`PersistBlob`) written raw to NVS via
// Preferences putBytes/getBytes, mirroring CompressorGuard/UiModel house style.
//
// Pure C++17, no Arduino.h — unit-tested in test/test_sensor_participation.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dettson {

class SensorParticipation {
 public:
  static constexpr size_t kMaxEntries = 8;   // == SensorFusion::kMaxSensors
  static constexpr size_t kIdLen = 24;       // == main_thermostat kSensorNameLen
  static constexpr uint32_t kMagic = 0x53505254u;  // 'SPRT'
  static constexpr uint16_t kVersion = 1;

  // POD blob for NVS (raw putBytes/getBytes). Only explicit choices are stored;
  // an id absent from the blob defaults to participating. All-char/uint8 body
  // so the layout is deterministic and memcpy-safe.
  struct PersistBlob {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t count = 0;
    struct Entry {
      char id[kIdLen] = {};
      uint8_t on = 1;
      uint8_t _pad[3] = {};
    } entries[kMaxEntries] = {};
  };

  // Effective participation for a sensor id. Default ON for any id never
  // explicitly toggled; a persisted OFF wins over a roster replay. Read-only —
  // never mutates the store (a replay is all reads).
  bool participating(const char* id) const {
    const int i = find(id);
    return i < 0 ? true : entries_[i].on;
  }

  // Record an explicit choice, keyed by wire id. Returns true iff the effective
  // value changed (persist-on-change). Setting ON for an unknown id is a no-op
  // (default is already ON); setting OFF adds/updates an entry.
  bool set(const char* id, bool on) {
    if (!id || !id[0]) return false;
    const int i = find(id);
    if (i >= 0) {
      if (static_cast<bool>(entries_[i].on) == on) return false;
      entries_[i].on = on ? 1 : 0;
      return true;
    }
    if (on) return false;  // matches default; nothing to persist
    if (count_ >= kMaxEntries) return false;  // full (>= fusion max: never in practice)
    std::strncpy(entries_[count_].id, id, kIdLen - 1);
    entries_[count_].id[kIdLen - 1] = '\0';
    entries_[count_].on = 0;
    ++count_;
    return true;
  }

  // NVS serialization (raw POD blob).
  void toBlob(PersistBlob& b) const {
    b = PersistBlob{};
    b.magic = kMagic;
    b.version = kVersion;
    b.count = static_cast<uint16_t>(count_);
    for (size_t i = 0; i < count_; ++i) {
      std::strncpy(b.entries[i].id, entries_[i].id, kIdLen - 1);
      b.entries[i].id[kIdLen - 1] = '\0';
      b.entries[i].on = entries_[i].on ? 1 : 0;
    }
  }

  // Restore from an NVS blob. Returns false (and leaves the store empty, i.e.
  // all-default) on magic/version mismatch — a corrupt or absent blob fails
  // open to the safe default of "everything participating".
  bool fromBlob(const PersistBlob& b) {
    count_ = 0;
    if (b.magic != kMagic || b.version != kVersion) return false;
    const size_t n = b.count > kMaxEntries ? kMaxEntries : b.count;
    for (size_t i = 0; i < n; ++i) {
      if (!b.entries[i].id[0]) continue;
      std::strncpy(entries_[count_].id, b.entries[i].id, kIdLen - 1);
      entries_[count_].id[kIdLen - 1] = '\0';
      entries_[count_].on = b.entries[i].on ? 1 : 0;
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

  struct E { char id[kIdLen] = {}; uint8_t on = 1; };
  E entries_[kMaxEntries] = {};
  size_t count_ = 0;
};

// The one authoritative fusion-participation rule: fuse a sensor iff it is a
// known roster member AND the user has it participating. Shared by the firmware
// (SensorFusion::setParticipating call) and the tests so they cannot drift.
inline bool fusionParticipates(bool inRoster, bool participating) {
  return inRoster && participating;
}

// Apply the persisted participation onto one rebuilt roster slot. Mirrors what
// handleSensorRoster does per roster entry: mark it a roster member and set its
// participation to the STORED choice (default ON) — NEVER force true. `SlotT`
// must expose writable `.inRoster` and `.participating`. Kept templated (like
// SensorRoster::findSlot) so main.cpp and the regression test run identical
// code: reverting this to `participating = true` is what the test catches.
template <typename SlotT>
inline void applyRosterMember(SlotT& slot, const char* wireId,
                              const SensorParticipation& store) {
  slot.inRoster = true;
  slot.participating = store.participating(wireId);
}

}  // namespace dettson
