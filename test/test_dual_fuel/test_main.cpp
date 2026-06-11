// DualFuelArbiter tests (issue #32): balance-point hysteresis (no chatter),
// OAT lockout bands, invalid-config rejection, escalation timing and
// de-escalation gating, OAT-invalid fail-cold, defrost temper request shape
// + cap, mutual exclusion / boot state.
#include <unity.h>
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
  RUN_TEST(test_defaults_match_canonical_config);
  return UNITY_END();
}
