#include "DemandShaper.h"
#include <cmath>

namespace dettson {

namespace {
bool invalidRequest(float pct) { return !std::isfinite(pct) || pct < 0.0f; }
float clampTop(float pct) { return pct > 100.0f ? 100.0f : pct; }
uint32_t elapsedS(uint32_t nowS, uint32_t thenS) {
  return nowS >= thenS ? nowS - thenS : 0;  // non-monotonic clock: assume no time passed
}
}  // namespace

// ---------------------------------------------------------------- GasShaper
void GasShaper::extinguish(uint32_t nowS) {
  lit_ = false;
  offAnchored_ = true;
  offSinceS_ = nowS;
  minOffWaived_ = false;
}

bool GasShaper::minOffServed(uint32_t nowS) const {
  return minOffWaived_ || elapsedS(nowS, offSinceS_) >= cfg_.minOffS;
}

void GasShaper::bootRestore(uint32_t nowS, bool minOffServed) {
  lit_ = false;
  runtimeTripped_ = false;
  offAnchored_ = true;
  offSinceS_ = nowS;
  minOffWaived_ = minOffServed;
}

void GasShaper::forceStop(uint32_t nowS) {
  extinguish(nowS);  // safety stop: always immediate, min-off restarts
}

float GasShaper::shape(float requestPct, uint32_t nowS) {
  if (!offAnchored_) {
    offAnchored_ = true;  // boot without bootRestore(): off-timer starts fresh
    offSinceS_ = nowS;
  }
  if (invalidRequest(requestPct)) {
    inputAlarm_ = true;
    if (lit_) extinguish(nowS);  // docs/04 §2: invalid input -> demand 0, safety stop
    return 0.0f;
  }
  requestPct = clampTop(requestPct);

  const float onThresh  = cfg_.floorPct + cfg_.lightMarginPct;
  const float offThresh = cfg_.floorPct - cfg_.extinguishMarginPct;

  if (runtimeTripped_) {
    // Held at 0 until the upstream call actually ends; a fresh call may
    // then start a new timed run. Alarm stays latched until clearAlarms().
    if (requestPct <= offThresh) runtimeTripped_ = false;
    return 0.0f;
  }

  if (lit_) {
    // Comfort stop: only after min-on; until then hold at minFire (G14).
    if (requestPct <= offThresh && elapsedS(nowS, runStartS_) >= cfg_.minOnS) {
      extinguish(nowS);
    }
  } else if (requestPct >= onThresh && minOffServed(nowS)) {
    lit_ = true;
    minOffWaived_ = false;
    runStartS_ = nowS;
  }

  if (lit_ && elapsedS(nowS, runStartS_) > cfg_.maxRuntimeS) {
    runtimeTripped_ = true;
    runtimeAlarm_ = true;
    extinguish(nowS);  // safety stop: bypasses min-on
    return 0.0f;
  }

  if (!lit_) return 0.0f;
  return requestPct < cfg_.floorPct ? cfg_.floorPct : requestPct;
}

void GasShaper::reset() {
  // Boot-conservative: the off-timer re-anchors fresh at the next shape().
  lit_ = false;
  runtimeTripped_ = false;
  offAnchored_ = false;
  minOffWaived_ = false;
}

// --------------------------------------------------------- HpInverterShaper
float HpInverterShaper::quantized() const {
  if (rawPct_ <= 0.0f) return 0.0f;
  float q = rawPct_;
  if (cfg_.stepPct > 0.0f) {
    q = std::round(rawPct_ / cfg_.stepPct) * cfg_.stepPct;
  }
  if (q < cfg_.floorPct) q = cfg_.floorPct;
  return clampTop(q);
}

float HpInverterShaper::shape(float requestPct, uint32_t nowS) {
  if (invalidRequest(requestPct)) {
    inputAlarm_ = true;
    rawPct_ = 0.0f;
    hasLast_ = false;
    return 0.0f;
  }
  requestPct = clampTop(requestPct);

  if (requestPct <= 0.0f) {
    rawPct_ = 0.0f;  // demand removal is immediate; restart timing is the guard's job
    lastS_ = nowS;
    hasLast_ = true;
    return 0.0f;
  }

  float target = requestPct < cfg_.floorPct ? cfg_.floorPct : requestPct;

  if (rawPct_ <= 0.0f) {
    rawPct_ = cfg_.floorPct;  // start at the floor, then slew upward
  } else {
    const float dtMin = hasLast_ ? static_cast<float>(elapsedS(nowS, lastS_)) / 60.0f : 0.0f;
    const float maxDelta = cfg_.slewPctPerMin * dtMin;
    const float delta = target - rawPct_;
    if (delta > maxDelta)        rawPct_ += maxDelta;
    else if (delta < -maxDelta)  rawPct_ -= maxDelta;
    else                         rawPct_ = target;
    if (rawPct_ < cfg_.floorPct) rawPct_ = cfg_.floorPct;
  }
  rawPct_ = clampTop(rawPct_);
  lastS_ = nowS;
  hasLast_ = true;
  return quantized();
}

void HpInverterShaper::reset() {
  rawPct_ = 0.0f;
  hasLast_ = false;
}

// ------------------------------------------------------------ HpRelayShaper
HpRelayShaper::HpRelayShaper(CompressorGate& gate, const Config& cfg)
    : gate_(gate), cfg_(cfg) {
  if (cfg_.maxStartsPerH == 0) cfg_.maxStartsPerH = 1;
  if (cfg_.maxStartsPerH > kMaxStartHistory) cfg_.maxStartsPerH = kMaxStartHistory;
}

uint8_t HpRelayShaper::startsInLastHour(uint32_t nowS) const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < startCount_; ++i) {
    if (elapsedS(nowS, startTimesS_[i]) < 3600) ++n;
  }
  return n;
}

bool HpRelayShaper::startBudgetOk(uint32_t nowS) const {
  return startsInLastHour(nowS) < cfg_.maxStartsPerH;
}

void HpRelayShaper::recordStart(uint32_t nowS) {
  startTimesS_[startHead_] = nowS;
  startHead_ = static_cast<uint8_t>((startHead_ + 1) % kMaxStartHistory);
  if (startCount_ < kMaxStartHistory) ++startCount_;
}

float HpRelayShaper::shape(float requestPct, uint32_t nowS) {
  if (invalidRequest(requestPct)) {
    inputAlarm_ = true;
    requestPct = 0.0f;  // fall through: the drop still honors the min-on gate
  }
  requestPct = clampTop(requestPct);

  const float duty = requestPct / 100.0f;
  const float periodS = 3600.0f / static_cast<float>(cfg_.maxStartsPerH);

  if (on_) {
    const bool continuous = duty >= 1.0f;
    const float onTimeS = duty * periodS;
    const bool wantOff =
        duty <= 0.0f ||
        (!continuous && static_cast<float>(elapsedS(nowS, phaseStartS_)) >= onTimeS);
    if (wantOff && gate_.canStop(nowS)) {  // min-on honored even on the way down (docs/04 §1)
      on_ = false;
      phaseStartS_ = nowS;
    }
  } else if (duty > 0.0f) {
    const float offTimeS = (1.0f - duty) * periodS;
    const bool offPhaseDone =
        !everCycled_ ||  // first call from idle: boot hold-off belongs to the gate
        static_cast<float>(elapsedS(nowS, phaseStartS_)) >= offTimeS;
    if (offPhaseDone && startBudgetOk(nowS) && gate_.canStart(nowS)) {
      on_ = true;
      everCycled_ = true;
      phaseStartS_ = nowS;
      recordStart(nowS);
    }
  }

  return on_ ? 100.0f : 0.0f;
}

void HpRelayShaper::reset() {
  // Start history survives reset on purpose: a soft restart must not refill
  // the starts-per-hour budget (docs/04 §2 reset-loop row).
  on_ = false;
  everCycled_ = false;
  phaseStartS_ = 0;
}

}  // namespace dettson
