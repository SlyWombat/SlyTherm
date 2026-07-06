// SleepState.cpp — see SleepState.h for the state model (issue #90).

#include "SleepState.h"

#include <cmath>
#include <cstring>

#include "ModeStateMachine.h"  // full definition (header only forward-declares)

namespace dettson {

bool SleepState::inNightWindow(int hour, uint8_t startH, uint8_t endH) {
  if (hour < 0 || hour > 23) return false;
  if (startH == endH) return false;              // zero-length window: never
  if (startH < endH) return hour >= startH && hour < endH;   // e.g. 0..6
  return hour >= startH || hour < endH;          // wraps midnight, e.g. 22..6
}

bool SleepState::update(bool clockValid, int hour, bool present,
                        bool anyReporting, uint32_t nowS) {
  bool want;
  switch (override_) {
    case SleepOverride::kForceAsleep: want = true;  break;
    case SleepOverride::kForceAwake:  want = false; break;
    default: {
      // No clock -> fail safe: never asleep (same rule as the #86 blank).
      const bool inWindow =
          clockValid && inNightWindow(hour, cfg_.startHour, cfg_.endHour);
      // Idle guard: a recent touch keeps (or knocks) us awake; it re-arms by
      // itself once the panel has been idle for cfg_.idleS.
      const bool idleOk = !hasTouch_ || (nowS - lastTouchS_) >= cfg_.idleS;
      // Away wins over Sleep: an empty house stays Away. With no presence
      // sensor reporting there is no away timer to suppress, but the window
      // still drives the preset/display.
      const bool awayBlocked = anyReporting && !present;
      want = inWindow && idleOk && !awayBlocked;
      break;
    }
  }
  if (want && !asleep_) {           // enter
    asleep_ = true;
    hasSlept_ = true;
    sleepStartS_ = nowS;
  } else if (!want && asleep_) {    // exit
    asleep_ = false;
    sleepEndS_ = nowS;
  }
  return asleep_;
}

uint32_t SleepState::awayCreditS(uint32_t lastSeenAgeS, uint32_t nowS) const {
  if (!hasSlept_ || lastSeenAgeS == 0xFFFFFFFFu) return 0;
  const uint32_t seenS = lastSeenAgeS >= nowS ? 0 : nowS - lastSeenAgeS;
  const uint32_t endS = asleep_ ? nowS : sleepEndS_;
  const uint32_t fromS = sleepStartS_ > seenS ? sleepStartS_ : seenS;
  return endS > fromS ? endS - fromS : 0;
}

void SleepPresetLink::onEdge(bool asleep, ModeStateMachine& sm, uint32_t nowS) {
  if (asleep) {  // ---- enter: snapshot + apply the sleep preset ----
    applied_ = false;
    if (sm.presetByName(kSleepPresetName) == nullptr) return;  // no roster entry
    // Snapshot BEFORE the apply so the exit edge can put it all back.
    std::strncpy(prevPreset_, sm.activePreset(), sizeof(prevPreset_) - 1);
    prevPreset_[sizeof(prevPreset_) - 1] = '\0';
    prevHeatC_ = sm.heatSetpoint();
    prevCoolC_ = sm.coolSetpoint();
    const ModeStateMachine::PresetResult r = sm.applyPreset(kSleepPresetName, nowS);
    if (!r.applied) return;  // blocked by a hold: user intent wins, restore nothing
    applied_ = true;
    sleepHeatC_ = sm.heatSetpoint();  // post-deadband pair, for the untouched check
    sleepCoolC_ = sm.coolSetpoint();
    return;
  }
  // ---- exit: restore only an untouched night ----
  if (!applied_) return;
  applied_ = false;
  const bool untouched =
      std::strcmp(sm.activePreset(), kSleepPresetName) == 0 &&
      std::fabs(sm.heatSetpoint() - sleepHeatC_) < 0.05f &&
      std::fabs(sm.coolSetpoint() - sleepCoolC_) < 0.05f;
  if (!untouched) return;  // overnight user/HA change wins
  if (prevPreset_[0] != '\0' && sm.presetByName(prevPreset_) != nullptr) {
    sm.applyPreset(prevPreset_, nowS);  // normal rules; a fresh hold blocks it
  } else {
    sm.setHeatSetpoint(prevHeatC_);  // time-less setters: no hold fabricated
    sm.setCoolSetpoint(prevCoolC_);
    sm.clearActivePreset();  // label no longer true (setpoints are the snapshot's)
  }
}

}  // namespace dettson
