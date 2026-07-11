// TrendEstimator.h — EMA'd slope of the fused temperature (issue #141,
// docs/13 §2 "steady-state crossing prediction").
//
// Small pure helper downstream of SensorFusion: feed it the fused output
// every control tick (update() with the fusion's valid flag) and read back a
// smoothed slope in degrees C per hour. Robust to fusion dropouts:
//   - invalid samples are skipped without disturbing the state (the next
//     valid sample bridges the gap with a slope over the whole gap);
//   - a valid-to-valid gap longer than maxGapS discards the trend entirely
//     (the room may have moved while we were blind) and re-anchors;
//   - each per-sample slope is clamped to +/- maxSlopeCPerH before entering
//     the EMA (spike rejection, same plausibility philosophy as
//     RecoveryEstimator's segment gates).
// The slope reads 0/invalid until warmupS of bridged history has been
// observed since the last reset.
//
// ADVISORY input only (docs/04 §3 single-emission-point rule): consumers
// (RecoveryEstimator::crossingBias -> StagedCoolShaper::requestFromError)
// bias demand REQUESTS with it; every request still passes the shapers'
// cycle hygiene and CompressorGuard downstream. A bogus slope can at worst
// start a normal, fully-protected cycle early — never bypass a timer.
//
// Pure C++17, no Arduino. Time is injected as uint32_t nowS (monotonic).

#pragma once
#include <cstdint>
#include "DettsonConfig.h"

namespace dettson {

class TrendEstimator {
 public:
  struct Config {
    uint32_t tauS           = kTrendTauS;           // slope EMA time constant
    uint32_t maxGapS        = kTrendMaxGapS;        // longer valid-gap -> reset
    uint32_t warmupS        = kTrendWarmupS;        // history span before valid()
    float    maxSlopeCPerH  = kTrendMaxSlopeCPerH;  // per-sample clamp
  };

  TrendEstimator() {}
  explicit TrendEstimator(const Config& cfg) : cfg_(cfg) {}

  // Feed the fused temperature at the control-loop rate. valid=false (fusion
  // dropout) never disturbs the state; NaN/non-finite is treated as invalid.
  void update(float tempC, bool valid, uint32_t nowS);

  void reset();

  // True once warmupS of bridged history has accumulated since reset.
  bool valid() const { return spanS_ >= cfg_.warmupS; }
  // Smoothed slope, degrees C per hour; 0.0 while !valid().
  float slopeCPerH() const { return valid() ? slopeCPerH_ : 0.0f; }

  const Config& config() const { return cfg_; }

 private:
  Config   cfg_;
  bool     havePrev_   = false;
  float    prevC_      = 0.0f;
  uint32_t prevS_      = 0;
  uint32_t spanS_      = 0;    // bridged history accumulated (capped at warmupS)
  float    slopeCPerH_ = 0.0f;
};

}  // namespace dettson
