// DemandShaper.h — one interface, four implementations (docs/05 module layout).
//
// Converts an upstream capacity request (0-100 %) into a demand that is legal
// for the selected actuator:
//   GasShaper        — Chinook modulating gas: output 0 or [floor..100] with
//                      floor-snap hysteresis (never dither below the 40% floor,
//                      docs/05 canonical table) + max-continuous-runtime trip
//                      + gas min-on/min-off timers (docs/07 G14; comfort vs
//                      safety stop split mirrors CompressorGuard).
//   HpInverterShaper — inverter HP % path: slew-limited, quantized, floored.
//   HpRelayShaper    — staged HP via relay: demand -> on/off duty at
//                      <= max starts/hour, gated by a CompressorGuard-like
//                      callback (docs/04 §1a: our timers are primary).
//   StagedCoolShaper — single-stage cooling on the CT-485 (issue #140):
//                      request is a RUNTIME FRACTION, realized as slow on/off
//                      cycling at the stage's fixed engage demand with hard
//                      cycle hygiene (min-run, min-off, starts cap) enforced
//                      at the demand level; same gate contract as the relay
//                      shaper (CompressorGuard hardware timers stay downstream).
//
// Safety semantics (docs/04 §2, binding): boot state = no demand; a
// non-finite or negative request is invalid -> demand 0 + alarm flag.
// Pure C++17, no Arduino; time injected as uint32_t nowS (monotonic seconds).

#pragma once
#include <cstdint>
#include "DettsonConfig.h"

namespace dettson {

class DemandShaper {
 public:
  virtual ~DemandShaper() = default;
  // requestPct: desired capacity 0-100 (values >100 clamp; non-finite/negative
  // are invalid -> 0 + alarm). Returns the commanded percent, already legal
  // for the target equipment. Call at the control-loop rate.
  virtual float shape(float requestPct, uint32_t nowS) = 0;
  virtual void reset() = 0;        // back to boot state: no demand, alarms kept
  virtual bool alarm() const = 0;
};

// ---------------------------------------------------------------------------
class GasShaper : public DemandShaper {
 public:
  struct Config {
    float    floorPct            = kGasFloorPct;     // valid demand: 0 or [floor..100]
    // Floor-snap hysteresis band (docs/05: "snap to 0/minFire with hysteresis
    // — never dither at the floor"). Local shaping params, not canonical.
    float    lightMarginPct      = 2.0f;   // light only when request >= floor + margin
    float    extinguishMarginPct = 5.0f;   // extinguish only when request <= floor - margin
    uint32_t maxRuntimeS         = kGasMaxRuntimeS;  // continuous run cap -> drop + alarm
    // Gas-side anti-short-cycle pair (docs/07 G14, docs/05 table). Stop
    // semantics mirror CompressorGuard's comfort/safety distinction:
    //   - min-on gates COMFORT stops only (request dropping out via shape()):
    //     while unserved the burner is held lit at minFire (floorPct).
    //   - SAFETY stops — invalid input, max-runtime trip, forceStop() — are
    //     ALWAYS immediate (docs/04 §1: shedding demand is never blocked).
    //   - every (re)light waits for min-off since the last extinguish.
    uint32_t minOnS              = kGasMinOnS;
    uint32_t minOffS             = kGasMinOffS;
  };

  GasShaper() {}
  explicit GasShaper(const Config& cfg) : cfg_(cfg) {}

  float shape(float requestPct, uint32_t nowS) override;
  void reset() override;
  bool alarm() const override { return inputAlarm_ || runtimeAlarm_; }

  // Boot conservatism (CompressorGuard's unknown -> hold-off philosophy with
  // the gas-appropriate timer): without this call the off-timer starts fresh
  // at the first shape() call. Pass minOffServed=true only when persisted
  // state proves the burner has already been off >= minOffS.
  void bootRestore(uint32_t nowS, bool minOffServed);

  // SAFETY stop (sensor fault, invariant trip, watchdog path): extinguishes
  // immediately — min-on never applies — and (re)starts the min-off timer,
  // so a persisting fault also blocks relight.
  void forceStop(uint32_t nowS);

  bool lit() const { return lit_; }
  bool runtimeAlarm() const { return runtimeAlarm_; }  // latched until clearAlarms()
  bool inputAlarm() const { return inputAlarm_; }      // latched until clearAlarms()
  void clearAlarms() { inputAlarm_ = false; runtimeAlarm_ = false; }
  void setConfig(const Config& cfg) { cfg_ = cfg; }

 private:
  void extinguish(uint32_t nowS);            // any stop lands here: starts min-off
  bool minOffServed(uint32_t nowS) const;

  Config   cfg_;
  bool     lit_            = false;
  bool     runtimeTripped_ = false;  // forces 0 until the call ends (request drops out)
  bool     runtimeAlarm_   = false;
  bool     inputAlarm_     = false;
  uint32_t runStartS_      = 0;
  bool     offAnchored_    = false;  // false until boot anchors the off-timer
  uint32_t offSinceS_      = 0;
  bool     minOffWaived_   = false;  // bootRestore(minOffServed=true); cleared on extinguish
};

// ---------------------------------------------------------------------------
class HpInverterShaper : public DemandShaper {
 public:
  struct Config {
    float slewPctPerMin = kHpSlewPctPerMin;
    float stepPct       = kHpStepPct;
    float floorPct      = kHpFloorPct;  // nonzero output is in [floor..100]; 0 allowed
  };

  HpInverterShaper() {}
  explicit HpInverterShaper(const Config& cfg) : cfg_(cfg) {}

  // On/off cycling decisions belong upstream (arbiter/guard); a nonzero
  // request clamps into [floor..100]. Demand removal is immediate (fail to
  // no-demand, docs/04 §1); upward movement is slew-limited then quantized.
  float shape(float requestPct, uint32_t nowS) override;
  void reset() override;
  bool alarm() const override { return inputAlarm_; }

  bool inputAlarm() const { return inputAlarm_; }
  void clearAlarms() { inputAlarm_ = false; }
  void setConfig(const Config& cfg) { cfg_ = cfg; }

 private:
  float quantized() const;

  Config   cfg_;
  float    rawPct_  = 0.0f;  // pre-quantization state so small dt steps accumulate
  uint32_t lastS_   = 0;
  bool     hasLast_ = false;
  bool     inputAlarm_ = false;
};

// ---------------------------------------------------------------------------
// Minimal start/stop gate so the shaper stays decoupled from CompressorGuard's
// concrete API; firmware adapts guard.requestStart()/requestStop() to this.
class CompressorGate {
 public:
  virtual ~CompressorGate() = default;
  virtual bool canStart(uint32_t nowS) = 0;
  virtual bool canStop(uint32_t nowS) = 0;  // false while min-on unsatisfied
};

class HpRelayShaper : public DemandShaper {
 public:
  struct Config {
    uint8_t maxStartsPerH = kCompressorMaxStartsPerH;  // clamped to history depth
  };
  static constexpr uint8_t kMaxStartHistory = 16;

  explicit HpRelayShaper(CompressorGate& gate) : HpRelayShaper(gate, Config{}) {}
  HpRelayShaper(CompressorGate& gate, const Config& cfg);

  // Maps request % to an on/off duty cycle over period 3600/maxStartsPerH s,
  // so steady cycling can never exceed the starts budget; an own start-history
  // window enforces the cap even against a permissive gate. Output: 0 or 100.
  // request >= 100 runs continuously (docs/05: never cycle the HP as a
  // failsafe). Transitions — including the drop on request 0 / invalid input —
  // always consult the gate (docs/04 §1: never violate compressor timers on
  // the way down or back up).
  float shape(float requestPct, uint32_t nowS) override;
  void reset() override;  // clears phase state, keeps start history + alarms
  bool alarm() const override { return inputAlarm_; }

  bool on() const { return on_; }
  bool inputAlarm() const { return inputAlarm_; }
  void clearAlarms() { inputAlarm_ = false; }
  uint8_t startsInLastHour(uint32_t nowS) const;

 private:
  bool startBudgetOk(uint32_t nowS) const;
  void recordStart(uint32_t nowS);

  CompressorGate& gate_;
  Config   cfg_;
  bool     on_          = false;
  bool     everCycled_  = false;  // first start needs no off-phase wait (gate owns boot hold-off)
  uint32_t phaseStartS_ = 0;
  bool     inputAlarm_  = false;
  uint32_t startTimesS_[kMaxStartHistory] = {};
  uint8_t  startCount_  = 0;  // valid entries, oldest overwritten ring-style
  uint8_t  startHead_   = 0;
};

// ---------------------------------------------------------------------------
// Single-stage cooling as long-low modulation (issue #140, docs/13 §4).
//
// Equipment truth (field-confirmed): this furnace's cooling is one stage,
// engaged at CT-485 demand kCoolStagePct (30%) — the only value the OEM stat
// ever sends. A capacity PID is meaningless here; what CAN be modulated is
// RUNTIME. shape()'s request is therefore a runtime-duty request (0-100 % of
// time running), realized as slow on/off cycling at stagePct:
//
//   period P(d) = max(cyclePeriodS, minOnS/d, minOffS/(1-d))
//   on for d*P, off for (1-d)*P     (d >= 1: continuous — never cycle off)
//
// so every run is >= minOnS, every rest >= minOffS, and steady cycling can
// never exceed the starts budget (cyclePeriodS is clamped to at least
// 3600/maxStartsPerH; an own start-history ring enforces the cap even
// against a permissive gate, mirroring HpRelayShaper). requestFromError()
// is the matching proportional-band map from degrees-above-cool-setpoint to
// duty (derivation from the 2026-07-09/10 field data in the .cpp).
//
// Stop semantics (docs/04 §1/§2, GasShaper's comfort/safety split):
//   - a VALID request drop (call ended) is a comfort stop: held until minOnS
//     has been served — long-cycle hygiene is the whole point of this shaper;
//   - invalid input raises the alarm and drops without the demand-level hold
//     (the gate is still consulted — compressor timers are never violated,
//     on the way down or back up).
class StagedCoolShaper : public DemandShaper {
 public:
  struct Config {
    float    stagePct      = kCoolStagePct;       // fixed engage demand (equipment truth)
    uint8_t  maxStartsPerH = kCoolMaxStartsPerH;  // hard cap; clamped to history depth
    uint32_t minOnS        = kCoolMinOnS;         // min run, enforced at the DEMAND level
    uint32_t minOffS       = kCoolMinOffS;        // min rest, ditto
    uint32_t cyclePeriodS  = kCoolCyclePeriodS;   // base duty period (>= 3600/maxStartsPerH)
    float    fullDutyErrC  = kCoolFullDutyErrC;   // proportional band top (requestFromError)
    // Manual-arm expiry: a user-initiated setpoint/mode/preset change arms a
    // one-shot (armManual) that lets the NEXT start skip the demand-level
    // min-OFF rest. Beyond this window the arm is ignored, so a stale manual
    // intent can never bypass an unrelated later AUTOMATIC start. Mirrors the
    // CompressorGuard manual window (main_thermostat kManualStartArmWindowS).
    uint32_t manualArmWindowS = kCoolManualArmWindowS;
  };
  static constexpr uint8_t kMaxStartHistory = 16;

  explicit StagedCoolShaper(CompressorGate& gate) : StagedCoolShaper(gate, Config{}) {}
  StagedCoolShaper(CompressorGate& gate, const Config& cfg);

  // Proportional band: degrees above the cool setpoint -> duty request %.
  // errC <= 0 or NaN -> 0; saturates to 100 at fullDutyErrC (see .cpp).
  float requestFromError(float errC) const;
  // #141 seam: same band with a crossing-prediction bias added to the error
  // (RecoveryEstimator::crossingBias, docs/13 §2). The bias is advisory and
  // ADDITIVE only — NaN/negative bias is clamped to 0, so it can raise a
  // request early but never suppress the plain-error request. This is what
  // lets a predicted crossing start a gentle ramp while the error is still
  // small (or slightly negative), instead of the deadband slam.
  float requestFromError(float errC, float predBiasC) const;

  float shape(float requestPct, uint32_t nowS) override;  // output: 0 or stagePct
  void reset() override;  // clears phase state + manual arm, keeps start history + alarms
  bool alarm() const override { return inputAlarm_; }

  // Manual-start bypass (issue #151, mirrors CompressorGuard's manual path).
  // A user-initiated setpoint/mode/preset change arms this one-shot; the next
  // start consumes it and skips the demand-level min-OFF rest ONLY, so a
  // deliberate human cool request isn't held for minOffS (up to 8 min). The
  // AUTOMATIC control loop is unaffected — its min-OFF (long-low modulation,
  // dehumidification; docs/13 §4/§8) stands. Every other protection is
  // preserved even for a manual start: min-ON still holds the resulting run,
  // the starts/hour cap still applies (this is what bounds any restart-abuse
  // pattern, so the ODU can't be walked into a latched fault), and the
  // CompressorGuard gate is still consulted downstream. The arm expires after
  // cfg.manualArmWindowS and is cleared by reset()/safety stop.
  void armManual(uint32_t nowS) { manualArmed_ = true; manualArmS_ = nowS; }

  bool on() const { return on_; }
  bool inputAlarm() const { return inputAlarm_; }
  void clearAlarms() { inputAlarm_ = false; }
  uint8_t startsInLastHour(uint32_t nowS) const;
  const Config& config() const { return cfg_; }

  // Seconds until THIS shaper's demand-level min-OFF rest is served — i.e. how
  // long a wanted restart is still blocked by the cool-side min-OFF (up to
  // minOffS, 480 s in the field config). Powers the on-panel "Cooling soon"
  // acknowledgement (owner report: min-OFF must not read as plain "Idle").
  // Returns 0 while running, before the first cycle, while a manual bypass
  // (armManual) is still armed, or once the rest is served. Surfaces the
  // existing timer state (phaseStartS_) — it does NOT recompute independently;
  // mirrors GasShaper::minOffServed's use of the same elapsed-since-off clock.
  uint32_t minOffRemainingS(uint32_t nowS) const;

 private:
  float periodS(float duty) const;  // P(d) above; duty must be in (0,1)
  bool startBudgetOk(uint32_t nowS) const;
  void recordStart(uint32_t nowS);

  CompressorGate& gate_;
  Config   cfg_;
  bool     on_          = false;
  bool     everCycled_  = false;  // first start needs no off-phase wait (gate owns boot hold-off)
  uint32_t phaseStartS_ = 0;
  bool     inputAlarm_  = false;
  bool     manualArmed_ = false;  // one-shot: next start skips the demand-level min-OFF
  uint32_t manualArmS_  = 0;      // when armed (armManual); expires after cfg.manualArmWindowS
  uint32_t startTimesS_[kMaxStartHistory] = {};
  uint8_t  startCount_  = 0;  // valid entries, oldest overwritten ring-style
  uint8_t  startHead_   = 0;
};

}  // namespace dettson
