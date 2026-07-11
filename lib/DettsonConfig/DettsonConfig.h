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
#include <cstddef>
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

// ---------- Economic switchover (DualFuelArbiter; issue #143, docs/13 §1) ----------
// Break-even COP* = elec$/kWh × kGasKwhPerM3 × AFUE ÷ gas$/m3: gas heat costs
// gas$/m3 ÷ (kGasKwhPerM3 × AFUE) per kWh-thermal, the HP costs elec$/kWh ÷
// COP(OAT) — run the HP whenever COP(OAT) > COP*. Identical to docs/13 §1's
// per-therm form (COP* = elec$ × 29.3 × AFUE ÷ gas$/therm) because
// 1 therm = 100,000 BTU = 29.307 kWh = kGasM3PerTherm m³.
//
// Canonical gas unit is $/m³ (Ontario bills in m³). Energy content: 1 m³ of
// pipeline natural gas ≈ 38.0 MJ HHV ≈ 10.55 kWh (Enbridge billing heating
// value; varies ±2% month to month — the price uncertainty dwarfs it).
// Converting a $/therm price: $/m³ = $/therm ÷ kGasM3PerTherm (≈ ÷2.778).
//
// DEFAULT OFF: heating logic is unvalidatable until winter; when off the
// arbiter runs today's fixed balancePointC. Prices are placeholders until HA
// publishes real ones (retained slytherm/cmd/energy_prices, NVS-persisted);
// use the ALL-IN marginal rates (energy + delivery + carbon), not the bare
// commodity charge — the marginal cost is what switchover arbitrates.
constexpr bool   kDualFuelEconomicEnabledDefault = false;
constexpr float  kAfueDefault           = 0.95f;   // condensing furnace nameplate
constexpr float  kElecPricePerKwhDefault = 0.15f;  // $/kWh placeholder (ON TOU midband)
constexpr float  kGasPricePerM3Default  = 0.45f;   // $/m3 placeholder (ON all-in ballpark)
constexpr float  kEnergyPriceMax        = 10.0f;   // sanity ceiling on either price
constexpr float  kAfueMin               = 0.50f;   // plausibility band on AFUE
constexpr float  kGasKwhPerM3           = 10.55f;  // HHV energy content, see above
constexpr float  kKwhPerTherm           = 29.307f; // 100,000 BTU
constexpr float  kGasM3PerTherm         = kKwhPerTherm / kGasKwhPerM3;  // ~2.778
// Seed COP(OAT) curve: piecewise-linear points, oatC strictly increasing, COP
// non-decreasing; flat extrapolation beyond the ends. PLACEHOLDERS shaped from
// typical cold-climate ASHP (ccASHP/NEEP-class) published ratings — COP ~3.6
// at the +8.3 °C (47 °F) rating point, ~2.4 near -8.3 °C (17 °F), tailing to
// ~1.4 at -30 °C — pending the installed unit's (FLEXX) submittal data and the
// #143 record-only field learning below (CopLearner). NOT field truth yet.
struct CopPoint { float oatC; float cop; };
constexpr size_t kCopCurvePoints = 5;
constexpr CopPoint kCopCurveSeed[kCopCurvePoints] = {
    {-30.0f, 1.4f}, {-20.0f, 1.8f}, {-10.0f, 2.3f}, {0.0f, 2.9f}, {8.3f, 3.6f}};

// ---------- COP proxy learning (CopLearner; issue #143, docs/13 §5) ----------
// RECORD-ONLY: per-OAT-bucket degree-days-per-runtime-hour proxy from HP-heat
// runtime vs indoor-outdoor delta. Telemetry only ([copx] telnet line +
// retained slytherm/state/cop_proxy); it does NOT correct the COP table —
// that closes after a season of data (see CopLearner.h "correction seam").
constexpr float    kCopBucketWidthC   = 3.0f;
constexpr float    kCopBucketMinOatC  = -33.0f;  // buckets span -33..+18 C
constexpr size_t   kCopBucketCount    = 17;
constexpr uint32_t kCopTickMaxGapS    = 60;      // stalled-loop gap cap per tick
constexpr uint32_t kCopSaveMinS       = 900;     // NVS write throttle (flash wear)
constexpr uint32_t kCopPublishMinS    = 300;     // retained MQTT republish cadence
constexpr uint32_t kCopLogMinS        = 600;     // [copx] telnet line cadence

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
// #141 two-ramp scheduled recovery (docs/13 §2, Honeywell US 5,622,310):
// underneath the HP-alone recovery ramp sits a steeper FALLBACK ramp at the
// (derated) learned gas rate arriving at the same target — gas is advised
// only when the measured temperature falls below that line. OFF by default:
// heating validation is a WINTER task; code + unit tests land now.
constexpr bool     kRecoveryTwoRampEnabledDefault = false;
constexpr float    kRecoveryFallbackMargin        = 0.85f; // assume gas achieves only this fraction of
                                                           //  its learned ramp -> the line sits higher,
                                                           //  so gas engages a little early, never late

// ---------- Fused-temp trend + crossing prediction (issue #141, docs/13 §2) ----------
// TrendEstimator (lib/SensorFusion): EMA'd slope of the fused temperature,
// robust to fusion dropouts. Feeds RecoveryEstimator::crossingBias — an
// ADVISORY error bias into StagedCoolShaper::requestFromError so a predicted
// setpoint crossing begins a gentle early duty ramp instead of waiting for
// the deadband slam. All shaper hygiene + CompressorGuard stay downstream.
// Horizon/bias validated on the 2026-07-09/10 trace (test/test_cool_replay).
constexpr uint32_t kTrendTauS                 = 600;    // slope EMA time constant (10-20 min window)
constexpr uint32_t kTrendMaxGapS              = 600;    // valid-to-valid gap beyond this -> trend resets
constexpr uint32_t kTrendWarmupS              = 300;    // observed span before the slope is trusted
constexpr float    kTrendMaxSlopeCPerH        = 10.0f;  // per-sample clamp (spike rejection; matches
                                                        //  the kRecoveryRateMaxCPerH plausibility band)
constexpr bool     kCoolPredictEnabledDefault = true;   // advisory shaping only; replay-validated
constexpr uint32_t kCoolPredictHorizonS       = 900;    // act when a deadband crossing is inside 15 min
constexpr float    kCoolPredictBiasMaxC       = 0.10f;  // error bias ramps 0 -> this as crossing nears
constexpr float    kCoolPredictMinReqPct      = 50.0f;  // pre-action floor: below this the predicted
                                                        //  duty's first on-phase could end BEFORE the
                                                        //  call opens and fragment the cycle — at >=50%
                                                        //  duty the on-phase (>=37 min) always bridges
                                                        //  the horizon into the call (replay-tuned)

// ---------- Blower-first pre-circulation (PreCirculator; issue #142, docs/13 §3+§8) ----------
// When the #141 crossing prediction says a call is imminent (crossing within
// kBlowerFirstLeadS), run the blower LOW ahead of the stage: destratify +
// give SensorFusion a truthful whole-space reading BEFORE the commit.
constexpr bool     kBlowerFirstHeatEnabledDefault = true;   // docs/13 §3: heat-side default ON
// docs/13 §8 (the #144 literature verdict): OFF for cooling season — a
// pre-run before a cool call moves air over the wet coil and re-evaporates
// the PREVIOUS cycle's held condensate (~2 lb; effective SHR -> 1.0 at part
// load), a latent penalty paid 1:1 for the sensible "gain". Enable only as
// an explicit owner decision under verified-dry conditions.
constexpr bool     kBlowerFirstCoolEnabledDefault = false;
constexpr float    kBlowerFirstFanPct             = 25.0f;  // CT-485 fan Low (0x66 pct*2 = 0x32) —
                                                            //  the lowest field-confirmed speed
constexpr uint32_t kBlowerFirstLeadS              = 120;    // pre-run lead, range 60-180 (§3: 1-3 min)
constexpr uint32_t kBlowerFirstMaxRunS            = 600;    // cap when the prediction hovers without
                                                            //  the call ever opening (re-arms after
                                                            //  the prediction drops or a call runs)

// ---------- Fallback / degraded modes ----------
constexpr float    kFallbackHeatSetpointC   = 18.0f;  // MQTT stale >30 min: dual-bounded, last user mode,
constexpr float    kFallbackCoolSetpointC   = 27.0f;  //  never escalate OFF (27-28 acceptable)
constexpr uint32_t kMqttStaleS              = 1800;
constexpr float    kDegradedHeatFloorC      = 16.0f;  // DS18B20-only degraded mode: heat-to floor 16-18,
constexpr float    kDegradedHeatCeilC       = 18.0f;  //  cooling disabled (or >=29 C ceiling), demand capped
constexpr float    kCoolingIndoorLockoutC   = 18.0f;  // never cool when indoor below this

}  // namespace dettson
