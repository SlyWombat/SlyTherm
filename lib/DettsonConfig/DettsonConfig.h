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

// ---------- Safety supervision (SafetySupervisor) ----------
constexpr uint32_t kBusDeadmanS             = 30;    // range 10-120; continuous CT-485 silence -> demand-drop
                                                     //  request + critical alarm (docs/04 §3 comms-loss deadman);
                                                     //  petting continues — a reboot cannot revive a dead bus,
                                                     //  and our silence already drops calls equipment-side
constexpr uint32_t kBootValidationGraceS    = 120;   // range 60-600; boot gate (sensor OK + setpoint present +
                                                     //  config CRC ok, docs/04 §3) still closed after this ->
                                                     //  alarm (loss of heat is itself a hazard, docs/04 §4);
                                                     //  the gate stays closed — the alarm is for visibility

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

// ---------- Staged cooling (StagedCoolShaper; issue #140, docs/13 §4) ----------
// Equipment truth (field-confirmed, 2026-07-08 CT-485 capture + 24 h shadow
// run): this furnace's cooling is SINGLE-STAGE, engaged at CT-485 demand 30%
// — the only value the OEM stat ever sends; commanding more is meaningless.
// "Long-low modulation" for cooling therefore means RUNTIME shaping: PID
// error -> runtime fraction -> slow on/off duty at kCoolStagePct.
// Timing defaults are derived from the 2026-07-09/10 shadow-vs-OEM field
// data (see test/test_cool_replay and the derivation in DemandShaper.cpp).
constexpr float    kCoolStagePct            = 30.0f;  // the stage's engage demand
constexpr uint8_t  kCoolMaxStartsPerH       = 2;      // hard demand-level cap (< guard's 3)
constexpr uint32_t kCoolMinOnS              = 780;    // 13 min; OEM's shortest observed run was 14.5 min
constexpr uint32_t kCoolMinOffS             = 480;    // 8 min demand-level rest (guard min-off 300 stays downstream)
constexpr uint32_t kCoolCyclePeriodS        = 4500;   // base duty period ~= OEM's mean cycle period (45-115 min observed)
constexpr float    kCoolFullDutyErrC        = 0.45f;  // proportional band top: err >= this -> continuous run

// ---------- Relay sequencing (RelaySequencer; Case B, docs/03 §7) ----------
constexpr uint32_t kRelayMinTransitionMs    = 500;   // min spacing between relay output transitions
                                                     //  (contact-chatter guard; goSilent/watchdog never spaced)
// Gree reversing-valve convention: B = ENERGIZED IN HEATING — opposite the
// common O=cool default; a wrong guess inverts heat/cool. Verify polarity on
// the installed equipment before field use (docs/04 §6 checklist).
constexpr bool     kObEnergizedIsHeat       = true;

// ---------- CT-485 demand TX (Ct485Thermostat) ----------
// Refresh-timer byte written into every demand frame: high nibble = minutes,
// low nibble = 3.75 s units (docs/02 §5a). 0x10 = 60 s. The equipment reverts
// the channel to off if the demand is not re-sent inside this window — the
// protocol's own per-channel deadman (docs/04 §3 refresh discipline).
constexpr uint8_t  kDemandRefreshTimerByte  = 0x10;
// Re-emit each active demand within this fraction of its refresh window
// (range 0.1-0.9). A full window elapsing with no successful re-send (token
// starved) raises the starvation alarm AND goes silent — never a retry storm.
constexpr float    kDemandRefreshFraction   = 0.5f;

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

// ---------- UI screen lock (UiModel; issue #45) ----------
// The lock blocks change intents only — it NEVER hides alarms, current temp,
// or status (docs/04 §1c: UI is a comfort layer; visibility is part of the
// alarming requirement in docs/04 §3).
constexpr uint32_t kUiAutoRelockS           = 120;   // range 30-600; inactivity -> relock (also expires installer access)
constexpr uint8_t  kUiPinMaxAttempts        = 5;     // range 3-10; failed PIN entries before backoff
constexpr uint32_t kUiPinBackoffS           = 60;    // range 30-600; PIN entry blocked after attempts exhausted

// ---------- Smart recovery (RecoveryEstimator; issue #50) ----------
// ADVISORY ONLY: the estimator recommends an early start for a scheduled
// setpoint change; ModeStateMachine/main glue decides whether to act, and
// CompressorGuard/DualFuelArbiter still gate every demand. Disabled by
// default until field-tuned (docs/06 "Smart recovery").
constexpr bool     kRecoveryEnabledDefault    = false;
constexpr float    kRecoverySeedHeatCPerH     = 1.0f;  // ramp-rate seeds used per {mode, equipment}
constexpr float    kRecoverySeedCoolCPerH     = 0.8f;  //  channel until that channel has learned
constexpr uint32_t kRecoveryMaxLookaheadS     = 7200;  // hard cap on any early-start recommendation
constexpr uint32_t kRecoveryMinSegmentS       = 900;   // learning gates: run segments shorter than
constexpr float    kRecoveryMinSegmentDeltaC  = 0.2f;  //  15 min or moving < 0.2 C are ignored
constexpr float    kRecoveryEmaAlpha          = 0.3f;  // per-segment-rate EMA weight
constexpr float    kRecoveryRateMinCPerH      = 0.1f;  // absolute plausibility band on a segment
constexpr float    kRecoveryRateMaxCPerH      = 10.0f; //  rate; outside -> segment rejected
constexpr float    kRecoveryOutlierRatio      = 3.0f;  // after kRecoveryOutlierMinSamples accepted
constexpr uint8_t  kRecoveryOutlierMinSamples = 3;     //  segments, reject rates > ratio off the
                                                       //  estimate (robust EMA, docs/05 table)

// ---------- Fallback / degraded modes ----------
constexpr float    kFallbackHeatSetpointC   = 18.0f;  // MQTT stale >30 min: dual-bounded, last user mode,
constexpr float    kFallbackCoolSetpointC   = 27.0f;  //  never escalate OFF (27-28 acceptable)
constexpr uint32_t kMqttStaleS              = 1800;
constexpr float    kDegradedHeatFloorC      = 16.0f;  // DS18B20-only degraded mode: heat-to floor 16-18,
constexpr float    kDegradedHeatCeilC       = 18.0f;  //  cooling disabled (or >=29 C ceiling), demand capped
constexpr float    kCoolingIndoorLockoutC   = 18.0f;  // never cool when indoor below this

}  // namespace dettson
