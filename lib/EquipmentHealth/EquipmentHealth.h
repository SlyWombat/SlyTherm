// EquipmentHealth.h — equipment-effectiveness watchdog (issue #189).
//
// Field incident 2026-08-09: the outdoor unit sat in a protection lockout for
// hours while every observable layer reported healthy — demands ACKed, furnace
// displaying "cooling", blower running — and the house warmed all afternoon
// with zero alarms. The equipment's own compliance is the ONE thing the bus
// cannot prove; this module infers it from what the house actually does.
//
// Two independent rules, evaluated per active equipment family:
//
//   TREND (no extra hardware): after minRunS of CONTINUOUS running, the fused
//   room temperature must have moved at least minProgressC in the right
//   direction, measured from the episode's WORST point (peak while cooling /
//   trough while heating; latched — any progress satisfies the episode).
//   No progress by the deadline -> ineffective.
//
//   SUPPLY DELTA (optional supply-air probe, any HA-bridged sensor): after
//   supplyMinRunS, cooling supply must run >= coolSupplyDeltaC BELOW the room
//   (heating: >= heatSupplyDeltaC ABOVE). A healthy delta also POSITIVELY
//   suppresses the trend rule — cold air pouring into a room that will not
//   cool (open windows, heat wave) is not an equipment fault.
//
// Episode = continuous run of one family. Mode change or idle resets. A
// setpoint change > kSetpointResetC resets (the comparison baseline moved).
// Invalid/degraded room temp SUSPENDS judgment (episode restarts on
// recovery) — a sensor problem must never masquerade as an equipment fault.
//
// Pure C++17, no Arduino; time injected as uint32_t nowS (monotonic).

#pragma once
#include <cstdint>

#include "DettsonConfig.h"

namespace dettson {

enum class EquipFamily : uint8_t { kIdle = 0, kCool, kHeat };

struct EquipmentHealthConfig {
  uint32_t minRunS        = kEffectMinRunS;         // trend-rule judgment deadline
  float    minProgressC   = kEffectMinProgressC;    // required movement by then
  uint32_t supplyMinRunS  = kEffectSupplyMinRunS;   // supply-rule deadline
  float    coolSupplyDeltaC = kEffectCoolSupplyDeltaC;  // room - supply while cooling
  float    heatSupplyDeltaC = kEffectHeatSupplyDeltaC;  // supply - room while heating
  float    setpointResetC = kEffectSetpointResetC;  // setpoint move that resets the episode
};

struct EquipmentHealthInputs {
  EquipFamily family = EquipFamily::kIdle;  // ACTIVE equipment family this tick
  float roomC        = 0.0f;                // fused indoor temperature
  bool  roomValid    = false;
  bool  roomDegraded = false;               // degraded fusion -> suspend judgment
  float setpointC    = 0.0f;                // active setpoint for the family
  float supplyC      = 0.0f;                // supply-air probe (optional)
  bool  supplyValid  = false;               // caller enforces staleness
};

struct EquipmentHealthOutput {
  bool        ineffective = false;  // the active family is provably not delivering
  EquipFamily family      = EquipFamily::kIdle;  // which family the verdict is about
};

class EquipmentHealth {
 public:
  EquipmentHealth() = default;
  explicit EquipmentHealth(const EquipmentHealthConfig& cfg) : cfg_(cfg) {}

  EquipmentHealthOutput step(const EquipmentHealthInputs& in, uint32_t nowS);

  // Diagnostics (current episode; 0/false when idle or suspended).
  uint32_t episodeRunS(uint32_t nowS) const {
    return running_ ? nowS - startS_ : 0;
  }
  bool progressed() const { return progressed_; }

 private:
  void reset() { running_ = false; progressed_ = false; verdict_ = false; }

  EquipmentHealthConfig cfg_;
  bool        running_    = false;
  EquipFamily family_     = EquipFamily::kIdle;
  uint32_t    startS_     = 0;
  float       worstRoomC_ = 0.0f;  // episode peak (cooling) / trough (heating)
  float       startSetC_  = 0.0f;
  bool        progressed_ = false;  // latched: progress seen this episode
  bool        verdict_    = false;  // latched until progress or episode end
};

}  // namespace dettson
