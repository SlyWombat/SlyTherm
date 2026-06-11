#include "DemandArbiter.h"
#include <cmath>

namespace dettson {

namespace {
bool invalidPct(float v) { return !std::isfinite(v) || v < 0.0f; }
float clampTop(float v) { return v > 100.0f ? 100.0f : v; }
uint32_t elapsedS(uint32_t nowS, uint32_t thenS) {
  return nowS >= thenS ? nowS - thenS : 0;  // non-monotonic clock: assume no time passed
}
}  // namespace

DemandArbiter::DemandArbiter(uint32_t bootNowS, const Config& cfg)
    : cfg_(cfg), lastHeatActiveS_(bootNowS), lastCoolActiveS_(bootNowS) {}

DemandSet DemandArbiter::set(const DemandRequest& req, uint32_t nowS) {
  inputAlarm_ = false;
  heatHeld_ = false;
  coolHeld_ = false;
  out_ = DemandSet{};  // every exit path below starts from no-demand

  if (invalidPct(req.gasHeatPct) || invalidPct(req.hpHeatPct) ||
      invalidPct(req.coolPct) || invalidPct(req.fanPct) ||
      invalidPct(req.defrostTemperPct)) {
    inputAlarm_ = true;  // docs/04 §2: invalid input -> demand 0 + alarm
    return out_;
  }

  if (invariantAlarm_) return out_;  // latched: stay at zero until manual clear

  const float gas     = clampTop(req.gasHeatPct);
  const float hp      = clampTop(req.hpHeatPct);
  const float cool    = clampTop(req.coolPct);
  const float fan     = clampTop(req.fanPct);
  const float defrost = clampTop(req.defrostTemperPct);

  // Invariants are checked on the REQUEST, before dwell suppression: a
  // conflicting request is an upstream logic bug regardless of what would
  // actually have been emitted.
  const bool heatFamily = gas > 0.0f || hp > 0.0f || defrost > 0.0f;
  const bool conflictHeatCool = heatFamily && cool > 0.0f;
  const bool conflictGasHp    = gas > 0.0f && hp > 0.0f;       // defrostTemper+hp is the sole sanctioned overlap
  const bool conflictGasTemper = gas > 0.0f && defrost > 0.0f; // two gas channels at once
  if (conflictHeatCool || conflictGasHp || conflictGasTemper) {
    invariantAlarm_ = true;
    return out_;
  }

  // Changeover sequencing (docs/04 §2): each direction waits its min-dwell
  // measured from the last nonzero EMISSION of the opposite family.
  float outGas = gas, outHp = hp, outCool = cool, outDefrost = defrost;
  if (heatFamily && elapsedS(nowS, lastCoolActiveS_) < cfg_.coolToHeatDwellS) {
    outGas = outHp = outDefrost = 0.0f;
    heatHeld_ = true;
  }
  if (cool > 0.0f && elapsedS(nowS, lastHeatActiveS_) < cfg_.heatToCoolDwellS) {
    outCool = 0.0f;
    coolHeld_ = true;
  }

  out_.gasHeatPct = outGas;
  out_.hpHeatPct = outHp;
  out_.coolPct = outCool;
  out_.fanPct = fan;
  out_.defrostTemperPct = outDefrost;

  if (outGas > 0.0f || outHp > 0.0f || outDefrost > 0.0f) lastHeatActiveS_ = nowS;
  if (outCool > 0.0f) lastCoolActiveS_ = nowS;

  return out_;
}

}  // namespace dettson
