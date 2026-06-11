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
