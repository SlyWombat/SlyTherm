// NetGuard.cpp — see NetGuard.h.

#include "NetGuard.h"

namespace net_guard {

State Guard::update(bool brokerUp, bool controllerUp, uint32_t attempts, uint32_t nowMs) {
  const bool healthy = brokerUp && controllerUp;

  if (healthy) {
    outage_ = false;
    outageAttempts_ = 0;
    state_ = State::kHealthy;
    return state_;
  }

  const bool isNetwork = !brokerUp;  // network outranks controller-offline
  if (!outage_ || outageIsNetwork_ != isNetwork) {
    // New outage, or it changed character (e.g. WiFi came back but the
    // Controller is still dark): restart the sustain clock — each panel
    // needs its own sustained window so a compound blip doesn't nag.
    outage_ = true;
    outageIsNetwork_ = isNetwork;
    outageStartMs_ = nowMs;
    attemptsAtStart_ = attempts;
  }
  outageAttempts_ = attempts - attemptsAtStart_;

  if (nowMs - outageStartMs_ >= sustainMs_) {
    state_ = isNetwork ? State::kNetworkUnavailable : State::kControllerOffline;
  } else {
    state_ = State::kBlip;
  }
  return state_;
}

}  // namespace net_guard
