// EquipmentHealth.cpp — see EquipmentHealth.h. Issue #189.
#include "EquipmentHealth.h"

#include <cmath>

namespace dettson {

EquipmentHealthOutput EquipmentHealth::step(const EquipmentHealthInputs& in,
                                            uint32_t nowS) {
  EquipmentHealthOutput out;
  out.family = in.family;

  // Idle, or an untrustworthy room reading: no episode, no verdict. A sensor
  // problem must never masquerade as an equipment fault.
  if (in.family == EquipFamily::kIdle || !in.roomValid || in.roomDegraded) {
    reset();
    family_ = in.family;
    return out;
  }

  // Family change or setpoint move: new baseline, new episode.
  if (!running_ || in.family != family_ ||
      fabsf(in.setpointC - startSetC_) > cfg_.setpointResetC) {
    running_    = true;
    family_     = in.family;
    startS_     = nowS;
    worstRoomC_ = in.roomC;
    startSetC_  = in.setpointC;
    progressed_ = false;
    verdict_    = false;
  }

  const bool cooling = family_ == EquipFamily::kCool;
  const uint32_t runS = nowS - startS_;

  // Progress latch, measured from the episode's WORST point (the peak the
  // room reached while cooling / the trough while heating): that is what
  // "the equipment started working" looks like even after the room first
  // drifted the wrong way — exactly the 2026-08-09 recovery shape.
  const float moveC = cooling ? worstRoomC_ - in.roomC : in.roomC - worstRoomC_;
  if (moveC >= cfg_.minProgressC) progressed_ = true;
  if (cooling ? in.roomC > worstRoomC_ : in.roomC < worstRoomC_)
    worstRoomC_ = in.roomC;

  // Supply rule (only with a live probe): delta proves delivery either way.
  bool supplyHealthy = false, supplyFailed = false;
  if (in.supplyValid) {
    const float deltaC = cooling ? in.roomC - in.supplyC : in.supplyC - in.roomC;
    const float needC  = cooling ? cfg_.coolSupplyDeltaC : cfg_.heatSupplyDeltaC;
    if (deltaC >= needC) supplyHealthy = true;                       // delivering
    else if (runS >= cfg_.supplyMinRunS) supplyFailed = true;        // provably not
  }

  if (supplyHealthy) {
    // Cold (hot) air is demonstrably flowing — whatever the room does, the
    // EQUIPMENT is fine. Clears a latched verdict too.
    verdict_ = false;
  } else if (supplyFailed) {
    verdict_ = true;
  } else if (runS >= cfg_.minRunS && !progressed_) {
    verdict_ = true;   // trend rule: ran the whole window, moved nothing
  } else if (progressed_) {
    verdict_ = false;  // progress clears a previously latched verdict
  }

  out.ineffective = verdict_;
  return out;
}

}  // namespace dettson
