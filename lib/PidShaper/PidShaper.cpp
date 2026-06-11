#include "PidShaper.h"
#include <cmath>

namespace dettson {

void PidShaper::setGains(uint8_t mode, const Gains& g) {
  cfg_.gains[clampMode(mode)] = g;
}

void PidShaper::selectMode(uint8_t mode) {
  mode = clampMode(mode);
  if (mode == mode_) return;
  mode_ = mode;
  integ_ = 0.0f;   // docs/05: integrator reset on mode change
  hasLast_ = false;
}

void PidShaper::reset() {
  integ_ = 0.0f;
  out_ = 0.0f;
  hasLast_ = false;
}

float PidShaper::update(float setpointC, float measuredC, bool inputValid,
                        bool freeze, uint32_t nowS) {
  if (!inputValid || !std::isfinite(setpointC) || !std::isfinite(measuredC)) {
    reset();  // clean restart on recovery: no stale integral/derivative kick
    return 0.0f;
  }

  if (freeze) {
    // Hold output, stop integrating; keep time/measurement current so the
    // unfreeze step sees a small dt instead of the whole defrost window.
    lastS_ = nowS;
    lastMeas_ = measuredC;
    hasLast_ = true;
    return out_;
  }

  const Gains& g = cfg_.gains[mode_];
  const float err = setpointC - measuredC;  // heating sense: too cold -> positive
  const float dtS = (hasLast_ && nowS >= lastS_)
                        ? static_cast<float>(nowS - lastS_)
                        : 0.0f;

  const float p = g.kp * err;
  float d = 0.0f;
  if (dtS > 0.0f) d = -g.kd * (measuredC - lastMeas_) / dtS;

  const float candidateI = integ_ + g.ki * err * dtS;
  const float unsat = p + candidateI + d;
  if (unsat > cfg_.outMaxPct) {
    if (candidateI < integ_) integ_ = candidateI;  // only unwinding allowed
  } else if (unsat < cfg_.outMinPct) {
    if (candidateI > integ_) integ_ = candidateI;
  } else {
    integ_ = candidateI;
  }

  float out = p + integ_ + d;
  if (out > cfg_.outMaxPct) out = cfg_.outMaxPct;
  if (out < cfg_.outMinPct) out = cfg_.outMinPct;
  out_ = out;

  lastS_ = nowS;
  lastMeas_ = measuredC;
  hasLast_ = true;
  return out_;
}

}  // namespace dettson
