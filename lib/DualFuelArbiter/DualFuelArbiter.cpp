#include "DualFuelArbiter.h"

namespace dettson {

// Compile-time sanity on the canonical defaults themselves.
static_assert(kCompressorMinOatC <= kAuxMaxOatC,
              "default lockouts leave an OAT band with no heat source");

DualFuelArbiter::DualFuelArbiter(const DualFuelConfig& cfg) {
  if (configValid(cfg)) {
    cfg_ = cfg;
  } else {
    configRejected_ = true;  // cfg_ stays at validated defaults
  }
}

bool DualFuelArbiter::configValid(const DualFuelConfig& cfg) {
  // Hard rule (docs/04 §4): compressorMinOat > auxMaxOat would leave the
  // band (auxMaxOat, compressorMinOat) with NO permitted heat source.
  if (cfg.compressorMinOatC > cfg.auxMaxOatC) return false;
  if (cfg.balanceHystC < 0.0f) return false;
  if (cfg.escalationDroopC <= 0.0f) return false;
  if (cfg.escalationHpDemandPct <= 0.0f || cfg.escalationHpDemandPct > 100.0f)
    return false;
  if (cfg.defrostTemperHeatPct < 0.0f || cfg.defrostTemperHeatPct > 100.0f)
    return false;
  return true;
}

bool DualFuelArbiter::setConfig(const DualFuelConfig& cfg) {
  if (!configValid(cfg)) {
    configRejected_ = true;
    return false;
  }
  cfg_ = cfg;
  configRejected_ = false;
  return true;
}

DualFuelOutput DualFuelArbiter::step(const DualFuelInputs& in, uint32_t nowS) {
  DualFuelOutput out;
  out.configRejectedAlarm = configRejected_;
  out.oatInvalidAlarm = !in.oatValid;

  // Balance-point preference latch with hysteresis: gas below the balance
  // point, HP only once OAT recovers above balance + hyst; hold in between.
  if (in.oatValid) {
    if (in.oatC < cfg_.balancePointC) {
      gasPreferred_ = true;
    } else if (in.oatC > cfg_.balancePointC + cfg_.balanceHystC) {
      gasPreferred_ = false;
    }
  }

  // De-escalation gate: dwell elapsed AND OAT above balance + hysteresis
  // (docs/05 defaults table). Evaluated even between calls so a struggling
  // HP is not retried early just because the call cycled.
  if (escalated_ && in.oatValid &&
      (nowS - escalatedAtS_) >= cfg_.deescalationMinS &&
      in.oatC > cfg_.balancePointC + cfg_.balanceHystC) {
    escalated_ = false;
  }

  // Defrost tempering: separate channel, fixed demand, hard time cap
  // (docs/05: 35% fixed never PID, 15 min cap). The cap here is a backstop
  // even if the runtime-tuned value exceeds the canonical constant.
  if (in.defrostActive) {
    if (!defrostPrev_) defrostStartS_ = nowS;
    uint32_t cap = cfg_.defrostTemperMaxS < kDefrostTemperMaxS
                       ? cfg_.defrostTemperMaxS
                       : kDefrostTemperMaxS;
    if (nowS - defrostStartS_ < cap) {
      out.temperRequest = true;
      out.temperHeatPct = cfg_.defrostTemperHeatPct;
    }
  }
  defrostPrev_ = in.defrostActive;

  if (!in.heatCall) {
    droopTiming_ = false;  // droop accumulation does not span calls
    out.escalated = escalated_;
    return out;  // source = kNone: no call -> no demand
  }

  if (!in.oatValid) {
    // Fail cold (docs/04 §4): never run the compressor on an unknown
    // outdoor temperature; gas remains allowed.
    droopTiming_ = false;
    out.source = HeatSource::kGas;
    out.escalated = escalated_;
    return out;
  }

  const bool hpAllowed = in.oatC >= cfg_.compressorMinOatC;
  const bool gasAllowed = in.oatC <= cfg_.auxMaxOatC;

  HeatSource src;
  if (!hpAllowed && !gasAllowed) {
    // Unreachable with a validated config; loss of heat is itself a hazard
    // (docs/04 §4), so pick gas (IFC keeps every combustion safety) + alarm.
    out.noSourcePermittedAlarm = true;
    src = HeatSource::kGas;
  } else if (!hpAllowed) {
    src = HeatSource::kGas;
  } else if (!gasAllowed) {
    src = HeatSource::kHeatPump;
  } else {
    src = gasPreferred_ ? HeatSource::kGas : HeatSource::kHeatPump;
  }

  // Escalation: droop below setpoint while HP demand is saturated, sustained
  // for escalationMinS -> stage to gas. Any break in the condition resets
  // the timer; an invalid room temp can never escalate.
  if (src == HeatSource::kHeatPump && !escalated_) {
    const bool drooping = in.roomTempValid &&
                          (in.setpointC - in.roomTempC) > cfg_.escalationDroopC &&
                          in.hpDemandPct >= cfg_.escalationHpDemandPct;
    if (drooping) {
      if (!droopTiming_) {
        droopTiming_ = true;
        droopStartS_ = nowS;
      }
      if (nowS - droopStartS_ >= cfg_.escalationMinS) {
        escalated_ = true;
        escalatedAtS_ = nowS;
      }
    } else {
      droopTiming_ = false;
    }
  } else {
    droopTiming_ = false;
  }

  if (escalated_ && src == HeatSource::kHeatPump && gasAllowed) {
    src = HeatSource::kGas;
  }

  out.source = src;
  out.escalated = escalated_;
  return out;
}

}  // namespace dettson
