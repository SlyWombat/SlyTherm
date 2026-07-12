// CompressorGuard unit tests: min-off/min-on/starts-per-hour matrix,
// boot-unknown hold-off, persistence round-trip (min-off across reboot),
// reset-loop latch + manual clear, safety-stop-always-allowed (docs/04
// §1a/§2/§3; docs/05 defaults table).
#include <unity.h>

#include <cstring>

#include "CompressorGuard.h"
#include "DettsonConfig.h"

using dettson::CompressorGuard;
using Deny = CompressorGuard::Deny;
using Blob = CompressorGuard::PersistBlob;

void setUp() {}
void tearDown() {}

// Booted guard with no persisted state at t=bootS (hold-off applies).
static CompressorGuard freshGuard(uint32_t bootS, const CompressorGuard::Config& cfg = {}) {
  CompressorGuard g(cfg);
  g.bootRestore(nullptr, bootS, /*abnormalReset=*/false);
  return g;
}

// ---------- boot hold-off ----------

static void test_unbooted_guard_denies_start() {
  CompressorGuard g;
  auto d = g.requestStart(0);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kBootHoldoff, d.reason);
}

static void test_boot_unknown_state_full_holdoff() {
  CompressorGuard g = freshGuard(1000);
  auto d = g.requestStart(1000);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kBootHoldoff, d.reason);
  TEST_ASSERT_EQUAL_UINT32(dettson::kBootCompressorHoldoffS, d.waitS);

  d = g.requestStart(1200);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL_UINT32(100, d.waitS);

  d = g.requestStart(1300);
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_TRUE(g.running());
}

static void test_boot_holdoff_jitter_added() {
  CompressorGuard g;
  g.bootRestore(nullptr, 1000, false, /*jitterS=*/45);
  TEST_ASSERT_FALSE(g.requestStart(1320).allowed);  // 300 s alone not enough
  TEST_ASSERT_TRUE(g.requestStart(1345).allowed);
}

static void test_corrupt_blob_treated_as_unknown() {
  CompressorGuard a = freshGuard(0);
  TEST_ASSERT_TRUE(a.requestStart(300).allowed);
  TEST_ASSERT_TRUE(a.requestStop(700, /*safety=*/true).allowed);
  Blob b;
  a.save(&b);
  b.lastStopS ^= 0xFFu;  // corrupt: crc must catch it

  CompressorGuard g;
  g.bootRestore(&b, 800, false);
  auto d = g.requestStart(900);  // persisted timers discarded -> full hold-off
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kBootHoldoff, d.reason);
  TEST_ASSERT_EQUAL_UINT32(200, d.waitS);
  TEST_ASSERT_TRUE(g.requestStart(1100).allowed);
}

// ---------- min-off / min-on ----------

static void test_min_off_enforced_after_stop() {
  CompressorGuard g = freshGuard(0);
  TEST_ASSERT_TRUE(g.requestStart(300).allowed);
  TEST_ASSERT_TRUE(g.requestStop(1000, true).allowed);
  auto d = g.requestStart(1100);  // 100 s into the 180 s min-off
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kMinOff, d.reason);
  TEST_ASSERT_EQUAL_UINT32(80, d.waitS);
  TEST_ASSERT_FALSE(g.requestStart(1179).allowed);  // 1 s short
  TEST_ASSERT_TRUE(g.requestStart(1180).allowed);   // 180 s served
}

static void test_min_on_advisory_for_comfort_stop() {
  CompressorGuard g = freshGuard(0);
  TEST_ASSERT_TRUE(g.requestStart(300).allowed);
  auto d = g.requestStop(400, /*safety=*/false);  // 100 s on < 300 s min-on
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kMinOn, d.reason);
  TEST_ASSERT_EQUAL_UINT32(200, d.waitS);
  TEST_ASSERT_TRUE(g.running());  // comfort stop deferred, compressor stays on
  d = g.requestStop(600, false);  // min-on satisfied
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_FALSE(g.running());
}

static void test_safety_stop_always_allowed() {
  CompressorGuard g = freshGuard(0);
  TEST_ASSERT_TRUE(g.requestStart(300).allowed);
  auto d = g.requestStop(310, /*safety=*/true);  // 10 s into min-on
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_FALSE(g.running());
  // Stop while already stopped is also always allowed (idempotent).
  d = g.requestStop(320, false);
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kAlreadyInState, d.reason);
  // And the safety stop still charges min-off on the restart path.
  d = g.requestStart(400);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kMinOff, d.reason);
}

static void test_start_idempotent_while_running() {
  CompressorGuard g = freshGuard(0);
  TEST_ASSERT_TRUE(g.requestStart(300).allowed);
  auto d = g.requestStart(310);
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kAlreadyInState, d.reason);
  TEST_ASSERT_EQUAL_UINT32(1, g.startsInWindow(310));  // no extra start burned
}

// ---------- manual (user-initiated) min-off bypass ----------
// docs/04 §1a: min-OFF gates the AUTOMATIC control loop; a deliberate human
// setpoint/mode change should issue demand immediately. requestStart(now,
// manual=true) bypasses min-OFF ONLY — the ODU's own ~3-min restart delay is
// the physical backstop. Every other gate stays in force.

static void test_auto_min_off_default_is_180() {
  // The shipped default now matches the ODU's ~3-min restart delay.
  TEST_ASSERT_EQUAL_UINT32(180u, dettson::kCompressorMinOffS);
}

static void test_manual_bypasses_min_off_auto_does_not() {
  CompressorGuard g = freshGuard(0);
  TEST_ASSERT_TRUE(g.requestStart(300).allowed);
  TEST_ASSERT_TRUE(g.requestStop(1000, /*safety=*/true).allowed);

  // Same instant, 100 s into the 180 s min-off: an AUTOMATIC start is denied…
  auto autoD = g.requestStart(1100, /*manual=*/false);
  TEST_ASSERT_FALSE(autoD.allowed);
  TEST_ASSERT_EQUAL(Deny::kMinOff, autoD.reason);
  TEST_ASSERT_EQUAL_UINT32(80, autoD.waitS);
  TEST_ASSERT_FALSE(g.running());

  // …but a MANUAL (user-initiated) start goes out immediately.
  auto manD = g.requestStart(1100, /*manual=*/true);
  TEST_ASSERT_TRUE(manD.allowed);
  TEST_ASSERT_EQUAL(Deny::kNone, manD.reason);
  TEST_ASSERT_TRUE(g.running());
  TEST_ASSERT_EQUAL_UINT32(2, g.startsInWindow(1100));  // the manual start is still counted
}

static void test_manual_start_does_not_bypass_boot_holdoff() {
  // Boot hold-off is preserved for manual requests (only min-off is relaxed).
  CompressorGuard g = freshGuard(1000);
  auto d = g.requestStart(1000, /*manual=*/true);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kBootHoldoff, d.reason);
}

static void test_manual_start_does_not_bypass_lockout() {
  CompressorGuard::Config cfg;
  cfg.resetLoopCount = 1;  // latch on the first abnormal boot
  CompressorGuard g(cfg);
  g.bootRestore(nullptr, 0, /*abnormalReset=*/true);
  TEST_ASSERT_TRUE(g.lockedOut());
  auto d = g.requestStart(5000, /*manual=*/true);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kResetLoopLockout, d.reason);
}

static void test_manual_start_does_not_bypass_starts_per_hour() {
  // max-starts/hour still caps a manual (or oscillating) request.
  CompressorGuard::Config cfg;
  cfg.minOffS = 0;  // isolate the starts cap from min-off
  cfg.minOnS = 0;
  cfg.bootHoldoffS = 0;
  CompressorGuard g = freshGuard(0, cfg);
  TEST_ASSERT_TRUE(g.requestStart(0).allowed);
  TEST_ASSERT_TRUE(g.requestStop(5, true).allowed);
  TEST_ASSERT_TRUE(g.requestStart(10).allowed);
  TEST_ASSERT_TRUE(g.requestStop(15, true).allowed);
  TEST_ASSERT_TRUE(g.requestStart(20).allowed);
  TEST_ASSERT_TRUE(g.requestStop(25, true).allowed);
  auto d = g.requestStart(30, /*manual=*/true);  // 4th start, manual — still capped
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kStartsPerHour, d.reason);
}

static void test_safety_stop_immediate_after_manual_start() {
  CompressorGuard g = freshGuard(0);
  TEST_ASSERT_TRUE(g.requestStart(300).allowed);
  TEST_ASSERT_TRUE(g.requestStop(1000, /*safety=*/true).allowed);
  TEST_ASSERT_TRUE(g.requestStart(1100, /*manual=*/true).allowed);  // bypassed min-off
  auto d = g.requestStop(1105, /*safety=*/true);  // safety stop still always immediate
  TEST_ASSERT_TRUE(d.allowed);
  TEST_ASSERT_FALSE(g.running());
}

// ---------- starts-per-hour rolling window ----------

static void test_starts_per_hour_rolling_window() {
  CompressorGuard::Config cfg;
  cfg.minOffS = 0;
  cfg.minOnS = 0;
  cfg.bootHoldoffS = 0;
  CompressorGuard g = freshGuard(0, cfg);

  TEST_ASSERT_TRUE(g.requestStart(0).allowed);
  TEST_ASSERT_TRUE(g.requestStop(5, true).allowed);
  TEST_ASSERT_TRUE(g.requestStart(10).allowed);
  TEST_ASSERT_TRUE(g.requestStop(15, true).allowed);
  TEST_ASSERT_TRUE(g.requestStart(20).allowed);
  TEST_ASSERT_TRUE(g.requestStop(25, true).allowed);

  auto d = g.requestStart(30);  // 4th start within the hour
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kStartsPerHour, d.reason);
  TEST_ASSERT_EQUAL_UINT32(3570, d.waitS);  // first start (t=0) ages out at 3600

  TEST_ASSERT_FALSE(g.requestStart(3599).allowed);
  TEST_ASSERT_EQUAL_UINT32(3, g.startsInWindow(3599));
  TEST_ASSERT_TRUE(g.requestStart(3600).allowed);  // window rolled
}

// ---------- persistence ----------

static void test_min_off_survives_reboot_via_blob() {
  CompressorGuard a = freshGuard(0);
  TEST_ASSERT_TRUE(a.requestStart(300).allowed);
  TEST_ASSERT_TRUE(a.requestStop(700, true).allowed);
  Blob b;
  a.save(&b);

  CompressorGuard g;
  g.bootRestore(&b, 800, false);  // reboot 100 s into the 180 s min-off
  auto d = g.requestStart(800);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kMinOff, d.reason);
  TEST_ASSERT_EQUAL_UINT32(80, d.waitS);  // remainder, not a fresh 180+holdoff
  TEST_ASSERT_TRUE(g.requestStart(880).allowed);  // 700 stop + 180
}

static void test_reboot_while_running_assumes_stop_at_boot() {
  CompressorGuard a = freshGuard(0);
  TEST_ASSERT_TRUE(a.requestStart(300).allowed);
  Blob b;
  a.save(&b);  // died with the compressor running; stop time unknown

  CompressorGuard g;
  g.bootRestore(&b, 900, false);
  TEST_ASSERT_FALSE(g.running());  // boot = no demand
  auto d = g.requestStart(1000);   // full min-off measured from boot (900)
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kMinOff, d.reason);
  TEST_ASSERT_EQUAL_UINT32(80, d.waitS);
  TEST_ASSERT_TRUE(g.requestStart(1080).allowed);  // 900 assumed-stop + 180
}

static void test_starts_window_survives_reboot() {
  CompressorGuard::Config cfg;
  cfg.minOffS = 0;
  cfg.minOnS = 0;
  cfg.bootHoldoffS = 0;
  CompressorGuard a = freshGuard(0, cfg);
  a.requestStart(0);
  a.requestStop(5, true);
  a.requestStart(10);
  a.requestStop(15, true);
  a.requestStart(20);
  a.requestStop(25, true);
  Blob b;
  a.save(&b);

  CompressorGuard g(cfg);
  g.bootRestore(&b, 30, false);
  auto d = g.requestStart(40);  // 3 persisted starts still in the window
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kStartsPerHour, d.reason);
}

// Pre-TX gate proof (2026-07-11 review): an OTA reboot landing RIGHT AFTER a
// compressor stop must not permit an instant restart. Firmware chain: the
// stop edge saves the blob within one control cycle (persistOnChange on the
// running() change), the monotonic clock base ("clk") resumes from a value
// persisted every 60 s, and setup() feeds the blob to bootRestore() — so the
// restored `now` is never AHEAD of true elapsed time and min-off can only be
// over-served, never shorted. The two tests below pin both halves.

static void test_ota_reboot_immediately_after_stop_no_short_cycle() {
  CompressorGuard a = freshGuard(0);
  TEST_ASSERT_TRUE(a.requestStart(300).allowed);
  TEST_ASSERT_TRUE(a.requestStop(700, /*safety=*/false).allowed);  // normal comfort stop
  Blob b;
  a.save(&b);  // persisted on the stop edge, then the OTA reboot lands

  CompressorGuard g;
  g.bootRestore(&b, 705, false);  // back up 5 s after the stop
  auto d = g.requestStart(706);   // first demand post-boot (boot-to-no-demand)
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kMinOff, d.reason);
  TEST_ASSERT_EQUAL_UINT32(174, d.waitS);          // remainder of 180 from t=700
  TEST_ASSERT_FALSE(g.requestStart(879).allowed);  // 1 s short: still held
  TEST_ASSERT_TRUE(g.requestStart(880).allowed);   // min-off truly served
}

static void test_reboot_clock_behind_stop_serves_full_min_off() {
  // The 60 s clock-persist cadence means boot can resume BEHIND the last-stop
  // stamp. elapsedS() clamps backwards time to zero -> the FULL min-off is
  // served from the restored now, never a negative/underflowed wait.
  CompressorGuard a = freshGuard(0);
  TEST_ASSERT_TRUE(a.requestStart(300).allowed);
  TEST_ASSERT_TRUE(a.requestStop(1000, /*safety=*/true).allowed);
  Blob b;
  a.save(&b);

  CompressorGuard g;
  g.bootRestore(&b, 950, false);  // clock resumed 50 s behind the stop
  auto d = g.requestStart(950);
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kMinOff, d.reason);
  TEST_ASSERT_EQUAL_UINT32(dettson::kCompressorMinOffS, d.waitS);  // full 180
  TEST_ASSERT_FALSE(g.requestStart(1179).allowed);
  TEST_ASSERT_TRUE(g.requestStart(1180).allowed);  // 1000(blob stop) + 180
}

// ---------- reset-loop lockout ----------

static void test_reset_loop_latches_and_manual_clear() {
  // Three abnormal resets within 30 min, state persisted between boots.
  CompressorGuard g1;
  g1.bootRestore(nullptr, 0, /*abnormalReset=*/true);
  Blob b1;
  g1.save(&b1);

  CompressorGuard g2;
  g2.bootRestore(&b1, 600, true);
  TEST_ASSERT_FALSE(g2.lockedOut());
  Blob b2;
  g2.save(&b2);

  CompressorGuard g3;
  g3.bootRestore(&b2, 1200, true);
  TEST_ASSERT_TRUE(g3.lockedOut());
  auto d = g3.requestStart(5000);  // latched: time does not clear it
  TEST_ASSERT_FALSE(d.allowed);
  TEST_ASSERT_EQUAL(Deny::kResetLoopLockout, d.reason);
  TEST_ASSERT_EQUAL_UINT32(CompressorGuard::kForeverS, d.waitS);

  // Lockout persists across a further (normal) reboot.
  Blob b3;
  g3.save(&b3);
  CompressorGuard g4;
  g4.bootRestore(&b3, 4000, false);
  TEST_ASSERT_TRUE(g4.lockedOut());

  // Manual clear is the only way out; other timers stay in force.
  g4.manualClear();
  TEST_ASSERT_FALSE(g4.lockedOut());
  TEST_ASSERT_TRUE(g4.requestStart(5000).allowed);
}

static void test_reset_loop_window_and_normal_boots() {
  // Abnormal boots at 0 and 600; third at 2000 — the t=0 boot has aged out
  // of the 1800 s window, so only 2 count: no latch.
  CompressorGuard g1;
  g1.bootRestore(nullptr, 0, true);
  Blob b1;
  g1.save(&b1);
  CompressorGuard g2;
  g2.bootRestore(&b1, 600, true);
  Blob b2;
  g2.save(&b2);
  CompressorGuard g3;
  g3.bootRestore(&b2, 2000, true);
  TEST_ASSERT_FALSE(g3.lockedOut());

  // Normal (power-on) resets never count toward the latch.
  CompressorGuard h1;
  h1.bootRestore(nullptr, 0, false);
  Blob c1;
  h1.save(&c1);
  CompressorGuard h2;
  h2.bootRestore(&c1, 60, false);
  Blob c2;
  h2.save(&c2);
  CompressorGuard h3;
  h3.bootRestore(&c2, 120, false);
  TEST_ASSERT_FALSE(h3.lockedOut());
}

static void test_locked_out_still_allows_stop() {
  CompressorGuard::Config cfg;
  cfg.resetLoopCount = 1;  // latch on the first abnormal boot
  CompressorGuard g(cfg);
  g.bootRestore(nullptr, 0, true);
  TEST_ASSERT_TRUE(g.lockedOut());
  // NO-DEMAND latch blocks starts but never blocks shedding demand.
  TEST_ASSERT_FALSE(g.requestStart(1000).allowed);
  TEST_ASSERT_TRUE(g.requestStop(1000, true).allowed);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_unbooted_guard_denies_start);
  RUN_TEST(test_boot_unknown_state_full_holdoff);
  RUN_TEST(test_boot_holdoff_jitter_added);
  RUN_TEST(test_corrupt_blob_treated_as_unknown);
  RUN_TEST(test_min_off_enforced_after_stop);
  RUN_TEST(test_auto_min_off_default_is_180);
  RUN_TEST(test_manual_bypasses_min_off_auto_does_not);
  RUN_TEST(test_manual_start_does_not_bypass_boot_holdoff);
  RUN_TEST(test_manual_start_does_not_bypass_lockout);
  RUN_TEST(test_manual_start_does_not_bypass_starts_per_hour);
  RUN_TEST(test_safety_stop_immediate_after_manual_start);
  RUN_TEST(test_min_on_advisory_for_comfort_stop);
  RUN_TEST(test_safety_stop_always_allowed);
  RUN_TEST(test_start_idempotent_while_running);
  RUN_TEST(test_starts_per_hour_rolling_window);
  RUN_TEST(test_min_off_survives_reboot_via_blob);
  RUN_TEST(test_reboot_while_running_assumes_stop_at_boot);
  RUN_TEST(test_starts_window_survives_reboot);
  RUN_TEST(test_ota_reboot_immediately_after_stop_no_short_cycle);
  RUN_TEST(test_reboot_clock_behind_stop_serves_full_min_off);
  RUN_TEST(test_reset_loop_latches_and_manual_clear);
  RUN_TEST(test_reset_loop_window_and_normal_boots);
  RUN_TEST(test_locked_out_still_allows_stop);
  return UNITY_END();
}
