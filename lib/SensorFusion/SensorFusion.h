// SensorFusion.h — multi-sensor indoor temperature fusion (docs/04 §4, docs/06).
//
// Occupancy-weighted "follow me" fusion of remote (HA/MQTT) sensors with the
// local DS18B20 as a flagged fallback. Per-sensor health gates (staleness,
// range, stuck-value, outlier-vs-median) are all mandatory per docs/04 §4.
//
// Fallback ladder: fused remotes -> single remaining remote -> local sensor
// (DEGRADED — caller must bound setpoints and disable cooling per the
// kDegraded* constants in DettsonConfig.h) -> nothing valid (caller goes to
// no-demand). Boot state is invalid/no-demand until real updates arrive.
//
// Pure C++17, no Arduino. Time is injected as uint32_t nowS.

#pragma once
#include <cstddef>
#include <cstdint>

#include "DettsonConfig.h"

#ifndef SLYTHERM_FUSION_MAX_SENSORS
#define SLYTHERM_FUSION_MAX_SENSORS 8
#endif

namespace dettson {

enum class Occupancy : uint8_t { kUnknown = 0, kVacant = 1, kOccupied = 2 };

enum class SourceTier : uint8_t {
  kNone = 0,         // nothing valid -> caller must drop to no-demand
  kFusedRemotes,     // >=2 healthy remote participants
  kSingleRemote,     // exactly 1 healthy remote
  kLocalDegraded,    // local fallback only — DEGRADED mode (docs/04 §4)
};

// Alarm bits, also used as per-sensor fault flags in SensorStatus.
enum FusionAlarm : uint16_t {
  kAlarmStale         = 1u << 0,
  kAlarmRange         = 1u << 1,  // out of [kSensorRangeMinC, kSensorRangeMaxC] or NaN
  kAlarmStuck         = 1u << 2,  // zero variance over the stuck window
  kAlarmOutlier       = 1u << 3,  // > outlier threshold from live-participant median
  kAlarmLocalDisagree = 1u << 4,  // local vs fused aggregate > kDs18b20DisagreeAlarmC
  kAlarmDegraded      = 1u << 5,  // running on the local fallback alone
  kAlarmAllBad        = 1u << 6,  // no valid source at all
};

struct FusedTemp {
  float value = 0.0f;     // meaningful only when valid
  bool valid = false;
  bool degraded = false;  // true iff tier == kLocalDegraded
  SourceTier tier = SourceTier::kNone;
  uint16_t alarms = 0;    // FusionAlarm bits observed this evaluation
};

struct SensorStatus {
  bool registered = false;
  bool live = false;       // passed all health gates at the last fusedTemp()
  bool participating = false;
  bool hasTemp = false;
  float tempC = 0.0f;      // corrected (offset applied) last value; valid iff hasTemp
  uint32_t ageS = 0;       // UINT32_MAX if never updated
  uint16_t faults = 0;     // FusionAlarm bits for this sensor (last evaluation)
  float offsetC = 0.0f;    // active calibration offset (docs/07 G6)
  bool occupied = false;   // occupied within the occupancy window ("present now")
  bool everOccupied = false;
  uint32_t lastOccAgeS = 0xFFFFFFFFu;  // seconds since last occupied (max = never)
  float weight = 1.0f;     // current occupancy weight = influence on the fusion
};

// Bounds documented in docs/05 defaults table but not (yet) named in
// DettsonConfig.h — module-local until promoted to the shared contract.
constexpr uint32_t kSensorMaxAgeMinS    = 180;
constexpr uint32_t kSensorMaxAgeMaxS    = 900;
constexpr uint32_t kStuckWindowDefaultS = 3600;  // zero-variance window (not in docs table)
constexpr uint32_t kStuckWindowMinS     = 300;

class SensorFusion {
 public:
  static constexpr size_t kMaxSensors = SLYTHERM_FUSION_MAX_SENSORS;
  static_assert(SLYTHERM_FUSION_MAX_SENSORS >= 2 && SLYTHERM_FUSION_MAX_SENSORS <= 16,
                "participant mask is 16-bit");

  // Registration / roster (docs/06 retained sensor roster). Returns false on
  // duplicate id or full table. maxAgeS is clamped to [180, 900].
  bool registerSensor(uint8_t id, bool isLocal = false, bool participating = true,
                      uint32_t maxAgeS = kSensorMaxAgeS);
  bool setParticipating(uint8_t id, bool participating);
  bool setSensorMaxAgeS(uint8_t id, uint32_t maxAgeS);

  // Calibration offset (docs/07 G6), clamped to ±kSensorOffsetMaxC; applies
  // to the local fallback sensor too. Corrected values feed every health gate
  // and the fusion mean; a change routes through the slew limit (no output
  // step) and never touches the stuck-value window. Non-finite is rejected.
  bool setSensorOffsetC(uint8_t id, float offsetC);

  // Feed a sample. occ = kUnknown for sensors without occupancy (e.g. local
  // DS18B20). Returns false for an unregistered id (caller should alarm).
  // Out-of-range/NaN samples are stored and rejected at evaluation time.
  bool update(uint8_t id, float tempC, Occupancy occ, uint32_t nowS);

  // Evaluate health, fuse, smooth, slew-limit. Call periodically (~1-60 s).
  FusedTemp fusedTemp(uint32_t nowS);

  SensorStatus status(uint8_t id, uint32_t nowS) const;

  // Id of the sensor with the greatest current influence on the fused value
  // (top occupancy weight among live, non-local participants) — the "driving"
  // sensor for the ambient screen. 0xFF if none (local-only / all bad).
  uint8_t dominantParticipant() const;

  // Runtime tunables, clamped to the documented ranges.
  void setWeightRampTauS(uint32_t tauS);    // [kWeightRampTauMinS, kWeightRampTauMaxS]
  void setSmoothingTauS(uint32_t tauS);     // [kFusionSmoothingTauMinS, kFusionSmoothingTauMaxS]
  void setOccupancyWindowS(uint32_t windowS);
  void setStuckWindowS(uint32_t windowS);   // >= kStuckWindowMinS
  void setSlewCPerMin(float cPerMin);
  void setOccupiedWeight(float w);          // >= 1.0
  void setOutlierC(float c);
  void setLocalDisagreeAlarmC(float c);

 private:
  struct Slot {
    bool used = false;
    uint8_t id = 0;
    bool isLocal = false;
    bool participating = true;
    uint32_t maxAgeS = kSensorMaxAgeS;
    float offsetC = 0.0f;

    bool hasUpdate = false;
    float tempC = 0.0f;
    uint32_t lastUpdateS = 0;

    bool everOccupied = false;
    uint32_t lastOccS = 0;
    float weight = 1.0f;  // ramped occupancy weight

    float lastDistinctTempC = 0.0f;  // stuck-value tracking
    uint32_t lastChangeS = 0;

    bool live = false;    // last-evaluation result
    uint16_t faults = 0;

    float correctedC() const { return tempC + offsetC; }
  };

  Slot* find(uint8_t id);
  const Slot* find(uint8_t id) const;

  Slot slots_[kMaxSensors];

  // Tunables (defaults from DettsonConfig.h).
  float outlierC_           = kSensorOutlierC;
  float occupiedWeight_     = kOccupiedWeight;
  uint32_t occupancyWindowS_= kOccupancyWindowS;
  uint32_t weightRampTauS_  = kWeightRampTauMinS;
  uint32_t smoothingTauS_   = kFusionSmoothingTauMinS;
  float slewCPerMin_        = kFusionSlewCPerMin;
  float localDisagreeC_     = kDs18b20DisagreeAlarmC;
  uint32_t stuckWindowS_    = kStuckWindowDefaultS;

  // Fusion state.
  bool hasTime_ = false;
  uint32_t lastNowS_ = 0;
  bool hasOutput_ = false;   // false at boot and after any all-bad interval
  float ema_ = 0.0f;
  float output_ = 0.0f;
  bool slewActive_ = false;  // engaged on participant-set change, released on catch-up
  uint16_t lastMask_ = 0;
};

}  // namespace dettson
