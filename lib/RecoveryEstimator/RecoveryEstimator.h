// RecoveryEstimator.h — smart recovery (pre-heat/pre-cool) advisor (issue #50).
//
// Learns equipment ramp rates (degrees C per hour) per {mode heat|cool,
// equipment hp|gas} channel from run segments the caller observes
// (startSegment/endSegment with temps + times), then — given the next
// scheduled target HA publishes on dettson/cmd/next_target (docs/06 "Smart
// recovery") — recommends how early the call should start so the room
// arrives at the setpoint on time.
//
// ADVISORY ONLY (docs/04 §3 single-emission-point rule): the output is a
// recommended early-start lead and nothing more. ModeStateMachine / main
// glue decides whether to act on it, and CompressorGuard + DualFuelArbiter
// still gate every demand — this module has NO demand authority. A bogus
// target can at worst start a normal, fully-protected call early; it can
// never bypass a lockout or timer.
//
// Learning is a robust EMA over per-segment rates: segments shorter than
// kRecoveryMinSegmentS or moving less than kRecoveryMinSegmentDeltaC are
// ignored, rates outside the absolute plausibility band are dropped, and
// once a channel has kRecoveryOutlierMinSamples accepted segments, a rate
// more than kRecoveryOutlierRatio off the estimate is rejected as an
// outlier (docs/05 canonical defaults table).
//
// Disabled by default (kRecoveryEnabledDefault) until field-tuned (docs/06).
//
// Pure C++17, no Arduino. Time is injected as uint32_t nowS (monotonic).

#pragma once
#include <cstdint>
#include "DettsonConfig.h"

namespace dettson {

enum class RecoveryMode : uint8_t { kHeat = 0, kCool = 1 };
enum class RecoveryEquipment : uint8_t { kHp = 0, kGas = 1 };

struct RecoveryConfig {
  bool     enabled          = kRecoveryEnabledDefault;
  float    seedHeatCPerH    = kRecoverySeedHeatCPerH;
  float    seedCoolCPerH    = kRecoverySeedCoolCPerH;
  uint32_t maxLookaheadS    = kRecoveryMaxLookaheadS;
  uint32_t minSegmentS      = kRecoveryMinSegmentS;
  float    minSegmentDeltaC = kRecoveryMinSegmentDeltaC;
  float    emaAlpha         = kRecoveryEmaAlpha;
  float    rateMinCPerH     = kRecoveryRateMinCPerH;
  float    rateMaxCPerH     = kRecoveryRateMaxCPerH;
  float    outlierRatio     = kRecoveryOutlierRatio;
  uint8_t  outlierMinSamples = kRecoveryOutlierMinSamples;
};

// The next scheduled setpoint change (glue maps hamqtt::NextTarget here).
struct RecoveryTarget {
  float        setpointC = 0.0f;
  RecoveryMode mode      = RecoveryMode::kHeat;
  uint32_t     inS       = 0;  // seconds until the target takes effect
};

struct RecoveryAdvice {
  // Recommended lead time before the target takes effect; 0 = no early
  // start (disabled, already at/past target, or invalid input). Always
  // <= maxLookaheadS.
  uint32_t startEarlyByS = 0;
  // Convenience for the glue: the lead already covers the remaining time
  // (startEarlyByS >= target.inS), so "start now" is the recommendation.
  bool startNow = false;
};

class RecoveryEstimator {
 public:
  RecoveryEstimator();
  explicit RecoveryEstimator(const RecoveryConfig& cfg);

  bool enabled() const { return cfg_.enabled; }
  void setEnabled(bool on) { cfg_.enabled = on; }

  // The caller (glue) reports observed run segments: startSegment when the
  // equipment begins serving a call, endSegment when it stops. A second
  // startSegment discards the open one (an interrupted segment is not a
  // clean ramp observation). Learning continues while disabled — only the
  // advice is gated — so enabling in the field starts from real data.
  void startSegment(RecoveryMode mode, RecoveryEquipment equip,
                    float tempC, uint32_t nowS);
  // Returns true iff the segment passed all gates and updated the estimate.
  bool endSegment(float tempC, uint32_t nowS);

  // Current ramp-rate estimate for a channel (the seed until learned).
  float rateCPerH(RecoveryMode mode, RecoveryEquipment equip) const;
  // Accepted-segment count for a channel.
  uint32_t samples(RecoveryMode mode, RecoveryEquipment equip) const;

  // Recommend an early-start lead for the given target, using the channel
  // for {target.mode, equip} (equip = what the caller expects to serve the
  // call). Pure function of state — no demand, no side effects.
  RecoveryAdvice advise(const RecoveryTarget& target, float currentTempC,
                        RecoveryEquipment equip) const;

  const RecoveryConfig& config() const { return cfg_; }

 private:
  struct Channel {
    float    rateCPerH = 0.0f;  // seeded in ctor
    uint32_t accepted  = 0;
  };

  Channel&       channel(RecoveryMode m, RecoveryEquipment e);
  const Channel& channel(RecoveryMode m, RecoveryEquipment e) const;
  void seed();

  RecoveryConfig cfg_;
  Channel        channels_[2][2];  // [mode][equipment]

  bool              segOpen_   = false;
  RecoveryMode      segMode_   = RecoveryMode::kHeat;
  RecoveryEquipment segEquip_  = RecoveryEquipment::kHp;
  float             segTempC_  = 0.0f;
  uint32_t          segStartS_ = 0;
};

}  // namespace dettson
