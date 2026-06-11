// ModeStateMachine.h — user mode + dual setpoints -> effective call.
//
// OFF / HEAT / COOL / AUTO / EMERGENCY_HEAT (bus BACKUP_HEAT naming;
// gas-only — the compressor is never requested in EMERGENCY_HEAT).
// First stage of the Phase 4 control pipeline (docs/05): the Call emitted
// here feeds DualFuelArbiter -> CompressorGuard -> shapers -> DemandArbiter;
// this module has no demand authority of its own.
//
// Deadband (docs/05 defaults table): cool setpoint >= heat setpoint +
// minSetpointDeltaC (default 2.8 C). A setpoint write violating it is
// honored by PUSHING the other setpoint, Ecobee-style; the result reports
// what changed so HA can echo the moved value. The delta itself is
// runtime-tunable but hard-clamped to kMinSetpointDeltaFloorC (1.1 C).
//
// Presets + holds (docs/07 gap G4): a config-defined roster of up to
// kMaxPresets named setpoint pairs replaces the fixed home/away/sleep set.
// applyPreset() writes the pair under the same deadband rules (a violating
// pair resolves with the cool value winning — heat is pushed down). Manual
// setpoint/mode changes create a hold of the configured default type;
// while a hold is active incoming presets are ignored, EXCEPT an
// until-next-preset hold ends when the next (valid) preset arrives. Timed
// holds expire by clock (checked in update()/applyPreset()); indefinite
// holds end only on clearHold(). Timed-hold expiry does NOT revert
// setpoints — HA owns the schedule and re-publishes the preset.
//
// AUTO changeover (docs/05 table): swapping to the opposite call requires
// the trigger sustained >= changeoverSustainS AND >= changeoverDwellS since
// the opposite call ended AND the caller-supplied compressorSwapOk gate
// (CompressorGuard verdict — either call direction of the swap may involve
// the compressor, so the caller queries the guard).
//
// Safety (docs/04 §2 sensor row, binding): boot state = no call; invalid
// temperature input (tempValid=false or NaN) -> call drops to none and
// inputFaultAlarm() raises. Recovery re-enters via normal hysteresis; the
// downstream guard owns compressor-timer protection on the way back up.
//
// Pure C++17, no Arduino dependencies; time injected as uint32_t nowS.

#pragma once
#include <cstddef>
#include <cstdint>

#include "DettsonConfig.h"

namespace dettson {

enum class UserMode : uint8_t { kOff = 0, kHeat, kCool, kAuto, kEmergencyHeat };

enum class CallType : uint8_t { kNone = 0, kHeat, kCool };

struct Call {
  CallType type   = CallType::kNone;
  float    errorC = 0.0f;  // strength context: degrees past the active setpoint
  bool     gasOnly = false;  // true only in EMERGENCY_HEAT — compressor prohibited
};

struct PresetDef {
  char  name[kPresetNameMaxLen + 1] = {0};
  float heatC = 0.0f;
  float coolC = 0.0f;
};

class ModeStateMachine {
 public:
  struct Config {
    float    hysteresisC        = kCallHysteresisC;
    float    minSetpointDeltaC  = kMinSetpointDeltaC;  // clamped >= floor on apply
    uint32_t changeoverDwellS   = kChangeoverDwellS;
    uint32_t changeoverSustainS = kChangeoverSustainS;
    float    setpointMinC       = 5.0f;   // sane absolute write bounds
    float    setpointMaxC       = 40.0f;
    HoldType defaultHoldType    = kDefaultHoldType;  // kNone: manual changes hold nothing
    uint32_t holdShortS         = kHoldShortS;
    uint32_t holdLongS          = kHoldLongS;
  };

  struct SetpointResult {
    bool  accepted    = false;  // false: invalid write (NaN / out of range), nothing changed
    bool  heatChanged = false;
    bool  coolChanged = false;
    float heatC       = 0.0f;   // resulting values, for the HA echo
    float coolC       = 0.0f;
  };

  struct PresetResult {
    bool applied       = false;  // false: unknown name or blocked by a hold
    bool blockedByHold = false;
    SetpointResult setpoints;    // valid only when applied
  };

  ModeStateMachine();
  explicit ModeStateMachine(const Config& cfg);

  // Mode change ends any active call immediately (next update() re-evaluates)
  // and, when the mode actually changes, creates the default-type hold
  // (manual change, Ecobee semantics).
  void setMode(UserMode mode, uint32_t nowS);
  UserMode mode() const { return mode_; }

  // EM HEAT toggle (docs/07 G15 — the HA switch path, orthogonal to comfort
  // presets). on=true behaves as setMode(kEmergencyHeat); on=false restores
  // the mode that was active when EMERGENCY_HEAT was entered (by either this
  // toggle or a direct setMode, e.g. from the wall UI). No-op when off while
  // not engaged.
  void setEmergencyHeat(bool on, uint32_t nowS);
  bool emergencyHeat() const { return mode_ == UserMode::kEmergencyHeat; }

  // Time-less setters: restore/preset paths — no hold is created.
  SetpointResult setHeatSetpoint(float c);
  SetpointResult setCoolSetpoint(float c);
  // Manual-write setters: an accepted write that changed either setpoint
  // clears the active preset and creates the default-type hold.
  SetpointResult setHeatSetpoint(float c, uint32_t nowS);
  SetpointResult setCoolSetpoint(float c, uint32_t nowS);

  // ----- Preset roster (docs/07 gap G4) -----
  // Atomically replaces the roster. Entries with an empty name, a duplicate
  // name, or a NaN/out-of-bounds setpoint are skipped; capped at kMaxPresets.
  // Returns the accepted count. An active preset missing from the new roster
  // is cleared (setpoints untouched).
  size_t setPresetRoster(const PresetDef* defs, size_t count);
  size_t presetCount() const { return rosterCount_; }
  const PresetDef* presetByName(const char* name) const;

  // Applies a roster preset's setpoint pair (deadband rules per header).
  // Hold interaction: timed/indefinite hold -> blocked; until-next-preset
  // hold -> ends and the preset applies. An unknown name does NOT end a hold.
  PresetResult applyPreset(const char* name, uint32_t nowS);
  const char* activePreset() const { return activePreset_; }  // "" when none

  // ----- Holds -----
  void startHold(HoldType t, uint32_t nowS);  // kNone behaves as clearHold()
  void clearHold() { hold_ = HoldType::kNone; }
  // Expiry of timed holds is applied by update()/applyPreset(); this reports
  // the state as of the last of those calls.
  HoldType activeHoldType() const { return hold_; }
  uint32_t holdRemainingS(uint32_t nowS) const;  // timed holds only, else 0
  // Clamped to [kMinSetpointDeltaFloorC, ...]; widening pushes the cool
  // setpoint up if the current pair now violates it. Returns applied delta.
  float setMinSetpointDelta(float deltaC);

  float heatSetpoint() const { return heatSpC_; }
  float coolSetpoint() const { return coolSpC_; }
  float minSetpointDelta() const { return cfg_.minSetpointDeltaC; }

  // One control-cycle step. compressorSwapOk: caller's CompressorGuard
  // verdict, gating AUTO changeover only (pass true when no compressor is
  // involved in the swap).
  Call update(float tempC, bool tempValid, uint32_t nowS,
              bool compressorSwapOk = true);

  Call call() const { return call_; }
  bool inputFaultAlarm() const { return inputFaultAlarm_; }

 private:
  void endActiveCall(uint32_t nowS);
  void startCall(CallType t, float tempC, bool gasOnly);
  void resetTriggers();
  void tickHold(uint32_t nowS);  // expires timed holds
  void onManualChange(uint32_t nowS);
  Call evalSimple(CallType want, float spC, float tempC, uint32_t nowS,
                  bool gasOnly);
  Call evalAuto(float tempC, uint32_t nowS, bool compressorSwapOk);

  Config   cfg_;
  UserMode mode_ = UserMode::kOff;  // boot = OFF/no demand until restored & validated
  UserMode priorMode_ = UserMode::kOff;  // mode to restore when EM HEAT disengages (G15)
  float    heatSpC_ = kFallbackHeatSetpointC;  // dual-bounded fallback pair as boot
  float    coolSpC_ = kFallbackCoolSetpointC;  //   defaults (docs/05 table)
  Call     call_;
  bool     inputFaultAlarm_ = false;

  PresetDef roster_[kMaxPresets];
  size_t    rosterCount_ = 0;
  char      activePreset_[kPresetNameMaxLen + 1] = {0};
  HoldType  hold_ = HoldType::kNone;
  uint32_t  holdEndS_ = 0;  // meaningful for timed holds only

  CallType lastEndedCall_ = CallType::kNone;  // most recently ended call type
  bool     hadHeatEnd_ = false, hadCoolEnd_ = false;
  uint32_t heatEndS_ = 0, coolEndS_ = 0;
  bool     heatTrigArmed_ = false, coolTrigArmed_ = false;
  uint32_t heatTrigStartS_ = 0, coolTrigStartS_ = 0;
};

}  // namespace dettson
