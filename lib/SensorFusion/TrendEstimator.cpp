// TrendEstimator.cpp — see TrendEstimator.h. Pure C++17, no Arduino.
#include "TrendEstimator.h"

#include <cmath>

namespace dettson {

void TrendEstimator::reset() {
  havePrev_ = false;
  spanS_ = 0;
  slopeCPerH_ = 0.0f;
}

void TrendEstimator::update(float tempC, bool valid, uint32_t nowS) {
  if (!valid || !std::isfinite(tempC)) return;  // dropout: state untouched

  if (havePrev_) {
    // Non-monotonic clock: treat as no time passed (same convention as the
    // shapers' elapsedS) — re-anchor without learning from a bogus interval.
    const uint32_t dtS = nowS >= prevS_ ? nowS - prevS_ : 0;
    if (dtS == 0) return;
    if (dtS > cfg_.maxGapS) {
      reset();  // blind too long: the old trend no longer describes the room
    } else {
      float s = (tempC - prevC_) * 3600.0f / static_cast<float>(dtS);
      if (s > cfg_.maxSlopeCPerH) s = cfg_.maxSlopeCPerH;      // spike clamp
      else if (s < -cfg_.maxSlopeCPerH) s = -cfg_.maxSlopeCPerH;
      // Irregular-interval EMA: weight ~ dt/(dt+tau) so the effective window
      // stays ~tauS regardless of sample cadence (60 s ticks or bridged gaps).
      const float w = static_cast<float>(dtS) /
                      static_cast<float>(dtS + cfg_.tauS);
      slopeCPerH_ += w * (s - slopeCPerH_);
      spanS_ = (spanS_ + dtS >= cfg_.warmupS) ? cfg_.warmupS : spanS_ + dtS;
    }
  }
  havePrev_ = true;
  prevC_ = tempC;
  prevS_ = nowS;
}

}  // namespace dettson
