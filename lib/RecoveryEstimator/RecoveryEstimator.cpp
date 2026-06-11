// RecoveryEstimator.cpp — see RecoveryEstimator.h. Pure C++17, no Arduino.
#include "RecoveryEstimator.h"

#include <cmath>

namespace dettson {

RecoveryEstimator::RecoveryEstimator() { seed(); }

RecoveryEstimator::RecoveryEstimator(const RecoveryConfig& cfg) : cfg_(cfg) {
  seed();
}

void RecoveryEstimator::seed() {
  for (int e = 0; e < 2; ++e) {
    channels_[static_cast<int>(RecoveryMode::kHeat)][e].rateCPerH =
        cfg_.seedHeatCPerH;
    channels_[static_cast<int>(RecoveryMode::kCool)][e].rateCPerH =
        cfg_.seedCoolCPerH;
  }
}

RecoveryEstimator::Channel& RecoveryEstimator::channel(RecoveryMode m,
                                                       RecoveryEquipment e) {
  return channels_[static_cast<int>(m)][static_cast<int>(e)];
}

const RecoveryEstimator::Channel& RecoveryEstimator::channel(
    RecoveryMode m, RecoveryEquipment e) const {
  return channels_[static_cast<int>(m)][static_cast<int>(e)];
}

void RecoveryEstimator::startSegment(RecoveryMode mode, RecoveryEquipment equip,
                                     float tempC, uint32_t nowS) {
  if (std::isnan(tempC)) {  // unusable anchor: leave no open segment behind
    segOpen_ = false;
    return;
  }
  segOpen_   = true;  // an already-open segment is discarded (interrupted)
  segMode_   = mode;
  segEquip_  = equip;
  segTempC_  = tempC;
  segStartS_ = nowS;
}

bool RecoveryEstimator::endSegment(float tempC, uint32_t nowS) {
  if (!segOpen_) return false;
  segOpen_ = false;  // segment consumed whatever the verdict
  if (std::isnan(tempC)) return false;

  const uint32_t durS = nowS - segStartS_;
  if (durS < cfg_.minSegmentS) return false;

  // Signed progress in the helpful direction: a heat segment that lost
  // ground is not a ramp observation (equipment outpaced by the load).
  const float delta = (segMode_ == RecoveryMode::kHeat)
                          ? tempC - segTempC_
                          : segTempC_ - tempC;
  if (delta < cfg_.minSegmentDeltaC) return false;

  const float rate = delta * 3600.0f / static_cast<float>(durS);
  if (rate < cfg_.rateMinCPerH || rate > cfg_.rateMaxCPerH) return false;

  Channel& ch = channel(segMode_, segEquip_);
  if (ch.accepted >= cfg_.outlierMinSamples &&
      (rate > ch.rateCPerH * cfg_.outlierRatio ||
       rate < ch.rateCPerH / cfg_.outlierRatio)) {
    return false;  // robust-EMA outlier rejection (docs/05 table)
  }
  ch.rateCPerH += cfg_.emaAlpha * (rate - ch.rateCPerH);
  ++ch.accepted;
  return true;
}

float RecoveryEstimator::rateCPerH(RecoveryMode mode,
                                   RecoveryEquipment equip) const {
  return channel(mode, equip).rateCPerH;
}

uint32_t RecoveryEstimator::samples(RecoveryMode mode,
                                    RecoveryEquipment equip) const {
  return channel(mode, equip).accepted;
}

RecoveryAdvice RecoveryEstimator::advise(const RecoveryTarget& target,
                                         float currentTempC,
                                         RecoveryEquipment equip) const {
  RecoveryAdvice advice;
  if (!cfg_.enabled) return advice;  // OFF by default (docs/06)
  if (std::isnan(currentTempC) || std::isnan(target.setpointC)) return advice;

  const float delta = (target.mode == RecoveryMode::kHeat)
                          ? target.setpointC - currentTempC
                          : currentTempC - target.setpointC;
  if (delta <= 0.0f) return advice;  // already at/past the target

  const float rate = channel(target.mode, equip).rateCPerH;  // > 0 (seeded)
  const float requiredS = std::ceil(delta * 3600.0f / rate);
  advice.startEarlyByS =
      (requiredS >= static_cast<float>(cfg_.maxLookaheadS))
          ? cfg_.maxLookaheadS
          : static_cast<uint32_t>(requiredS);
  advice.startNow = advice.startEarlyByS >= target.inS;
  return advice;
}

}  // namespace dettson
