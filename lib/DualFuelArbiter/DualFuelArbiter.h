// DualFuelArbiter.h — heat-source selection for the dual-fuel pipeline
// (issue #32; docs/05 Phase 4 pipeline, docs/04 §2 mutual exclusion).
//
// Given the effective heat call, OAT (+validity), current HP demand and
// injected time, picks exactly ONE heat source. The output enum is
// structurally incapable of "gas + compressor" — defrost tempering, the sole
// sanctioned overlap, is emitted as a SEPARATE temper request that the caller
// routes to DemandArbiter's defrost channel, never the main heat channel.
//
// Policy (docs/05 defaults table, docs/04 §4):
//   - balance point with hysteresis: below -> gas preferred;
//   - #143 economic switchover (docs/13 §1, default OFF): the balance point
//     becomes COMPUTED — the OAT where COP(OAT) crosses the break-even
//     COP* = elec$/kWh × kGasKwhPerM3 × AFUE ÷ gas$/m3 — clamped inside
//     [balancePointC, auxMaxOatC]. Economics only ever moves switchover
//     WITHIN the thermally safe band: balancePointC keeps its role as the
//     capacity floor (below it the HP cannot carry the load regardless of
//     price) and both hard lockouts stay untouched downstream;
//   - low-OAT compressor lockout, high-OAT aux/gas lockout;
//   - config validation (hard rule): reject lockouts leaving any OAT band
//     with no permitted heat source;
//   - OAT invalid -> fail cold: gas allowed, compressor locked out;
//   - escalation: droop + saturated HP demand sustained -> stage to gas;
//     de-escalate only after a dwell AND OAT above balance + hysteresis.
//
// Pure C++17, no Arduino. Time is injected as uint32_t nowS (monotonic).

#pragma once
#include <cstdint>
#include "DettsonConfig.h"

namespace dettson {

enum class HeatSource : uint8_t { kNone, kHeatPump, kGas };

struct DualFuelConfig {
  float    balancePointC       = kBalancePointC;
  float    balanceHystC        = kBalancePointHystC;
  float    compressorMinOatC   = kCompressorMinOatC;  // HP locked out below
  float    auxMaxOatC          = kAuxMaxOatC;         // gas locked out above
  float    escalationDroopC    = kEscalationDroopC;
  uint32_t escalationMinS      = kEscalationMinS;
  float    escalationHpDemandPct = kEscalationHpDemandPct;
  uint32_t deescalationMinS    = kDeescalationMinS;
  float    defrostTemperHeatPct = kDefrostTemperHeatPct;
  uint32_t defrostTemperMaxS   = kDefrostTemperMaxS;  // hard-capped at the constant

  // #143 economic switchover (docs/13 §1). OFF by default — winter task;
  // when off, balancePointC alone decides (today's behavior, bit-for-bit).
  // When on, balancePointC is reinterpreted as the CAPACITY balance point:
  // the hard floor economics can never move switchover below.
  bool     economicEnabled  = kDualFuelEconomicEnabledDefault;
  float    elecPricePerKwh  = kElecPricePerKwhDefault;  // $/kWh, all-in marginal
  float    gasPricePerM3    = kGasPricePerM3Default;    // $/m3, all-in marginal
  float    afue             = kAfueDefault;
  // COP(OAT) piecewise-linear curve: oatC strictly increasing, cop finite,
  // positive and non-decreasing (configValid enforces). Seed values are
  // nameplate-shaped placeholders (see DettsonConfig.h) pending the installed
  // unit's data + the #143 CopLearner field record.
  CopPoint copCurve[kCopCurvePoints] = {
      kCopCurveSeed[0], kCopCurveSeed[1], kCopCurveSeed[2],
      kCopCurveSeed[3], kCopCurveSeed[4]};
};

struct DualFuelInputs {
  bool  heatCall      = false;  // effective call from ModeStateMachine
  float setpointC     = 0.0f;   // active heat setpoint (droop reference)
  float roomTempC     = 0.0f;   // fused room temp
  bool  roomTempValid = false;
  float oatC          = 0.0f;
  bool  oatValid      = false;  // from OutdoorTempSource (all rungs stale -> false)
  float hpDemandPct   = 0.0f;   // HP demand currently being run (0-100)
  bool  defrostActive = false;  // bus defrost signature or D-wire sense, injected
};

struct DualFuelOutput {
  HeatSource source        = HeatSource::kNone;
  bool       escalated     = false;
  bool       temperRequest = false;  // defrost channel ONLY (docs/04 §2)
  float      temperHeatPct = 0.0f;
  bool       oatInvalidAlarm        = false;  // fail-cold in effect
  bool       configRejectedAlarm    = false;  // last config offered was invalid
  bool       noSourcePermittedAlarm = false;  // defensive; unreachable with a
                                              // validated config — picked gas
};

class DualFuelArbiter {
 public:
  DualFuelArbiter() = default;
  // Invalid cfg -> validated defaults are used instead and the rejected
  // alarm is latched until a valid setConfig() (docs/04 §4 hard rule).
  explicit DualFuelArbiter(const DualFuelConfig& cfg);

  // Hard rule (docs/04 §4): no OAT band may be left with no permitted heat
  // source, i.e. compressorMinOatC must not exceed auxMaxOatC.
  static bool configValid(const DualFuelConfig& cfg);

  // Rejects (returns false, keeps current config, flags alarm) when invalid.
  bool setConfig(const DualFuelConfig& cfg);
  const DualFuelConfig& config() const { return cfg_; }

  // ---- #143 economics (pure arithmetic, unit-tested; docs/13 §1) ----
  // Break-even COP*: the HP is cheaper than gas exactly when COP(OAT) > COP*.
  // COP* = elecPerKwh × kGasKwhPerM3 × afue ÷ gasPerM3 (per-m3 form of the
  // docs/13 per-therm arithmetic; equivalent via 1 therm = kGasM3PerTherm m3).
  static float breakEvenCop(float elecPerKwh, float gasPerM3, float afue);
  float breakEvenCop() const {
    return breakEvenCop(cfg_.elecPricePerKwh, cfg_.gasPricePerM3, cfg_.afue);
  }
  // Piecewise-linear COP(OAT) from cfg_.copCurve; flat beyond the end points.
  float copAtOat(float oatC) const;
  // OAT where COP(OAT) crosses COP*, CLAMPED to the thermally safe band
  // [balancePointC, auxMaxOatC] — economics never moves switchover below the
  // capacity floor or above the gas lockout. COP* below the whole curve
  // (cheap electricity) clamps low; above it (cheap gas) clamps high.
  float economicBalancePointC() const;
  // The balance point step() actually uses: economic when enabled, else the
  // fixed configured one — the ONE seam the #143 economics enters through.
  float effectiveBalancePointC() const {
    return cfg_.economicEnabled ? economicBalancePointC() : cfg_.balancePointC;
  }

  // Call once per control tick with monotonic nowS.
  DualFuelOutput step(const DualFuelInputs& in, uint32_t nowS);

 private:
  DualFuelConfig cfg_;
  bool configRejected_ = false;

  // Boot state = no latched preference for gas, nothing escalated, no
  // defrost in progress (docs/04 §3 boot = no demand).
  bool     gasPreferred_      = false;
  bool     escalated_         = false;
  uint32_t escalatedAtS_      = 0;
  bool     droopTiming_       = false;
  uint32_t droopStartS_       = 0;
  bool     defrostPrev_       = false;
  uint32_t defrostStartS_     = 0;
};

}  // namespace dettson
