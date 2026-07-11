// DualFuelArbiter tests (issue #32): balance-point hysteresis (no chatter),
// OAT lockout bands, invalid-config rejection, escalation timing and
// de-escalation gating, OAT-invalid fail-cold, defrost temper request shape
// + cap, mutual exclusion / boot state.
// #143 additions: break-even COP* arithmetic (docs/13 §1 worked example),
// COP(OAT) interpolation, economic balance point clamped inside the safe
// band, hard floors never violated with economics ON, price/curve validation,
// and OFF-by-default equivalence with the fixed balance point.
#include <unity.h>
#include <cmath>
#include "DualFuelArbiter.h"
#include "DettsonConfig.h"

using namespace dettson;

void setUp() {}
void tearDown() {}

// Comfortable HP-territory call: OAT above balance + hyst, no droop.
static DualFuelInputs hpCall(float oatC) {
  DualFuelInputs in;
  in.heatCall = true;
  in.setpointC = 21.0f;
  in.roomTempC = 20.8f;
  in.roomTempValid = true;
  in.oatC = oatC;
  in.oatValid = true;
  in.hpDemandPct = 60.0f;
  return in;
}

static void test_boot_no_call_no_demand() {
  DualFuelArbiter arb;
  DualFuelInputs in;  // everything invalid/off: boot state
  DualFuelOutput out = arb.step(in, 0);
  TEST_ASSERT_TRUE(out.source == HeatSource::kNone);
  TEST_ASSERT_FALSE(out.temperRequest);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.temperHeatPct);
  TEST_ASSERT_FALSE(out.escalated);
  TEST_ASSERT_FALSE(out.configRejectedAlarm);
}

static void test_balance_point_hysteresis_no_chatter() {
  DualFuelArbiter arb;
  uint32_t t = 0;
  // Below balance point (-8): gas.
  TEST_ASSERT_TRUE(arb.step(hpCall(-10.0f), t += 10).source == HeatSource::kGas);
  // Rises into the hysteresis band (-8..-6): still gas — no chatter.
  TEST_ASSERT_TRUE(arb.step(hpCall(-7.5f), t += 10).source == HeatSource::kGas);
  TEST_ASSERT_TRUE(arb.step(hpCall(-6.5f), t += 10).source == HeatSource::kGas);
  // Clears balance + hyst (-6): HP.
  TEST_ASSERT_TRUE(arb.step(hpCall(-5.9f), t += 10).source == HeatSource::kHeatPump);
  // Dips back into the band: still HP — no chatter.
  TEST_ASSERT_TRUE(arb.step(hpCall(-7.0f), t += 10).source == HeatSource::kHeatPump);
  // Drops below balance: gas again.
  TEST_ASSERT_TRUE(arb.step(hpCall(-8.5f), t += 10).source == HeatSource::kGas);
}

static void test_lockout_low_oat_forces_gas() {
  // Custom config where HP would be PREFERRED at the test OAT, but the
  // compressor low-OAT lockout overrides the preference.
  DualFuelConfig cfg;
  cfg.balancePointC = -25.0f;
  cfg.compressorMinOatC = -20.0f;
  DualFuelArbiter arb(cfg);
  DualFuelOutput out = arb.step(hpCall(-22.0f), 10);  // > balance, < lockout
  TEST_ASSERT_TRUE(out.source == HeatSource::kGas);
  TEST_ASSERT_FALSE(out.noSourcePermittedAlarm);
}

static void test_lockout_high_oat_forces_hp() {
  // Gas would be preferred (below balance) but the aux/gas high-OAT lockout
  // overrides it.
  DualFuelConfig cfg;
  cfg.balancePointC = 12.0f;  // within the documented -30..15 range
  cfg.auxMaxOatC = 10.0f;
  DualFuelArbiter arb(cfg);
  DualFuelOutput out = arb.step(hpCall(11.0f), 10);  // < balance, > aux max
  TEST_ASSERT_TRUE(out.source == HeatSource::kHeatPump);
}

static void test_invalid_config_rejected() {
  // compressorMinOat > auxMaxOat leaves (-10..-5) with no heat source:
  // hard-rule rejection (docs/04 §4).
  DualFuelConfig bad;
  bad.compressorMinOatC = -5.0f;
  bad.auxMaxOatC = -10.0f;
  TEST_ASSERT_FALSE(DualFuelArbiter::configValid(bad));

  DualFuelArbiter arb;
  TEST_ASSERT_FALSE(arb.setConfig(bad));
  // Old (default) config still in effect, alarm flagged.
  TEST_ASSERT_EQUAL_FLOAT(kCompressorMinOatC, arb.config().compressorMinOatC);
  DualFuelOutput out = arb.step(hpCall(-9.0f), 10);  // gap band of the bad cfg
  TEST_ASSERT_TRUE(out.configRejectedAlarm);
  TEST_ASSERT_TRUE(out.source == HeatSource::kGas);  // defaults: below balance
  TEST_ASSERT_FALSE(out.noSourcePermittedAlarm);

  // A valid config clears the alarm.
  TEST_ASSERT_TRUE(arb.setConfig(DualFuelConfig{}));
  TEST_ASSERT_FALSE(arb.step(hpCall(-7.0f), 20).configRejectedAlarm);
}

static void test_invalid_config_at_construction_falls_back_to_defaults() {
  DualFuelConfig bad;
  bad.compressorMinOatC = 5.0f;
  bad.auxMaxOatC = 0.0f;
  DualFuelArbiter arb(bad);
  TEST_ASSERT_EQUAL_FLOAT(kAuxMaxOatC, arb.config().auxMaxOatC);
  DualFuelOutput out = arb.step(hpCall(2.0f), 10);
  TEST_ASSERT_TRUE(out.configRejectedAlarm);
  TEST_ASSERT_TRUE(out.source == HeatSource::kHeatPump);  // defaults applied
}

static DualFuelInputs droopingHp(float oatC) {
  DualFuelInputs in = hpCall(oatC);
  in.roomTempC = in.setpointC - kEscalationDroopC - 0.5f;  // droop > threshold
  in.hpDemandPct = 100.0f;                                 // saturated
  return in;
}

static void test_escalation_requires_droop_demand_and_duration() {
  DualFuelArbiter arb;
  const float oat = -5.0f;  // above balance + hyst: HP territory
  uint32_t t0 = 1000;

  // Saturated + drooping, but not yet for kEscalationMinS: still HP.
  TEST_ASSERT_TRUE(arb.step(droopingHp(oat), t0).source == HeatSource::kHeatPump);
  DualFuelOutput out = arb.step(droopingHp(oat), t0 + kEscalationMinS - 1);
  TEST_ASSERT_TRUE(out.source == HeatSource::kHeatPump);
  TEST_ASSERT_FALSE(out.escalated);

  // Duration met: escalate to gas.
  out = arb.step(droopingHp(oat), t0 + kEscalationMinS);
  TEST_ASSERT_TRUE(out.escalated);
  TEST_ASSERT_TRUE(out.source == HeatSource::kGas);
}

static void test_escalation_timer_resets_on_condition_break() {
  DualFuelArbiter arb;
  const float oat = -5.0f;
  uint32_t t0 = 1000;
  arb.step(droopingHp(oat), t0);
  // Halfway through, demand sags below the saturation threshold one tick.
  DualFuelInputs relaxed = droopingHp(oat);
  relaxed.hpDemandPct = kEscalationHpDemandPct - 1.0f;
  arb.step(relaxed, t0 + kEscalationMinS / 2);
  // Droop resumes: the clock restarted, so the original deadline passes on HP.
  uint32_t t1 = t0 + kEscalationMinS / 2 + 1;
  arb.step(droopingHp(oat), t1);
  DualFuelOutput out = arb.step(droopingHp(oat), t0 + kEscalationMinS);
  TEST_ASSERT_FALSE(out.escalated);
  TEST_ASSERT_TRUE(out.source == HeatSource::kHeatPump);
  // ...and escalates kEscalationMinS after the RESTART.
  out = arb.step(droopingHp(oat), t1 + kEscalationMinS);
  TEST_ASSERT_TRUE(out.escalated);
}

static void test_no_escalation_below_demand_threshold_or_invalid_room() {
  DualFuelArbiter arb;
  const float oat = -5.0f;
  DualFuelInputs in = droopingHp(oat);
  in.hpDemandPct = 90.0f;  // < 95: HP not saturated
  arb.step(in, 0);
  TEST_ASSERT_FALSE(arb.step(in, 2 * kEscalationMinS).escalated);

  DualFuelArbiter arb2;
  DualFuelInputs in2 = droopingHp(oat);
  in2.roomTempValid = false;  // invalid input can never drive escalation
  arb2.step(in2, 0);
  TEST_ASSERT_FALSE(arb2.step(in2, 2 * kEscalationMinS).escalated);
}

static void test_deescalation_needs_dwell_and_oat() {
  DualFuelArbiter arb;
  const float oat = -5.0f;  // above balance + hyst throughout
  uint32_t t0 = 1000;
  arb.step(droopingHp(oat), t0);
  uint32_t tEsc = t0 + kEscalationMinS;
  TEST_ASSERT_TRUE(arb.step(droopingHp(oat), tEsc).escalated);

  // Gas is heating; droop recovers. Dwell not yet elapsed: stay on gas.
  DualFuelOutput out = arb.step(hpCall(oat), tEsc + kDeescalationMinS - 1);
  TEST_ASSERT_TRUE(out.escalated);
  TEST_ASSERT_TRUE(out.source == HeatSource::kGas);

  // Dwell elapsed AND OAT above balance + hyst: back to HP.
  out = arb.step(hpCall(oat), tEsc + kDeescalationMinS);
  TEST_ASSERT_FALSE(out.escalated);
  TEST_ASSERT_TRUE(out.source == HeatSource::kHeatPump);
}

static void test_deescalation_blocked_by_low_oat() {
  DualFuelArbiter arb;
  const float oat = -7.0f;  // inside hysteresis band: above balance,
                            // NOT above balance + hyst
  uint32_t t0 = 1000;
  arb.step(droopingHp(oat), t0);
  uint32_t tEsc = t0 + kEscalationMinS;
  TEST_ASSERT_TRUE(arb.step(droopingHp(oat), tEsc).escalated);

  // Dwell long since elapsed, but OAT gate not met: stay escalated on gas.
  DualFuelOutput out = arb.step(hpCall(oat), tEsc + 3 * kDeescalationMinS);
  TEST_ASSERT_TRUE(out.escalated);
  TEST_ASSERT_TRUE(out.source == HeatSource::kGas);
}

static void test_escalation_latch_survives_call_cycling() {
  // Dropping and re-raising the call must not bypass the de-escalation dwell.
  DualFuelArbiter arb;
  const float oat = -5.0f;
  arb.step(droopingHp(oat), 0);
  TEST_ASSERT_TRUE(arb.step(droopingHp(oat), kEscalationMinS).escalated);

  DualFuelInputs idle = hpCall(oat);
  idle.heatCall = false;
  arb.step(idle, kEscalationMinS + 60);
  // Call returns before the dwell: still gas.
  DualFuelOutput out = arb.step(hpCall(oat), kEscalationMinS + 120);
  TEST_ASSERT_TRUE(out.source == HeatSource::kGas);
  // The dwell can elapse while idle; with OAT good, next call runs the HP.
  arb.step(idle, kEscalationMinS + kDeescalationMinS);
  out = arb.step(hpCall(oat), kEscalationMinS + kDeescalationMinS + 60);
  TEST_ASSERT_TRUE(out.source == HeatSource::kHeatPump);
}

static void test_oat_invalid_fails_cold() {
  DualFuelArbiter arb;
  DualFuelInputs in = hpCall(5.0f);  // would be HP territory if believed...
  in.oatValid = false;               // ...but OAT is unknown
  DualFuelOutput out = arb.step(in, 10);
  TEST_ASSERT_TRUE(out.source == HeatSource::kGas);  // compressor locked out
  TEST_ASSERT_TRUE(out.oatInvalidAlarm);

  // OAT returns above balance + hyst: HP allowed again.
  out = arb.step(hpCall(5.0f), 20);
  TEST_ASSERT_TRUE(out.source == HeatSource::kHeatPump);
  TEST_ASSERT_FALSE(out.oatInvalidAlarm);
}

static void test_defrost_temper_shape_and_cap() {
  DualFuelArbiter arb;
  DualFuelInputs in = hpCall(-5.0f);
  in.defrostActive = true;
  uint32_t t0 = 500;

  DualFuelOutput out = arb.step(in, t0);
  TEST_ASSERT_TRUE(out.temperRequest);
  TEST_ASSERT_EQUAL_FLOAT(kDefrostTemperHeatPct, out.temperHeatPct);
  // Temper rides the SEPARATE defrost channel; the main source stays HP —
  // the sole sanctioned overlap (docs/04 §2).
  TEST_ASSERT_TRUE(out.source == HeatSource::kHeatPump);

  // Just inside the hard cap: still tempering.
  out = arb.step(in, t0 + kDefrostTemperMaxS - 1);
  TEST_ASSERT_TRUE(out.temperRequest);
  // Cap reached: temper request drops even though defrost is still asserted.
  out = arb.step(in, t0 + kDefrostTemperMaxS);
  TEST_ASSERT_FALSE(out.temperRequest);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, out.temperHeatPct);

  // Defrost ends, a fresh cycle re-arms the cap timer.
  in.defrostActive = false;
  arb.step(in, t0 + kDefrostTemperMaxS + 100);
  in.defrostActive = true;
  out = arb.step(in, t0 + kDefrostTemperMaxS + 200);
  TEST_ASSERT_TRUE(out.temperRequest);
}

static void test_temper_max_capped_at_canonical_constant() {
  DualFuelConfig cfg;
  cfg.defrostTemperMaxS = kDefrostTemperMaxS * 4;  // runtime mis-tune
  DualFuelArbiter arb(cfg);
  DualFuelInputs in = hpCall(-5.0f);
  in.defrostActive = true;
  arb.step(in, 0);
  // Hard cap (docs/05: 15 min) wins over the configured value.
  TEST_ASSERT_FALSE(arb.step(in, kDefrostTemperMaxS).temperRequest);
}

static void test_mutual_exclusion_single_source_always() {
  // Sweep a mixed scenario; the main output can never be "both" (single
  // enum), and gas selection always implies the HP channel is not selected.
  DualFuelArbiter arb;
  uint32_t t = 0;
  const float oats[] = {-25.0f, -10.0f, -7.0f, -5.0f, 0.0f, 11.0f, 15.0f};
  for (float oat : oats) {
    DualFuelInputs in = droopingHp(oat);
    in.defrostActive = true;
    DualFuelOutput out = arb.step(in, t += 600);
    TEST_ASSERT_TRUE(out.source == HeatSource::kGas ||
                     out.source == HeatSource::kHeatPump);
    if (out.temperRequest) {
      // Tempering never hijacks the main channel.
      TEST_ASSERT_EQUAL_FLOAT(kDefrostTemperHeatPct, out.temperHeatPct);
    }
  }
}

// ---------------------------------------------------------------------------
// #143 economic switchover (docs/13 §1)
// ---------------------------------------------------------------------------

static void test_break_even_cop_worked_example() {
  // docs/13 §1: COP* = elec$ × 29.3 × AFUE ÷ gas$/therm ≈ 2.7-2.8 at
  // $0.15/kWh, $1.50/therm, 0.95 AFUE. Our canonical unit is $/m3, so the
  // therm price converts via kGasM3PerTherm (energy content, DettsonConfig.h).
  const float gasPerM3 = 1.50f / kGasM3PerTherm;      // ≈ $0.540/m3
  const float copStar = DualFuelArbiter::breakEvenCop(0.15f, gasPerM3, 0.95f);
  TEST_ASSERT_TRUE(copStar > 2.7f && copStar < 2.85f);
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 2.784f, copStar);
  // Identity with the per-therm form: elec$ × kKwhPerTherm × AFUE ÷ gas$/therm.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.15f * kKwhPerTherm * 0.95f / 1.50f, copStar);
}

static void test_cop_curve_interpolation_and_flat_ends() {
  DualFuelArbiter arb;  // seed curve: (-30,1.4)..(8.3,3.6)
  // Flat extrapolation beyond the end points.
  TEST_ASSERT_EQUAL_FLOAT(kCopCurveSeed[0].cop, arb.copAtOat(-40.0f));
  TEST_ASSERT_EQUAL_FLOAT(kCopCurveSeed[kCopCurvePoints - 1].cop, arb.copAtOat(15.0f));
  // Exact points.
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.3f, arb.copAtOat(-10.0f));
  // Segment midpoints: (-30,1.4)-(-20,1.8) at -25 -> 1.6; (-10,2.3)-(0,2.9)
  // at -5 -> 2.6.
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.6f, arb.copAtOat(-25.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.6f, arb.copAtOat(-5.0f));
}

static void test_economic_balance_point_from_default_prices() {
  DualFuelArbiter arb;  // defaults: $0.15/kWh, $0.45/m3, AFUE 0.95
  // COP* = 0.15 × 10.55 × 0.95 / 0.45 = 3.341, crossing the seed curve in
  // the (0,2.9)-(8.3,3.6) segment at ~+5.2 C — inside [-8, 10], no clamp.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 3.341f, arb.breakEvenCop());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 5.23f, arb.economicBalancePointC());
}

static void test_economic_balance_clamped_to_safe_band() {
  // Cheap electricity: COP* below the whole curve -> switchover would fall
  // to -inf; it must clamp at the CAPACITY floor (balancePointC), because
  // below it the HP cannot carry the load regardless of price.
  DualFuelConfig cheap;
  cheap.economicEnabled = true;
  cheap.elecPricePerKwh = 0.02f;  // COP* ≈ 0.45 < curve min 1.4
  DualFuelArbiter arbCheap(cheap);
  TEST_ASSERT_EQUAL_FLOAT(cheap.balancePointC, arbCheap.economicBalancePointC());

  // Expensive electricity: COP* above the whole curve -> HP never economic;
  // clamps at the gas-lockout ceiling (auxMaxOatC), never beyond.
  DualFuelConfig dear;
  dear.economicEnabled = true;
  dear.elecPricePerKwh = 1.0f;  // COP* ≈ 22 > curve max 3.6
  DualFuelArbiter arbDear(dear);
  TEST_ASSERT_EQUAL_FLOAT(dear.auxMaxOatC, arbDear.economicBalancePointC());
}

static void test_economic_off_by_default_keeps_fixed_balance() {
  DualFuelConfig cfg;
  TEST_ASSERT_FALSE(cfg.economicEnabled);  // winter task: ships OFF
  DualFuelArbiter arb;
  // Effective balance is the fixed one...
  TEST_ASSERT_EQUAL_FLOAT(kBalancePointC, arb.effectiveBalancePointC());
  // ...so at OAT 0 (below the ~+5.2 economic balance but above -8) the HP
  // still runs — economics must not leak into the pick while disabled.
  TEST_ASSERT_TRUE(arb.step(hpCall(0.0f), 10).source == HeatSource::kHeatPump);
}

static void test_economic_switchover_moves_preference_with_hysteresis() {
  DualFuelConfig cfg;
  cfg.economicEnabled = true;  // defaults -> economic balance ≈ +5.2 C
  DualFuelArbiter arb(cfg);
  uint32_t t = 0;
  // Below the economic balance: gas preferred even though OAT is far above
  // the fixed -8 capacity floor.
  TEST_ASSERT_TRUE(arb.step(hpCall(0.0f), t += 10).source == HeatSource::kGas);
  // Inside the hysteresis band (5.2..7.2): still gas — no chatter.
  TEST_ASSERT_TRUE(arb.step(hpCall(6.5f), t += 10).source == HeatSource::kGas);
  // Clears balance + hyst: HP.
  TEST_ASSERT_TRUE(arb.step(hpCall(7.5f), t += 10).source == HeatSource::kHeatPump);
  // Dips back into the band: still HP.
  TEST_ASSERT_TRUE(arb.step(hpCall(6.0f), t += 10).source == HeatSource::kHeatPump);
}

static void test_economic_mode_never_violates_hard_floors() {
  // Whatever the prices say, the lockouts win: sweep both price extremes
  // across the full OAT range and assert the floors hold at every step.
  for (int priceCase = 0; priceCase < 2; ++priceCase) {
    DualFuelConfig cfg;
    cfg.economicEnabled = true;
    cfg.elecPricePerKwh = priceCase == 0 ? 0.02f : 1.0f;  // HP-always / gas-always
    DualFuelArbiter arb(cfg);
    uint32_t t = 0;
    for (float oat = -35.0f; oat <= 20.0f; oat += 0.5f) {
      const DualFuelOutput out = arb.step(hpCall(oat), t += 600);
      if (oat < cfg.compressorMinOatC)
        TEST_ASSERT_TRUE(out.source != HeatSource::kHeatPump);  // lockout floor
      if (oat > cfg.auxMaxOatC)
        TEST_ASSERT_TRUE(out.source != HeatSource::kGas);       // gas lockout
      TEST_ASSERT_FALSE(out.noSourcePermittedAlarm);
    }
    // The economic balance itself stayed inside the safe band.
    TEST_ASSERT_TRUE(arb.economicBalancePointC() >= cfg.balancePointC);
    TEST_ASSERT_TRUE(arb.economicBalancePointC() <= cfg.auxMaxOatC);
  }
}

static void test_economic_config_validation() {
  DualFuelConfig cfg;  // defaults valid
  TEST_ASSERT_TRUE(DualFuelArbiter::configValid(cfg));

  DualFuelConfig bad = cfg;
  bad.elecPricePerKwh = 0.0f;
  TEST_ASSERT_FALSE(DualFuelArbiter::configValid(bad));
  bad = cfg; bad.elecPricePerKwh = -0.15f;
  TEST_ASSERT_FALSE(DualFuelArbiter::configValid(bad));
  bad = cfg; bad.gasPricePerM3 = kEnergyPriceMax + 1.0f;
  TEST_ASSERT_FALSE(DualFuelArbiter::configValid(bad));
  bad = cfg; bad.gasPricePerM3 = NAN;
  TEST_ASSERT_FALSE(DualFuelArbiter::configValid(bad));
  bad = cfg; bad.afue = 0.3f;   // below the plausibility band
  TEST_ASSERT_FALSE(DualFuelArbiter::configValid(bad));
  bad = cfg; bad.afue = 1.2f;   // >100% on HHV-based AFUE is junk
  TEST_ASSERT_FALSE(DualFuelArbiter::configValid(bad));
  bad = cfg; bad.copCurve[2].oatC = bad.copCurve[1].oatC;  // not strictly increasing
  TEST_ASSERT_FALSE(DualFuelArbiter::configValid(bad));
  bad = cfg; bad.copCurve[3].cop = bad.copCurve[2].cop - 0.5f;  // COP decreasing
  TEST_ASSERT_FALSE(DualFuelArbiter::configValid(bad));
  bad = cfg; bad.copCurve[0].cop = 0.0f;
  TEST_ASSERT_FALSE(DualFuelArbiter::configValid(bad));

  // Economics fields are validated even with the mode OFF: enabling later
  // must never re-discover junk at runtime.
  bad = cfg; bad.economicEnabled = false; bad.gasPricePerM3 = -1.0f;
  TEST_ASSERT_FALSE(DualFuelArbiter::configValid(bad));

  // setConfig keeps the old config + flags the alarm, same as other rejects.
  DualFuelArbiter arb;
  bad = cfg; bad.elecPricePerKwh = NAN;
  TEST_ASSERT_FALSE(arb.setConfig(bad));
  TEST_ASSERT_EQUAL_FLOAT(kElecPricePerKwhDefault, arb.config().elecPricePerKwh);
  TEST_ASSERT_TRUE(arb.step(hpCall(-5.0f), 10).configRejectedAlarm);
}

static void test_defaults_match_canonical_config() {
  DualFuelConfig cfg;
  TEST_ASSERT_EQUAL_FLOAT(kBalancePointC, cfg.balancePointC);
  TEST_ASSERT_EQUAL_FLOAT(kBalancePointHystC, cfg.balanceHystC);
  TEST_ASSERT_EQUAL_FLOAT(kCompressorMinOatC, cfg.compressorMinOatC);
  TEST_ASSERT_EQUAL_FLOAT(kAuxMaxOatC, cfg.auxMaxOatC);
  TEST_ASSERT_EQUAL_FLOAT(kEscalationDroopC, cfg.escalationDroopC);
  TEST_ASSERT_EQUAL_UINT32(kEscalationMinS, cfg.escalationMinS);
  TEST_ASSERT_EQUAL_FLOAT(kEscalationHpDemandPct, cfg.escalationHpDemandPct);
  TEST_ASSERT_EQUAL_UINT32(kDeescalationMinS, cfg.deescalationMinS);
  TEST_ASSERT_EQUAL_FLOAT(kDefrostTemperHeatPct, cfg.defrostTemperHeatPct);
  TEST_ASSERT_EQUAL_UINT32(kDefrostTemperMaxS, cfg.defrostTemperMaxS);
  // #143 economics defaults (OFF by default — winter validation task).
  TEST_ASSERT_FALSE(cfg.economicEnabled);
  TEST_ASSERT_EQUAL_FLOAT(kElecPricePerKwhDefault, cfg.elecPricePerKwh);
  TEST_ASSERT_EQUAL_FLOAT(kGasPricePerM3Default, cfg.gasPricePerM3);
  TEST_ASSERT_EQUAL_FLOAT(kAfueDefault, cfg.afue);
  for (size_t i = 0; i < kCopCurvePoints; ++i) {
    TEST_ASSERT_EQUAL_FLOAT(kCopCurveSeed[i].oatC, cfg.copCurve[i].oatC);
    TEST_ASSERT_EQUAL_FLOAT(kCopCurveSeed[i].cop, cfg.copCurve[i].cop);
  }
  TEST_ASSERT_TRUE(DualFuelArbiter::configValid(cfg));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_boot_no_call_no_demand);
  RUN_TEST(test_balance_point_hysteresis_no_chatter);
  RUN_TEST(test_lockout_low_oat_forces_gas);
  RUN_TEST(test_lockout_high_oat_forces_hp);
  RUN_TEST(test_invalid_config_rejected);
  RUN_TEST(test_invalid_config_at_construction_falls_back_to_defaults);
  RUN_TEST(test_escalation_requires_droop_demand_and_duration);
  RUN_TEST(test_escalation_timer_resets_on_condition_break);
  RUN_TEST(test_no_escalation_below_demand_threshold_or_invalid_room);
  RUN_TEST(test_deescalation_needs_dwell_and_oat);
  RUN_TEST(test_deescalation_blocked_by_low_oat);
  RUN_TEST(test_escalation_latch_survives_call_cycling);
  RUN_TEST(test_oat_invalid_fails_cold);
  RUN_TEST(test_defrost_temper_shape_and_cap);
  RUN_TEST(test_temper_max_capped_at_canonical_constant);
  RUN_TEST(test_mutual_exclusion_single_source_always);
  RUN_TEST(test_break_even_cop_worked_example);
  RUN_TEST(test_cop_curve_interpolation_and_flat_ends);
  RUN_TEST(test_economic_balance_point_from_default_prices);
  RUN_TEST(test_economic_balance_clamped_to_safe_band);
  RUN_TEST(test_economic_off_by_default_keeps_fixed_balance);
  RUN_TEST(test_economic_switchover_moves_preference_with_hysteresis);
  RUN_TEST(test_economic_mode_never_violates_hard_floors);
  RUN_TEST(test_economic_config_validation);
  RUN_TEST(test_defaults_match_canonical_config);
  return UNITY_END();
}
