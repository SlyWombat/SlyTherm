// Unit tests for lib/PreCirculator — blower-first pre-circulation (issue
// #142, docs/13 §3+§8): trigger, lead-time composition with #141's
// crossingBias, cancel on prediction loss, override by a real call, the
// §8 cool-side default-off, the hovering-prediction run cap, and the
// #53 circulate-duty credit ledger.
#include <unity.h>

#include "DettsonConfig.h"
#include "PreCirculator.h"
#include "RecoveryEstimator.h"

using namespace dettson;

void setUp() {}
void tearDown() {}

static PreCirculator::Inputs heatPred(bool call = false) {
  PreCirculator::Inputs in;
  in.heatPredicted = true;
  in.callActive = call;
  return in;
}

static PreCirculator::Inputs coolPred(bool call = false) {
  PreCirculator::Inputs in;
  in.coolPredicted = true;
  in.callActive = call;
  return in;
}

// ---------- trigger ----------

static void test_idle_by_default_and_heat_trigger_starts_low_fan() {
  PreCirculator pc;
  PreCirculator::Inputs none;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pc.update(none, 1000));
  TEST_ASSERT_FALSE(pc.active());

  // Heat-side prediction (default enabled) -> the LOW fan level, no more.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, kBlowerFirstFanPct, pc.update(heatPred(), 1010));
  TEST_ASSERT_TRUE(pc.active());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, kBlowerFirstFanPct);  // CT-485 Low (0x32)
}

static void test_no_trigger_while_call_already_active() {
  PreCirculator pc;
  // A live call means the equipment already owns airflow: never start.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pc.update(heatPred(/*call=*/true), 1000));
  TEST_ASSERT_FALSE(pc.active());
}

// ---------- lead time (composition with #141's crossingBias) ----------

static void test_lead_time_gates_the_trigger_via_crossing_bias() {
  // The glue passes crossingBias(..., horizon = leadS).predicted as the
  // trigger. 0.10 C to go at 1.2 C/h -> crossing in 300 s: inside a 120 s
  // lead? No. Inside once it closes to 0.03 C (90 s out)? Yes.
  const uint32_t leadS = kBlowerFirstLeadS;
  TEST_ASSERT_EQUAL_UINT32(120, leadS);  // docs/05 mirror: 1-3 min band default

  const CrossingBias farOut =
      RecoveryEstimator::crossingBias(0.10f, 1.2f, leadS, kCoolPredictBiasMaxC);
  TEST_ASSERT_FALSE(farOut.predicted);  // crossing at 300 s > 120 s lead

  const CrossingBias inLead =
      RecoveryEstimator::crossingBias(0.03f, 1.2f, leadS, kCoolPredictBiasMaxC);
  TEST_ASSERT_TRUE(inLead.predicted);   // crossing at 90 s <= 120 s lead
  TEST_ASSERT_TRUE(inLead.inS <= leadS);

  PreCirculator pc;
  PreCirculator::Inputs in;
  in.heatPredicted = farOut.predicted;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pc.update(in, 1000));
  in.heatPredicted = inLead.predicted;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, kBlowerFirstFanPct, pc.update(in, 1010));
}

// ---------- cancel ----------

static void test_prediction_loss_cancels_the_pre_run() {
  PreCirculator pc;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, kBlowerFirstFanPct, pc.update(heatPred(), 1000));
  PreCirculator::Inputs none;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pc.update(none, 1060));  // evaporated
  TEST_ASSERT_FALSE(pc.active());
  // Re-arms cleanly on the next prediction.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, kBlowerFirstFanPct, pc.update(heatPred(), 1120));
}

// ---------- override ----------

static void test_real_call_overrides_and_holds_off_the_pre_run() {
  PreCirculator pc;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, kBlowerFirstFanPct, pc.update(heatPred(), 1000));
  // The call opens: pre-run yields immediately (the stage owns airflow)...
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pc.update(heatPred(/*call=*/true), 1090));
  TEST_ASSERT_FALSE(pc.active());
  // ...and stays off for as long as the call runs, prediction or not.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pc.update(heatPred(/*call=*/true), 1300));
}

// ---------- §8 cool-side policy ----------

static void test_cool_side_default_off_per_docs13_s8() {
  PreCirculator pc;
  // Humid-season rule (docs/13 §8): a pre-run before a cool call
  // re-evaporates the previous cycle's coil condensate — default OFF.
  TEST_ASSERT_FALSE(kBlowerFirstCoolEnabledDefault);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pc.update(coolPred(), 1000));
  TEST_ASSERT_FALSE(pc.active());
}

static void test_cool_side_runs_only_when_explicitly_enabled() {
  PreCirculator::Config cfg;
  cfg.coolEnabled = true;  // explicit owner decision (verified-dry conditions)
  PreCirculator pc(cfg);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, kBlowerFirstFanPct, pc.update(coolPred(), 1000));
}

static void test_heat_side_can_be_disabled_too() {
  PreCirculator::Config cfg;
  cfg.heatEnabled = false;
  PreCirculator pc(cfg);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pc.update(heatPred(), 1000));
}

// ---------- hovering-prediction cap ----------

static void test_max_run_cap_then_rearm_after_prediction_drops() {
  PreCirculator pc;
  uint32_t t = 1000;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, kBlowerFirstFanPct, pc.update(heatPred(), t));
  // Prediction hovers without the call ever opening: run exactly maxRunS.
  while (t < 1000 + kBlowerFirstMaxRunS - 10) {
    t += 10;
    TEST_ASSERT_FLOAT_WITHIN(0.001f, kBlowerFirstFanPct, pc.update(heatPred(), t));
  }
  t += 10;  // cap reached
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pc.update(heatPred(), t));
  // Still hovering: stays quiet (no fan flapping at the cap).
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pc.update(heatPred(), t + 60));
  // Prediction drops -> re-armed; the next prediction pre-runs again.
  PreCirculator::Inputs none;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pc.update(none, t + 120));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, kBlowerFirstFanPct, pc.update(heatPred(), t + 180));
}

static void test_spent_state_also_rearms_via_a_call() {
  PreCirculator pc;
  uint32_t t = 1000;
  pc.update(heatPred(), t);
  t += kBlowerFirstMaxRunS;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pc.update(heatPred(), t));  // spent
  // The call finally opens (spent -> idle), then ends: fresh trigger works.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pc.update(heatPred(/*call=*/true), t + 30));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, kBlowerFirstFanPct, pc.update(heatPred(), t + 900));
}

// ---------- #53 circulate-duty credit ----------

static void test_pre_run_seconds_credit_the_current_hour() {
  PreCirculator pc;
  const uint32_t t0 = 7200;  // exactly an hour boundary for a clean bucket
  TEST_ASSERT_EQUAL_UINT32(0, pc.dutyCreditS(t0));
  pc.update(heatPred(), t0);
  pc.update(heatPred(), t0 + 60);
  pc.update(heatPred(), t0 + 120);  // 120 s of pre-run so far
  TEST_ASSERT_EQUAL_UINT32(120, pc.dutyCreditS(t0 + 120));
  // Handoff to the call: the credit stays banked for this hour.
  pc.update(heatPred(/*call=*/true), t0 + 130);
  TEST_ASSERT_EQUAL_UINT32(130, pc.dutyCreditS(t0 + 130));
}

static void test_duty_credit_resets_when_the_hour_rolls() {
  PreCirculator pc;
  const uint32_t t0 = 7200;
  pc.update(heatPred(), t0);
  pc.update(heatPred(), t0 + 120);
  TEST_ASSERT_EQUAL_UINT32(120, pc.dutyCreditS(t0 + 120));
  // Query in a later hour without an update: no stale credit leaks.
  TEST_ASSERT_EQUAL_UINT32(0, pc.dutyCreditS(t0 + 3600));
  // A pre-run slice rediscovered after the roll is CLIPPED to the new
  // bucket: only the in-hour remainder (100 s here) is credited — the
  // pre-roll part belonged to the closed hour and never leaks forward.
  PreCirculator::Inputs none;
  pc.update(none, t0 + 3700);
  TEST_ASSERT_EQUAL_UINT32(100, pc.dutyCreditS(t0 + 3700));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_idle_by_default_and_heat_trigger_starts_low_fan);
  RUN_TEST(test_no_trigger_while_call_already_active);
  RUN_TEST(test_lead_time_gates_the_trigger_via_crossing_bias);
  RUN_TEST(test_prediction_loss_cancels_the_pre_run);
  RUN_TEST(test_real_call_overrides_and_holds_off_the_pre_run);
  RUN_TEST(test_cool_side_default_off_per_docs13_s8);
  RUN_TEST(test_cool_side_runs_only_when_explicitly_enabled);
  RUN_TEST(test_heat_side_can_be_disabled_too);
  RUN_TEST(test_max_run_cap_then_rearm_after_prediction_drops);
  RUN_TEST(test_spent_state_also_rearms_via_a_call);
  RUN_TEST(test_pre_run_seconds_credit_the_current_hour);
  RUN_TEST(test_duty_credit_resets_when_the_hour_rolls);
  return UNITY_END();
}
