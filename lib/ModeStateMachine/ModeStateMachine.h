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

class ModeStateMachine {
 public:
  struct Config {
    float    hysteresisC        = kCallHysteresisC;
    float    minSetpointDeltaC  = kMinSetpointDeltaC;  // clamped >= floor on apply
    uint32_t changeoverDwellS   = kChangeoverDwellS;
    uint32_t changeoverSustainS = kChangeoverSustainS;
    float    setpointMinC       = 5.0f;   // sane absolute write bounds
    float    setpointMaxC       = 40.0f;
  };

  struct SetpointResult {
    bool  accepted    = false;  // false: invalid write (NaN / out of range), nothing changed
    bool  heatChanged = false;
    bool  coolChanged = false;
    float heatC       = 0.0f;   // resulting values, for the HA echo
    float coolC       = 0.0f;
  };

  ModeStateMachine();
  explicit ModeStateMachine(const Config& cfg);

  // Mode change ends any active call immediately (next update() re-evaluates).
  void setMode(UserMode mode, uint32_t nowS);
  UserMode mode() const { return mode_; }

  SetpointResult setHeatSetpoint(float c);
  SetpointResult setCoolSetpoint(float c);
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
  Call evalSimple(CallType want, float spC, float tempC, uint32_t nowS,
                  bool gasOnly);
  Call evalAuto(float tempC, uint32_t nowS, bool compressorSwapOk);

  Config   cfg_;
  UserMode mode_ = UserMode::kOff;  // boot = OFF/no demand until restored & validated
  float    heatSpC_ = kFallbackHeatSetpointC;  // dual-bounded fallback pair as boot
  float    coolSpC_ = kFallbackCoolSetpointC;  //   defaults (docs/05 table)
  Call     call_;
  bool     inputFaultAlarm_ = false;

  CallType lastEndedCall_ = CallType::kNone;  // most recently ended call type
  bool     hadHeatEnd_ = false, hadCoolEnd_ = false;
  uint32_t heatEndS_ = 0, coolEndS_ = 0;
  bool     heatTrigArmed_ = false, coolTrigArmed_ = false;
  uint32_t heatTrigStartS_ = 0, coolTrigStartS_ = 0;
};

}  // namespace dettson
