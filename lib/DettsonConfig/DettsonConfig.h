// DettsonConfig.h — canonical control/safety default parameters.
//
// Single source of truth in code for the defaults table in
// docs/05-firmware-plan.md ("Canonical default parameters"). If a value
// changes, change it in the doc AND here. All temperatures are degrees C,
// all durations are seconds unless suffixed otherwise. Values are
// runtime-tunable (HA-editable MQTT numbers) but these are the boot/NVS-reset
// defaults; tunables must be range-clamped to the documented bounds.
//
// Pure C++17, no Arduino dependencies.

#pragma once
#include <cstdint>

namespace dettson {

// ---------- Compressor protection (CompressorGuard) ----------
constexpr uint32_t kCompressorMinOffS       = 300;   // range 240-900
constexpr uint32_t kCompressorMinOnS        = 300;   // range 60-1200
constexpr uint8_t  kCompressorMaxStartsPerH = 3;
constexpr uint32_t kBootCompressorHoldoffS  = 300;   // + 0-60 s jitter; if persisted state unknown -> full hold-off
constexpr uint8_t  kResetLoopLockoutCount   = 3;     // >=3 watchdog/brownout resets...
constexpr uint32_t kResetLoopWindowS        = 1800;  // ...in 30 min -> latched NO-DEMAND, manual clear

// ---------- Setpoints, deadband, changeover (ModeStateMachine) ----------
constexpr float    kCallHysteresisC         = 0.55f; // ~1 degF; Ecobee's 0.5 degF default short-cycles
constexpr float    kMinSetpointDeltaC       = 2.8f;  // auto-mode heat/cool deadband default (5 degF)
constexpr float    kMinSetpointDeltaFloorC  = 1.1f;  // hard floor (2 degF); clamp + push other setpoint
constexpr uint32_t kChangeoverDwellS        = 1800;  // 30 min since opposite call
constexpr uint32_t kChangeoverSustainS      = 600;   // trigger must persist >=10 min

// ---------- Presets & holds (ModeStateMachine; docs/07 gap G4) ----------
// Hold semantics are Ecobee-style: a manual setpoint/mode change creates a
// hold of kDefaultHoldType; presets are ignored while held, except an
// until-next-preset hold ends when the next preset arrives.
enum class HoldType : uint8_t {
  kNone = 0,            // no hold (also a valid default: manual changes hold nothing)
  kUntilNextPreset,
  kTwoHours,
  kFourHours,
  kIndefinite,          // ends only on explicit clear
};
constexpr HoldType kDefaultHoldType = HoldType::kUntilNextPreset;
constexpr uint32_t kHoldShortS      = 7200;   // "2 h" hold
constexpr uint32_t kHoldLongS       = 14400;  // "4 h" hold
constexpr uint8_t  kMaxPresets      = 8;      // config-driven roster cap
constexpr uint8_t  kPresetNameMaxLen = 23;    // bytes, excluding nul

// ---------- Dual fuel (DualFuelArbiter) ----------
constexpr float    kBalancePointC           = -8.0f;  // mirror R02P034 P124 range -30..15
constexpr float    kBalancePointHystC       = 2.0f;
constexpr float    kCompressorMinOatC       = -20.0f; // set from installed model submittal (FLEXX rated -30)
constexpr float    kAuxMaxOatC              = 10.0f;  // gas/aux lockout above this OAT
constexpr float    kEscalationDroopC        = 1.0f;   // droop below setpoint...
constexpr uint32_t kEscalationMinS          = 1800;   // ...for >=30 min at >=95% HP demand -> stage gas
constexpr float    kEscalationHpDemandPct   = 95.0f;
constexpr uint32_t kDeescalationMinS        = 3600;   // stage back after 60 min + OAT above balance + hyst

// ---------- Gas modulation (GasShaper) ----------
constexpr float    kGasFloorPct             = 40.0f;  // Chinook low fire; valid demand is 0 or 40-100
constexpr uint32_t kGasMaxRuntimeS          = 14400;  // 4 h continuous -> drop + alarm (HP: alarm only, never auto-cycle)
constexpr uint32_t kGasMinOnS               = 300;    // range 60-900; comfort stops only — safety stops always immediate (docs/07 G14)
constexpr uint32_t kGasMinOffS              = 300;    // range 60-900; boot starts the off-timer fresh unless persisted state proves it served

// ---------- Defrost tempering ----------
constexpr float    kDefrostTemperHeatPct    = 35.0f;  // fixed, never PID
constexpr uint32_t kDefrostTemperMaxS       = 900;    // 15 min hard cap

// ---------- HP inverter shaping (if a % path exists — Phase 2 answers) ----------
constexpr float    kHpSlewPctPerMin         = 10.0f;
constexpr float    kHpStepPct               = 5.0f;
constexpr float    kHpFloorPct              = 30.0f;  // verify against installed model

// ---------- Sensor fusion (SensorFusion) ----------
constexpr uint32_t kSensorMaxAgeS           = 300;    // per-sensor staleness, configurable 180-900
constexpr uint32_t kSensorHeartbeatS        = 60;     // HA bridge republish heartbeat
constexpr float    kSensorRangeMinC         = 5.0f;
constexpr float    kSensorRangeMaxC         = 40.0f;
constexpr float    kSensorOutlierC          = 4.0f;   // >4 C from median -> exclude + alarm
constexpr float    kOccupiedWeight          = 2.0f;   // vs 1.0 unoccupied
constexpr uint32_t kOccupancyWindowS        = 1800;   // 30 min "follow me" window
constexpr uint32_t kWeightRampTauMinS       = 600;    // weight phase-in/out tau 10-30 min (tunable;
constexpr uint32_t kWeightRampTauMaxS       = 1800;   //  Ecobee constants unpublished)
constexpr uint32_t kFusionSmoothingTauMinS  = 120;    // EMA tau 2-5 min
constexpr uint32_t kFusionSmoothingTauMaxS  = 300;
constexpr float    kFusionSlewCPerMin       = 0.1f;   // slew limit on participant-set changes
constexpr float    kDs18b20DisagreeAlarmC   = 5.0f;   // fallback sensor vs fusion aggregate sanity
constexpr float    kSensorOffsetMaxC        = 5.0f;   // per-sensor calibration offset clamp (docs/07 G6)

// ---------- Outdoor temperature (OutdoorTempSource) ----------
constexpr uint32_t kOatStaleS               = 1800;   // 30 min -> next rung (bus -> wired -> HA weather)
constexpr float    kOatRungDisagreeAlarmC   = 5.0f;
// All rungs stale -> fail cold: gas allowed, compressor locked out;
// cooling locked out per the indoor-18 C policy.

// ---------- Fallback / degraded modes ----------
constexpr float    kFallbackHeatSetpointC   = 18.0f;  // MQTT stale >30 min: dual-bounded, last user mode,
constexpr float    kFallbackCoolSetpointC   = 27.0f;  //  never escalate OFF (27-28 acceptable)
constexpr uint32_t kMqttStaleS              = 1800;
constexpr float    kDegradedHeatFloorC      = 16.0f;  // DS18B20-only degraded mode: heat-to floor 16-18,
constexpr float    kDegradedHeatCeilC       = 18.0f;  //  cooling disabled (or >=29 C ceiling), demand capped
constexpr float    kCoolingIndoorLockoutC   = 18.0f;  // never cool when indoor below this

}  // namespace dettson
