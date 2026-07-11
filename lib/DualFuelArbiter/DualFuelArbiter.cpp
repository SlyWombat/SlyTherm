#include "DualFuelArbiter.h"

#include <cmath>

namespace dettson {

// Compile-time sanity on the canonical defaults themselves.
static_assert(kCompressorMinOatC <= kAuxMaxOatC,
              "default lockouts leave an OAT band with no heat source");
static_assert(kCopCurvePoints >= 2, "COP curve needs at least one segment");

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
  // #143 economics fields are validated UNCONDITIONALLY (not only when
  // economicEnabled): a config carrying junk prices/curve is a junk config,
  // and enabling economics later must never re-discover it at runtime.
  if (!std::isfinite(cfg.elecPricePerKwh) || cfg.elecPricePerKwh <= 0.0f ||
      cfg.elecPricePerKwh > kEnergyPriceMax)
    return false;
  if (!std::isfinite(cfg.gasPricePerM3) || cfg.gasPricePerM3 <= 0.0f ||
      cfg.gasPricePerM3 > kEnergyPriceMax)
    return false;
  if (!std::isfinite(cfg.afue) || cfg.afue < kAfueMin || cfg.afue > 1.0f)
    return false;
  for (size_t i = 0; i < kCopCurvePoints; ++i) {
    const CopPoint& p = cfg.copCurve[i];
    if (!std::isfinite(p.oatC) || !std::isfinite(p.cop) || p.cop <= 0.0f)
      return false;
    if (i > 0 && (p.oatC <= cfg.copCurve[i - 1].oatC ||   // OAT strictly up
                  p.cop < cfg.copCurve[i - 1].cop))       // COP non-decreasing
      return false;
  }
  return true;
}

float DualFuelArbiter::breakEvenCop(float elecPerKwh, float gasPerM3,
                                    float afue) {
  // gas $/kWh-thermal = gasPerM3 / (kGasKwhPerM3 * afue);
  // HP  $/kWh-thermal = elecPerKwh / COP  ==>  equal at COP*:
  return elecPerKwh * kGasKwhPerM3 * afue / gasPerM3;
}

float DualFuelArbiter::copAtOat(float oatC) const {
  const CopPoint* c = cfg_.copCurve;
  if (oatC <= c[0].oatC) return c[0].cop;  // flat extrapolation at the ends
  if (oatC >= c[kCopCurvePoints - 1].oatC) return c[kCopCurvePoints - 1].cop;
  for (size_t i = 1; i < kCopCurvePoints; ++i) {
    if (oatC <= c[i].oatC) {
      const float t = (oatC - c[i - 1].oatC) / (c[i].oatC - c[i - 1].oatC);
      return c[i - 1].cop + t * (c[i].cop - c[i - 1].cop);
    }
  }
  return c[kCopCurvePoints - 1].cop;  // unreachable
}

float DualFuelArbiter::economicBalancePointC() const {
  const CopPoint* c = cfg_.copCurve;
  const float copStar = breakEvenCop();
  float oat;
  if (copStar <= c[0].cop) {
    // HP economic across the whole curve: switchover falls to the floor.
    oat = c[0].oatC - 1.0f;  // below every point; the clamp does the rest
  } else if (copStar > c[kCopCurvePoints - 1].cop) {
    // HP never economic: switchover rises to the ceiling (gas lockout).
    oat = c[kCopCurvePoints - 1].oatC + 100.0f;
  } else {
    oat = c[kCopCurvePoints - 1].oatC;
    for (size_t i = 1; i < kCopCurvePoints; ++i) {
      if (copStar <= c[i].cop) {
        // c[i-1].cop < copStar <= c[i].cop, so the segment rises strictly.
        const float t = (copStar - c[i - 1].cop) / (c[i].cop - c[i - 1].cop);
        oat = c[i - 1].oatC + t * (c[i].oatC - c[i - 1].oatC);
        break;
      }
    }
  }
  // Clamp inside the thermally safe band (docs/13 §1: economics only ever
  // moves switchover WITHIN it): capacity floor .. gas lockout ceiling.
  if (oat < cfg_.balancePointC) oat = cfg_.balancePointC;
  if (oat > cfg_.auxMaxOatC) oat = cfg_.auxMaxOatC;
  return oat;
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
  // #143: the balance point may be the COMPUTED economic one (COP crossing,
  // clamped inside the safe band) — same latch, same hysteresis either way.
  const float balC = effectiveBalancePointC();
  if (in.oatValid) {
    if (in.oatC < balC) {
      gasPreferred_ = true;
    } else if (in.oatC > balC + cfg_.balanceHystC) {
      gasPreferred_ = false;
    }
  }

  // De-escalation gate: dwell elapsed AND OAT above balance + hysteresis
  // (docs/05 defaults table). Evaluated even between calls so a struggling
  // HP is not retried early just because the call cycled.
  if (escalated_ && in.oatValid &&
      (nowS - escalatedAtS_) >= cfg_.deescalationMinS &&
      in.oatC > balC + cfg_.balanceHystC) {
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
