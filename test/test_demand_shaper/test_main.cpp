// DemandShaper unit tests: GasShaper floor-snap hysteresis + max-runtime trip,
// HpInverterShaper slew/quantize/floor, HpRelayShaper duty + starts cap + gate.
#include <unity.h>
#include <cmath>
#include "DemandShaper.h"
#include "DettsonConfig.h"

using namespace dettson;

void setUp() {}
void tearDown() {}

// ------------------------------------------------------------------ GasShaper

// Floor-snap hysteresis tests run with the G14 min-on/min-off timers zeroed
// so they exercise only the snap behavior; timer tests use the defaults.
static GasShaper::Config gasNoTimers() {
  GasShaper::Config c;
  c.minOnS = 0;
  c.minOffS = 0;
  return c;
}

static void test_gas_boot_state_no_demand() {
  GasShaper g;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(0.0f, 0));
  TEST_ASSERT_FALSE(g.lit());
  TEST_ASSERT_FALSE(g.alarm());
}

static void test_gas_does_not_light_at_or_below_floor() {
  GasShaper g(gasNoTimers());  // light threshold = 40 + 2 = 42
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(39.0f, 0));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(40.0f, 10));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(41.9f, 20));
  TEST_ASSERT_FALSE(g.lit());
}

static void test_gas_lights_above_margin_and_tracks_request() {
  GasShaper g(gasNoTimers());
  TEST_ASSERT_EQUAL_FLOAT(45.0f, g.shape(45.0f, 0));
  TEST_ASSERT_TRUE(g.lit());
  TEST_ASSERT_EQUAL_FLOAT(80.0f, g.shape(80.0f, 10));
  TEST_ASSERT_EQUAL_FLOAT(100.0f, g.shape(100.0f, 20));
}

static void test_gas_clamps_overrange_request_to_100() {
  GasShaper g(gasNoTimers());
  TEST_ASSERT_EQUAL_FLOAT(100.0f, g.shape(150.0f, 0));
  TEST_ASSERT_FALSE(g.alarm());
}

static void test_gas_falling_holds_floor_then_extinguishes() {
  GasShaper g(gasNoTimers());  // extinguish threshold = 40 - 5 = 35
  g.shape(60.0f, 0);
  TEST_ASSERT_EQUAL_FLOAT(40.0f, g.shape(38.0f, 10));  // below floor: hold floor, stay lit
  TEST_ASSERT_EQUAL_FLOAT(40.0f, g.shape(36.0f, 20));
  TEST_ASSERT_TRUE(g.lit());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(35.0f, 30));   // clearly below: out
  TEST_ASSERT_FALSE(g.lit());
}

static void test_gas_no_dither_at_floor_either_direction() {
  GasShaper g(gasNoTimers());
  // Lit, request oscillating just around the floor: burner never flaps off —
  // sub-floor requests pin to the floor, in-band requests pass through.
  g.shape(60.0f, 0);
  for (uint32_t t = 10; t <= 100; t += 10) {
    float req = (t / 10) % 2 ? 39.0f : 41.0f;
    float expected = req < 40.0f ? 40.0f : req;
    TEST_ASSERT_EQUAL_FLOAT(expected, g.shape(req, t));
    TEST_ASSERT_TRUE(g.lit());
  }
  // Unlit, same oscillation: output pinned at 0 (never lights below 42).
  GasShaper g2(gasNoTimers());
  for (uint32_t t = 0; t <= 100; t += 10) {
    float req = (t / 10) % 2 ? 39.0f : 41.0f;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, g2.shape(req, t));
  }
  TEST_ASSERT_FALSE(g2.lit());
}

static void test_gas_relight_requires_full_on_threshold() {
  GasShaper g(gasNoTimers());
  g.shape(60.0f, 0);
  g.shape(30.0f, 10);  // extinguished
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(41.0f, 20));  // 40..42: not enough to relight
  TEST_ASSERT_EQUAL_FLOAT(42.0f, g.shape(42.0f, 30));
}

static void test_gas_invalid_input_drops_demand_and_alarms() {
  GasShaper g(gasNoTimers());
  g.shape(60.0f, 0);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(NAN, 10));
  TEST_ASSERT_TRUE(g.inputAlarm());
  TEST_ASSERT_FALSE(g.lit());
  GasShaper g2(gasNoTimers());
  g2.shape(60.0f, 0);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g2.shape(-5.0f, 10));
  TEST_ASSERT_TRUE(g2.inputAlarm());
}

static void test_gas_max_runtime_trips_then_auto_relights_while_call_persists() {
  // #158: a legit long call must not lose heat permanently. With no timers the
  // min-off rest is zero, so the cycle after the trip relights for a fresh run.
  GasShaper g(gasNoTimers());
  TEST_ASSERT_EQUAL_FLOAT(60.0f, g.shape(60.0f, 0));
  TEST_ASSERT_EQUAL_FLOAT(60.0f, g.shape(60.0f, kGasMaxRuntimeS));      // exactly at cap: still ok
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(60.0f, kGasMaxRuntimeS + 1));   // exceeds -> trip + drop
  TEST_ASSERT_TRUE(g.runtimeAlarm());
  TEST_ASSERT_FALSE(g.lit());
  // Call still up: relight for another timed run rather than latching at 0.
  TEST_ASSERT_EQUAL_FLOAT(60.0f, g.shape(60.0f, kGasMaxRuntimeS + 2));
  TEST_ASSERT_TRUE(g.lit());
  TEST_ASSERT_TRUE(g.runtimeAlarm());                                   // alarm stays latched
  // The relit run gets its own maxRuntime window, measured from the relight.
  TEST_ASSERT_EQUAL_FLOAT(60.0f, g.shape(60.0f, 2 * kGasMaxRuntimeS + 2));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(60.0f, 2 * kGasMaxRuntimeS + 3)); // relit run exceeds -> trip
  // Call ends: the trip clears; the alarm remains latched until acknowledged.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(0.0f, 2 * kGasMaxRuntimeS + 10));
  TEST_ASSERT_TRUE(g.runtimeAlarm());
  g.clearAlarms();
  TEST_ASSERT_FALSE(g.alarm());
}

static void test_gas_max_runtime_relight_waits_for_min_off() {
  // #158: the relight after a trip still owes the normal min-off rest.
  GasShaper g;                          // default (nonzero) timers
  g.bootRestore(0, /*minOffServed=*/true);
  TEST_ASSERT_EQUAL_FLOAT(60.0f, g.shape(60.0f, 0));                        // lights
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(60.0f, kGasMaxRuntimeS + 1));       // trip -> min-off starts
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(60.0f, kGasMaxRuntimeS + kGasMinOffS)); // 1 s short: no relight
  TEST_ASSERT_FALSE(g.lit());
  TEST_ASSERT_EQUAL_FLOAT(60.0f, g.shape(60.0f, kGasMaxRuntimeS + 1 + kGasMinOffS)); // served -> relight
  TEST_ASSERT_TRUE(g.lit());
}

static void test_gas_extinguish_resets_runtime_clock() {
  GasShaper g(gasNoTimers());
  g.shape(60.0f, 0);
  g.shape(0.0f, 1000);            // run ends after 1000 s
  g.shape(60.0f, 2000);           // new run
  // Old start must not count: 2000 + max is fine, trips only past new start + max.
  TEST_ASSERT_EQUAL_FLOAT(60.0f, g.shape(60.0f, 2000 + kGasMaxRuntimeS));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(60.0f, 2001 + kGasMaxRuntimeS));
}

// ------------------------------------------- GasShaper min-on/min-off (G14)

static void test_gas_boot_starts_off_timer_fresh() {
  GasShaper g;  // default timers; no bootRestore(): conservative
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(60.0f, 0));  // anchors the off-timer
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(60.0f, kGasMinOffS - 1));
  TEST_ASSERT_FALSE(g.lit());
  TEST_ASSERT_FALSE(g.alarm());  // a timer hold is not a fault
  TEST_ASSERT_EQUAL_FLOAT(60.0f, g.shape(60.0f, kGasMinOffS));
  TEST_ASSERT_TRUE(g.lit());
}

static void test_gas_boot_restore_served_lights_immediately() {
  GasShaper g;
  g.bootRestore(0, /*minOffServed=*/true);  // persisted state proves off-time served
  TEST_ASSERT_EQUAL_FLOAT(60.0f, g.shape(60.0f, 0));
  TEST_ASSERT_TRUE(g.lit());
}

static void test_gas_boot_restore_unserved_holds_full_min_off() {
  GasShaper g;
  g.bootRestore(100, /*minOffServed=*/false);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(60.0f, 100 + kGasMinOffS - 1));
  TEST_ASSERT_EQUAL_FLOAT(60.0f, g.shape(60.0f, 100 + kGasMinOffS));
}

static void test_gas_min_on_blocks_comfort_stop_holds_min_fire() {
  GasShaper g;
  g.bootRestore(0, true);
  TEST_ASSERT_EQUAL_FLOAT(60.0f, g.shape(60.0f, 0));
  // Request drops out (comfort stop): held lit at minFire until min-on served.
  TEST_ASSERT_EQUAL_FLOAT(kGasFloorPct, g.shape(0.0f, 10));
  TEST_ASSERT_TRUE(g.lit());
  TEST_ASSERT_EQUAL_FLOAT(kGasFloorPct, g.shape(0.0f, kGasMinOnS - 1));
  TEST_ASSERT_TRUE(g.lit());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(0.0f, kGasMinOnS));  // served: out
  TEST_ASSERT_FALSE(g.lit());
  TEST_ASSERT_FALSE(g.alarm());
}

static void test_gas_min_off_blocks_relight_after_comfort_stop() {
  GasShaper g;
  g.bootRestore(0, true);
  g.shape(60.0f, 0);
  g.shape(0.0f, kGasMinOnS);  // comfort stop lands exactly at min-on
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(60.0f, kGasMinOnS + kGasMinOffS - 1));
  TEST_ASSERT_FALSE(g.lit());
  TEST_ASSERT_EQUAL_FLOAT(60.0f, g.shape(60.0f, kGasMinOnS + kGasMinOffS));
  TEST_ASSERT_TRUE(g.lit());
}

static void test_gas_safety_stop_invalid_input_bypasses_min_on() {
  GasShaper g;
  g.bootRestore(0, true);
  g.shape(60.0f, 0);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(NAN, 10));  // safety: immediate despite min-on
  TEST_ASSERT_FALSE(g.lit());
  TEST_ASSERT_TRUE(g.inputAlarm());
  // The safety stop started min-off: relight waits.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(60.0f, 10 + kGasMinOffS - 1));
  TEST_ASSERT_EQUAL_FLOAT(60.0f, g.shape(60.0f, 10 + kGasMinOffS));
}

static void test_gas_force_stop_immediate_and_starts_min_off() {
  GasShaper g;
  g.bootRestore(0, true);
  g.shape(60.0f, 0);
  g.forceStop(50);  // sensor fault / invariant trip / watchdog path
  TEST_ASSERT_FALSE(g.lit());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(60.0f, 51));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(60.0f, 50 + kGasMinOffS - 1));
  TEST_ASSERT_EQUAL_FLOAT(60.0f, g.shape(60.0f, 50 + kGasMinOffS));
}

static void test_gas_runtime_trip_bypasses_min_on() {
  GasShaper::Config c;
  c.maxRuntimeS = 100;
  c.minOnS = 10000;  // far longer than the runtime cap
  c.minOffS = 0;
  GasShaper g(c);
  g.shape(60.0f, 0);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.shape(60.0f, 101));  // safety trip wins over min-on
  TEST_ASSERT_FALSE(g.lit());
  TEST_ASSERT_TRUE(g.runtimeAlarm());
}

// ----------------------------------------------------------- HpInverterShaper

static void test_hp_inv_starts_at_floor_then_slews_up() {
  HpInverterShaper h;  // floor 30, slew 10 %/min, step 5
  TEST_ASSERT_EQUAL_FLOAT(30.0f, h.shape(60.0f, 0));    // start at floor
  TEST_ASSERT_EQUAL_FLOAT(40.0f, h.shape(60.0f, 60));   // +10 after 1 min
  TEST_ASSERT_EQUAL_FLOAT(50.0f, h.shape(60.0f, 120));
  TEST_ASSERT_EQUAL_FLOAT(60.0f, h.shape(60.0f, 180));  // reached target
  TEST_ASSERT_EQUAL_FLOAT(60.0f, h.shape(60.0f, 240));  // holds
}

static void test_hp_inv_quantizes_to_step() {
  HpInverterShaper h;
  h.shape(100.0f, 0);                                    // 30
  TEST_ASSERT_EQUAL_FLOAT(35.0f, h.shape(100.0f, 30));   // +5 after 30 s
  TEST_ASSERT_EQUAL_FLOAT(40.0f, h.shape(43.0f, 60));    // target 43 -> raw 40 -> q 40
  TEST_ASSERT_EQUAL_FLOAT(45.0f, h.shape(43.0f, 90));    // raw 43 -> rounds to 45
}

static void test_hp_inv_small_dt_accumulates() {
  HpInverterShaper h;
  h.shape(100.0f, 0);  // 30
  // 6 s calls = 1 % raw each; quantized output may not move every call but
  // must not be lost to rounding.
  float out = 0.0f;
  for (uint32_t t = 6; t <= 60; t += 6) out = h.shape(100.0f, t);
  TEST_ASSERT_EQUAL_FLOAT(40.0f, out);  // 30 + 10 %/min after 1 min
}

static void test_hp_inv_below_floor_request_runs_at_floor() {
  HpInverterShaper h;
  TEST_ASSERT_EQUAL_FLOAT(30.0f, h.shape(10.0f, 0));
  TEST_ASSERT_EQUAL_FLOAT(30.0f, h.shape(10.0f, 600));
}

static void test_hp_inv_drop_to_zero_is_immediate() {
  HpInverterShaper h;
  h.shape(100.0f, 0);
  h.shape(100.0f, 300);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, h.shape(0.0f, 301));  // no slow ramp-down to off
}

static void test_hp_inv_slew_down_limited_but_not_below_floor() {
  HpInverterShaper h;
  h.shape(100.0f, 0);
  for (uint32_t t = 60; t <= 420; t += 60) h.shape(100.0f, t);  // reach 100
  TEST_ASSERT_EQUAL_FLOAT(90.0f, h.shape(30.0f, 480));          // -10/min
  TEST_ASSERT_EQUAL_FLOAT(80.0f, h.shape(30.0f, 540));
}

static void test_hp_inv_invalid_input_zeroes_and_alarms() {
  HpInverterShaper h;
  h.shape(100.0f, 0);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, h.shape(NAN, 60));
  TEST_ASSERT_TRUE(h.inputAlarm());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, h.shape(-1.0f, 120));
}

// -------------------------------------------------------------- HpRelayShaper

struct FakeGate : public CompressorGate {
  bool allowStart = true;
  bool allowStop = true;
  int startAsks = 0;
  int stopAsks = 0;
  bool canStart(uint32_t) override { ++startAsks; return allowStart; }
  bool canStop(uint32_t) override { ++stopAsks; return allowStop; }
};

static void test_hp_relay_boot_off_and_first_start() {
  FakeGate gate;
  HpRelayShaper r(gate);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.shape(0.0f, 0));
  TEST_ASSERT_EQUAL_FLOAT(100.0f, r.shape(50.0f, 10));  // gate said yes; no off-phase wait at boot
  TEST_ASSERT_TRUE(r.on());
  TEST_ASSERT_EQUAL_UINT8(1, r.startsInLastHour(10));
}

static void test_hp_relay_duty_cycle_timing() {
  FakeGate gate;
  HpRelayShaper r(gate);  // 3 starts/h -> period 1200 s; duty 50 % -> 600 on / 600 off
  TEST_ASSERT_EQUAL_FLOAT(100.0f, r.shape(50.0f, 0));
  TEST_ASSERT_EQUAL_FLOAT(100.0f, r.shape(50.0f, 599));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.shape(50.0f, 600));    // on-time served
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.shape(50.0f, 1199));   // off-time pending
  TEST_ASSERT_EQUAL_FLOAT(100.0f, r.shape(50.0f, 1200)); // next cycle
  TEST_ASSERT_EQUAL_UINT8(2, r.startsInLastHour(1200));
}

static void test_hp_relay_request_zero_turns_off_via_gate() {
  FakeGate gate;
  HpRelayShaper r(gate);
  r.shape(80.0f, 0);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.shape(0.0f, 100));
  TEST_ASSERT_TRUE(gate.stopAsks > 0);
}

static void test_hp_relay_gate_refuses_stop_holds_on() {
  FakeGate gate;
  HpRelayShaper r(gate);
  r.shape(80.0f, 0);
  gate.allowStop = false;  // min-on unsatisfied
  TEST_ASSERT_EQUAL_FLOAT(100.0f, r.shape(0.0f, 100));  // timers honored on the way down
  gate.allowStop = true;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.shape(0.0f, 400));
}

static void test_hp_relay_gate_refuses_start_stays_off() {
  FakeGate gate;
  gate.allowStart = false;
  HpRelayShaper r(gate);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.shape(100.0f, 0));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.shape(100.0f, 100));
  TEST_ASSERT_EQUAL_UINT8(0, r.startsInLastHour(100));
  gate.allowStart = true;
  TEST_ASSERT_EQUAL_FLOAT(100.0f, r.shape(100.0f, 200));
}

static void test_hp_relay_starts_per_hour_cap_holds_even_with_permissive_gate() {
  FakeGate gate;
  HpRelayShaper r(gate);
  // request 100 -> off-time 0, so only the budget brakes restarts.
  TEST_ASSERT_EQUAL_FLOAT(100.0f, r.shape(100.0f, 0));    // start 1
  r.shape(0.0f, 10);
  TEST_ASSERT_EQUAL_FLOAT(100.0f, r.shape(100.0f, 20));   // start 2
  r.shape(0.0f, 30);
  TEST_ASSERT_EQUAL_FLOAT(100.0f, r.shape(100.0f, 40));   // start 3
  r.shape(0.0f, 50);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.shape(100.0f, 60));     // budget spent
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.shape(100.0f, 3599));
  TEST_ASSERT_EQUAL_FLOAT(100.0f, r.shape(100.0f, 3601)); // start 1 aged out
}

static void test_hp_relay_full_demand_runs_continuously() {
  FakeGate gate;
  HpRelayShaper r(gate);
  r.shape(100.0f, 0);
  TEST_ASSERT_EQUAL_FLOAT(100.0f, r.shape(100.0f, 5000));   // > period: still on, no cycling
  TEST_ASSERT_EQUAL_FLOAT(100.0f, r.shape(100.0f, 100000)); // no runtime cap on the HP (docs/05)
  TEST_ASSERT_EQUAL_UINT8(0, r.startsInLastHour(100000));
}

static void test_hp_relay_invalid_input_drops_and_alarms() {
  FakeGate gate;
  HpRelayShaper r(gate);
  r.shape(80.0f, 0);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, r.shape(NAN, 100));
  TEST_ASSERT_TRUE(r.inputAlarm());
}

static void test_hp_relay_invalid_input_still_respects_min_on() {
  FakeGate gate;
  HpRelayShaper r(gate);
  r.shape(80.0f, 0);
  gate.allowStop = false;
  TEST_ASSERT_EQUAL_FLOAT(100.0f, r.shape(NAN, 5));  // alarm, but no timer violation
  TEST_ASSERT_TRUE(r.inputAlarm());
}

// ------------------------------------------------------------ StagedCoolShaper

// Timing tests use round numbers instead of the field-derived defaults:
// duty 50 % -> period max(1200, 600/0.5, 300/0.5) = 1200 -> 600 on / 600 off.
static StagedCoolShaper::Config coolCfg() {
  StagedCoolShaper::Config c;
  c.stagePct = 30.0f;
  c.maxStartsPerH = 3;
  c.minOnS = 600;
  c.minOffS = 300;
  c.cyclePeriodS = 1200;
  c.fullDutyErrC = 0.5f;
  return c;
}

static void test_cool_boot_state_no_demand() {
  FakeGate gate;
  StagedCoolShaper s(gate);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.shape(0.0f, 0));
  TEST_ASSERT_FALSE(s.on());
  TEST_ASSERT_FALSE(s.alarm());
}

static void test_cool_output_is_stage_pct_never_request() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(100.0f, 0));  // honest 30 % intent (#140)
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(73.0f, 60));
}

static void test_cool_request_from_error_band() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());  // band top 0.5 C
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.requestFromError(0.0f));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.requestFromError(-1.0f));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.requestFromError(NAN));
  TEST_ASSERT_EQUAL_FLOAT(50.0f, s.requestFromError(0.25f));
  TEST_ASSERT_EQUAL_FLOAT(100.0f, s.requestFromError(0.5f));
  TEST_ASSERT_EQUAL_FLOAT(100.0f, s.requestFromError(2.0f));  // saturates, never >100
}

static void test_cool_request_from_error_with_prediction_bias() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());  // band top 0.5 C
  // #141 seam: the bias is additive degrees through the same band...
  TEST_ASSERT_EQUAL_FLOAT(50.0f, s.requestFromError(0.15f, 0.10f));
  // ...and lets a predicted crossing raise a gentle request while the error
  // is still at/below zero (pre-crossing) — the whole point of the seam.
  TEST_ASSERT_EQUAL_FLOAT(20.0f, s.requestFromError(0.0f, 0.10f));
  TEST_ASSERT_EQUAL_FLOAT(10.0f, s.requestFromError(-0.05f, 0.10f));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.requestFromError(-0.20f, 0.10f));
  // Zero bias reproduces the plain band exactly (#140 unchanged).
  TEST_ASSERT_EQUAL_FLOAT(s.requestFromError(0.25f),
                          s.requestFromError(0.25f, 0.0f));
  // Advisory contract: NaN/negative bias never reduces the request...
  TEST_ASSERT_EQUAL_FLOAT(50.0f, s.requestFromError(0.25f, -0.30f));
  TEST_ASSERT_EQUAL_FLOAT(50.0f, s.requestFromError(0.25f, NAN));
  // ...a NaN error still yields no demand, and saturation still caps at 100.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.requestFromError(NAN, 0.10f));
  TEST_ASSERT_EQUAL_FLOAT(100.0f, s.requestFromError(0.45f, 0.10f));
}

static void test_cool_duty_cycle_timing() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());  // duty 50 % -> 600 on / 600 off
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(50.0f, 0));
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(50.0f, 599));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.shape(50.0f, 600));    // on-phase served
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.shape(50.0f, 1199));   // off-phase pending
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(50.0f, 1200));  // next cycle
  TEST_ASSERT_EQUAL_UINT8(2, s.startsInLastHour(1200));
}

static void test_cool_low_duty_stretches_period_for_min_run() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());  // duty 20 % -> P = 600/0.2 = 3000: 600 on / 2400 off
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(20.0f, 0));
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(20.0f, 599));   // min-run never truncated
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.shape(20.0f, 600));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.shape(20.0f, 2999));   // duty fraction preserved
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(20.0f, 3000));
}

static void test_cool_high_duty_pins_off_time_at_min_off() {
  FakeGate gate;
  // duty 87.5 % (exactly representable) -> P = 300/0.125 = 2400: 2100 on / 300 off
  StagedCoolShaper s(gate, coolCfg());
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(87.5f, 0));
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(87.5f, 2099));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.shape(87.5f, 2100));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.shape(87.5f, 2399));
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(87.5f, 2400));
}

static void test_cool_full_duty_runs_continuously() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());
  s.shape(100.0f, 0);
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(100.0f, 5000));    // > any period: no cycling
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(100.0f, 100000));  // no runtime cap (docs/05: HP alarm-only)
  TEST_ASSERT_EQUAL_UINT8(0, s.startsInLastHour(100000));
}

static void test_cool_comfort_stop_held_to_min_run() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());
  s.shape(100.0f, 0);
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(0.0f, 100));  // call ended early: hold the run
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(0.0f, 599));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.shape(0.0f, 600));   // min-run served -> off
}

static void test_cool_restart_after_stop_waits_min_off() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());
  s.shape(100.0f, 0);
  s.shape(0.0f, 600);  // comfort stop at min-run
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.shape(100.0f, 899));  // full duty: off-time = minOffS
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(100.0f, 900));
}

static void test_cool_invalid_input_drops_without_min_run_hold() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());
  s.shape(100.0f, 0);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.shape(NAN, 60));  // safety-flavored: no demand hold
  TEST_ASSERT_TRUE(s.inputAlarm());
  TEST_ASSERT_TRUE(gate.stopAsks > 0);  // ...but the drop still went through the gate
}

static void test_cool_invalid_input_still_respects_gate_min_on() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());
  s.shape(100.0f, 0);
  gate.allowStop = false;
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(NAN, 5));  // alarm, but no timer violation
  TEST_ASSERT_TRUE(s.inputAlarm());
}

static void test_cool_gate_refuses_start_stays_off() {
  FakeGate gate;
  gate.allowStart = false;
  StagedCoolShaper s(gate, coolCfg());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.shape(100.0f, 0));
  TEST_ASSERT_EQUAL_UINT8(0, s.startsInLastHour(0));
  gate.allowStart = true;
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(100.0f, 60));
}

static void test_cool_starts_per_hour_cap_holds_even_with_permissive_gate() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());  // cap 3/h
  // Invalid-input drops bypass the demand min-run, making rapid restart
  // attempts possible — the budget ring must still hold the line.
  s.shape(100.0f, 0);      // start 1
  s.shape(NAN, 10);        // off
  s.shape(100.0f, 310);    // start 2 (min-off 300 served)
  s.shape(NAN, 320);       // off
  s.shape(100.0f, 620);    // start 3
  s.shape(NAN, 630);       // off
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.shape(100.0f, 930));  // budget spent
  TEST_ASSERT_EQUAL_UINT8(3, s.startsInLastHour(930));
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(100.0f, 3601));  // start 1 aged out
}

static void test_cool_config_clamps_period_to_starts_cap() {
  StagedCoolShaper::Config c = coolCfg();
  c.cyclePeriodS = 100;  // shorter than 3600/maxStartsPerH: hygiene floor wins
  FakeGate gate;
  StagedCoolShaper s(gate, c);
  TEST_ASSERT_EQUAL_UINT32(1200, s.config().cyclePeriodS);
}

// ---- #151 manual-start bypass of the demand-level min-OFF -----------------
// The AUTO baseline is test_cool_restart_after_stop_waits_min_off above: after
// a comfort stop at 600, a restart at full duty is blocked until minOffS=300
// (ready at 900). These tests arm the manual bypass on that same trace.

static void test_cool_manual_bypasses_min_off() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());
  s.shape(100.0f, 0);          // start 1 (everCycled)
  s.shape(0.0f, 600);          // comfort stop at min-run -> off at 600
  s.armManual(610);            // user drops the cool setpoint
  // AUTO would sit off until 900; the manual arm starts immediately instead.
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(100.0f, 650));
  TEST_ASSERT_TRUE(s.on());
}

static void test_cool_manual_does_not_bypass_starts_cap() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());  // cap 3/h
  // Spend the whole starts budget (invalid drops bypass min-run, like the
  // existing cap test), then a manual arm must STILL be denied.
  s.shape(100.0f, 0);      // start 1
  s.shape(NAN, 10);        // off
  s.shape(100.0f, 310);    // start 2 (min-off 300 served)
  s.shape(NAN, 320);       // off
  s.shape(100.0f, 620);    // start 3
  s.shape(NAN, 630);       // off
  s.armManual(640);        // manual bypass of min-OFF...
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.shape(100.0f, 930));  // ...but the cap holds
  TEST_ASSERT_EQUAL_UINT8(3, s.startsInLastHour(930));
}

static void test_cool_manual_does_not_bypass_gate() {
  FakeGate gate;
  gate.allowStart = false;   // CompressorGuard downstream refuses the start
  StagedCoolShaper s(gate, coolCfg());
  s.armManual(0);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.shape(100.0f, 10));  // manual can't override the gate
  TEST_ASSERT_EQUAL_UINT8(0, s.startsInLastHour(10));
}

static void test_cool_manual_arm_is_one_shot() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());
  s.shape(100.0f, 0);          // start 1
  s.shape(0.0f, 600);          // off at 600
  s.armManual(601);
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(100.0f, 610));  // manual start 2 (consumes arm)
  s.shape(0.0f, 1210);         // held to min-run (610+600), then off
  TEST_ASSERT_FALSE(s.on());
  // A LATER automatic start must again wait the full min-off (arm was one-shot).
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.shape(100.0f, 1509));  // 1210 + 299 < min-off
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(100.0f, 1510));  // min-off served
}

static void test_cool_manual_arm_expires() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());  // manualArmWindowS defaults to 120
  s.shape(100.0f, 0);
  s.shape(0.0f, 600);          // off at 600
  s.armManual(601);
  // Attempt past the arm window (601+121): the stale arm no longer bypasses,
  // and min-off (ready at 900) is not yet served.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.shape(100.0f, 722));
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(100.0f, 900));  // normal min-off still governs
}

static void test_cool_manual_started_run_still_holds_min_on() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());
  s.shape(100.0f, 0);
  s.shape(0.0f, 600);          // off at 600
  s.armManual(601);
  s.shape(100.0f, 610);        // manual start 2
  // min-ON is preserved for a manual-started run: an early call-drop is held.
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(0.0f, 650));   // held (610+600 not served)
  TEST_ASSERT_EQUAL_FLOAT(30.0f, s.shape(0.0f, 1209));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.shape(0.0f, 1210));   // min-run served -> off
}

// minOffRemainingS surfaces the demand-level rest for the UI "Cooling soon" ack.
static void test_cool_min_off_remaining_counts_down_after_stop() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());  // minOffS = 300
  TEST_ASSERT_EQUAL_UINT32(0, s.minOffRemainingS(0));    // never cycled -> nothing owed
  s.shape(100.0f, 0);                                    // start
  TEST_ASSERT_EQUAL_UINT32(0, s.minOffRemainingS(100));  // running -> 0
  s.shape(0.0f, 600);                                    // comfort stop at 600 (min-on served)
  TEST_ASSERT_FALSE(s.on());
  TEST_ASSERT_EQUAL_UINT32(300, s.minOffRemainingS(600));  // full rest ahead
  TEST_ASSERT_EQUAL_UINT32(100, s.minOffRemainingS(800));  // 200 s elapsed
  TEST_ASSERT_EQUAL_UINT32(0, s.minOffRemainingS(900));    // rest served
  TEST_ASSERT_EQUAL_UINT32(0, s.minOffRemainingS(1000));   // stays 0
}

static void test_cool_min_off_remaining_zero_while_manual_armed() {
  FakeGate gate;
  StagedCoolShaper s(gate, coolCfg());
  s.shape(100.0f, 0);
  s.shape(0.0f, 600);           // off at 600
  s.armManual(601);             // #151 manual bypass armed within the window
  TEST_ASSERT_EQUAL_UINT32(0, s.minOffRemainingS(650));  // bypass waives the rest -> no wait shown
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_gas_boot_state_no_demand);
  RUN_TEST(test_gas_does_not_light_at_or_below_floor);
  RUN_TEST(test_gas_lights_above_margin_and_tracks_request);
  RUN_TEST(test_gas_clamps_overrange_request_to_100);
  RUN_TEST(test_gas_falling_holds_floor_then_extinguishes);
  RUN_TEST(test_gas_no_dither_at_floor_either_direction);
  RUN_TEST(test_gas_relight_requires_full_on_threshold);
  RUN_TEST(test_gas_invalid_input_drops_demand_and_alarms);
  RUN_TEST(test_gas_max_runtime_trips_then_auto_relights_while_call_persists);
  RUN_TEST(test_gas_max_runtime_relight_waits_for_min_off);
  RUN_TEST(test_gas_extinguish_resets_runtime_clock);
  RUN_TEST(test_gas_boot_starts_off_timer_fresh);
  RUN_TEST(test_gas_boot_restore_served_lights_immediately);
  RUN_TEST(test_gas_boot_restore_unserved_holds_full_min_off);
  RUN_TEST(test_gas_min_on_blocks_comfort_stop_holds_min_fire);
  RUN_TEST(test_gas_min_off_blocks_relight_after_comfort_stop);
  RUN_TEST(test_gas_safety_stop_invalid_input_bypasses_min_on);
  RUN_TEST(test_gas_force_stop_immediate_and_starts_min_off);
  RUN_TEST(test_gas_runtime_trip_bypasses_min_on);
  RUN_TEST(test_hp_inv_starts_at_floor_then_slews_up);
  RUN_TEST(test_hp_inv_quantizes_to_step);
  RUN_TEST(test_hp_inv_small_dt_accumulates);
  RUN_TEST(test_hp_inv_below_floor_request_runs_at_floor);
  RUN_TEST(test_hp_inv_drop_to_zero_is_immediate);
  RUN_TEST(test_hp_inv_slew_down_limited_but_not_below_floor);
  RUN_TEST(test_hp_inv_invalid_input_zeroes_and_alarms);
  RUN_TEST(test_hp_relay_boot_off_and_first_start);
  RUN_TEST(test_hp_relay_duty_cycle_timing);
  RUN_TEST(test_hp_relay_request_zero_turns_off_via_gate);
  RUN_TEST(test_hp_relay_gate_refuses_stop_holds_on);
  RUN_TEST(test_hp_relay_gate_refuses_start_stays_off);
  RUN_TEST(test_hp_relay_starts_per_hour_cap_holds_even_with_permissive_gate);
  RUN_TEST(test_hp_relay_full_demand_runs_continuously);
  RUN_TEST(test_hp_relay_invalid_input_drops_and_alarms);
  RUN_TEST(test_hp_relay_invalid_input_still_respects_min_on);
  RUN_TEST(test_cool_boot_state_no_demand);
  RUN_TEST(test_cool_output_is_stage_pct_never_request);
  RUN_TEST(test_cool_request_from_error_band);
  RUN_TEST(test_cool_request_from_error_with_prediction_bias);
  RUN_TEST(test_cool_duty_cycle_timing);
  RUN_TEST(test_cool_low_duty_stretches_period_for_min_run);
  RUN_TEST(test_cool_high_duty_pins_off_time_at_min_off);
  RUN_TEST(test_cool_full_duty_runs_continuously);
  RUN_TEST(test_cool_comfort_stop_held_to_min_run);
  RUN_TEST(test_cool_restart_after_stop_waits_min_off);
  RUN_TEST(test_cool_invalid_input_drops_without_min_run_hold);
  RUN_TEST(test_cool_invalid_input_still_respects_gate_min_on);
  RUN_TEST(test_cool_gate_refuses_start_stays_off);
  RUN_TEST(test_cool_starts_per_hour_cap_holds_even_with_permissive_gate);
  RUN_TEST(test_cool_config_clamps_period_to_starts_cap);
  RUN_TEST(test_cool_manual_bypasses_min_off);
  RUN_TEST(test_cool_manual_does_not_bypass_starts_cap);
  RUN_TEST(test_cool_manual_does_not_bypass_gate);
  RUN_TEST(test_cool_manual_arm_is_one_shot);
  RUN_TEST(test_cool_manual_arm_expires);
  RUN_TEST(test_cool_manual_started_run_still_holds_min_on);
  RUN_TEST(test_cool_min_off_remaining_counts_down_after_stop);
  RUN_TEST(test_cool_min_off_remaining_zero_while_manual_armed);
  return UNITY_END();
}
