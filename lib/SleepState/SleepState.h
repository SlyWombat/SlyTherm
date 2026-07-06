// SleepState.h — night "Sleep" presence state (issue #90).
//
// THE MODEL. Away (#88) means nobody home: no presence sensor's HA last_seen
// within kPresenceAwayS (3 h). Sleep is different: people ARE home but asleep
// overnight, so motion drops to near-zero for hours and the 3 h away timer
// would wrongly flip the house to Away mid-night. Sleep is therefore a state
// ON TOP of the #88 presence ledger, never a replacement for it:
//
//   trigger    Default: the night window (kSleepStartHour..kSleepEndHour,
//              00:00-06:00 — the SAME window as the #86 night deep-blank, so
//              display and behavior stay coherent) plus a touch-idle guard
//              (kSleepIdleS): the panel must be untouched for 30 min. That one
//              rule collapses enter/exit/re-entry: a 2 am touch wakes the
//              system instantly, and 30 idle minutes later (still night) it
//              slides back to Sleep on its own. An optional HA override
//              (slytherm/cmd/sleep: "on"/"off"/"auto") forces the state either
//              way — e.g. a bedroom presence sensor or HA's own sleep mode.
//   presence   Away WINS over Sleep: with presence sensors reporting "nobody
//              home" the house does not enter Sleep (an empty house is Away,
//              and away setpoints are typically deeper). While asleep the
//              away countdown is FROZEN, not bypassed: awayCreditS() returns
//              the seconds of sleep overlapping [last_seen, now], and the
//              caller re-evaluates presenceWithin(kPresenceAwayS + credit).
//              Net effect: last seen 23:30, asleep 00:00-06:00 -> Present all
//              night, and after wake the timer resumes from 30 min — Away can
//              only fire after 3 h of AWAKE time without a sighting. Exiting
//              Sleep therefore resumes the normal 3 h logic with no cliff.
//   preset     The Sleep STATE drives the existing `sleep` setpoint PRESET,
//              reversibly (SleepPresetLink below): the enter edge snapshots
//              the active preset/setpoints and applies "sleep" under the
//              normal preset rules (a timed/indefinite hold blocks it, like
//              any scheduled preset); the exit edge restores the snapshot —
//              but ONLY if nothing changed overnight (activePreset still
//              "sleep", setpoints still the sleep pair). Any overnight user/HA
//              intervention wins and is left alone.
//   clock      getLocalTime() failing (no NTP yet) fails SAFE: never asleep,
//              and an in-progress Sleep exits — identical to the #86 blank
//              rule. No clock, no window.
//
// Pure C++17, no Arduino. Time is injected as uint32_t nowS (monotonic
// seconds); the local hour is injected by the caller from getLocalTime().

#pragma once
#include <cstdint>

#include "DettsonConfig.h"  // kPresetNameMaxLen

namespace dettson {

// Forward-declared on purpose: slytherm_ui.cpp includes this header for the
// shared night-window constants under `using namespace dettson`, and pulling
// in ModeStateMachine.h there would collide dettson::UserMode with
// dettson::ui::UserMode. Only SleepState.cpp needs the full definition.
class ModeStateMachine;

// Night window shared with the #86 deep-blank (slytherm_ui.cpp uses these same
// constants so the dark screen and the Sleep state cover the same hours).
constexpr uint8_t kSleepStartHour = 0;   // window start, local hour [0..23]
constexpr uint8_t kSleepEndHour   = 6;   // window end (exclusive), local hour
// Touch-idle guard: the panel must be untouched this long before (re)entering
// Sleep. Doubles as the wake latch: a night-time touch exits Sleep and holds
// it off until the panel has been idle this long again.
constexpr uint32_t kSleepIdleS = 30u * 60u;

enum class SleepOverride : uint8_t {
  kAuto = 0,     // night window + idle guard decide (default)
  kForceAsleep,  // HA says sleeping — asleep regardless of window/presence
  kForceAwake,   // HA says awake — never asleep
};

class SleepState {
 public:
  struct Config {
    uint8_t  startHour = kSleepStartHour;  // window may wrap midnight (22..6)
    uint8_t  endHour   = kSleepEndHour;
    uint32_t idleS     = kSleepIdleS;
  };

  SleepState() = default;
  explicit SleepState(const Config& cfg) : cfg_(cfg) {}

  // HA override hook (retained slytherm/cmd/sleep). kAuto returns control to
  // the night window.
  void setOverride(SleepOverride o) { override_ = o; }
  SleepOverride overrideMode() const { return override_; }

  // Any deliberate wall-panel touch. Exits Sleep (evaluated at the next
  // update()) and restarts the idle guard.
  void noteTouch(uint32_t nowS) { hasTouch_ = true; lastTouchS_ = nowS; }

  // Evaluate once per control cycle. clockValid/hour from getLocalTime()
  // (hour ignored when !clockValid); present/anyReporting from the CREDITED
  // presence evaluation (see awayCreditS). Returns the new asleep state.
  bool update(bool clockValid, int hour, bool present, bool anyReporting,
              uint32_t nowS);

  bool asleep() const { return asleep_; }

  // Away-timer freeze (#88 interaction): seconds of the current/most recent
  // sleep interval overlapping [last_seen, now]. The caller widens the away
  // window by this: presenceWithin(kPresenceAwayS + credit). 0 when never
  // slept or lastSeenAgeS is "never" (0xFFFFFFFF).
  uint32_t awayCreditS(uint32_t lastSeenAgeS, uint32_t nowS) const;

  // True when `hour` falls in [startH, endH); supports windows that wrap
  // midnight (startH > endH, e.g. 22..6). startH == endH -> never.
  static bool inNightWindow(int hour, uint8_t startH, uint8_t endH);

 private:
  Config        cfg_;
  SleepOverride override_ = SleepOverride::kAuto;
  bool          asleep_   = false;
  bool          hasTouch_ = false;
  uint32_t      lastTouchS_ = 0;
  bool          hasSlept_   = false;  // a sleep interval exists
  uint32_t      sleepStartS_ = 0;     // current/most recent interval
  uint32_t      sleepEndS_   = 0;     // meaningful when !asleep_
};

// Reversible Sleep-state -> `sleep`-preset glue (issue #90). Call onEdge()
// whenever SleepState::update() changes state. Enter: snapshot the active
// preset + setpoints, applyPreset("sleep") under the normal rules. Exit:
// restore the snapshot only if the night was untouched (activePreset still
// "sleep" and setpoints still the pair the sleep preset produced); a missing
// previous preset restores the raw setpoints through the time-less setters
// (no hold fabricated). No `sleep` preset in the roster -> inert.
class SleepPresetLink {
 public:
  static constexpr const char* kSleepPresetName = "sleep";

  void onEdge(bool asleep, ModeStateMachine& sm, uint32_t nowS);
  bool applied() const { return applied_; }  // sleep preset currently applied by us

 private:
  bool  applied_ = false;
  char  prevPreset_[kPresetNameMaxLen + 1] = {0};  // "" = none active at entry
  float prevHeatC_  = 0.0f;
  float prevCoolC_  = 0.0f;
  float sleepHeatC_ = 0.0f;  // pair applyPreset("sleep") produced (post-deadband)
  float sleepCoolC_ = 0.0f;
};

}  // namespace dettson
