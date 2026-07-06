// SleepState unit tests (issue #90): night-window enter/exit, touch wake +
// re-entry, clock fail-safe, away-wins gate, HA override, the away-timer
// freeze against the real SensorFusion presence ledger, and the reversible
// SleepPresetLink apply/restore.
#include <unity.h>

#include <cstring>

#include "ModeStateMachine.h"
#include "SensorFusion.h"
#include "SleepState.h"

using dettson::kPresenceAwayS;
using dettson::kSleepIdleS;
using dettson::ModeStateMachine;
using dettson::PresenceState;
using dettson::PresetDef;
using dettson::SensorFusion;
using dettson::SleepOverride;
using dettson::SleepPresetLink;
using dettson::SleepState;

namespace {
constexpr uint32_t kHour = 3600;

// Time helper: tests drive a fake clock where nowS grows and the local hour
// is derived as (nowS / 3600) % 24 with nowS==0 <=> 00:00.
int hourOf(uint32_t nowS) { return static_cast<int>((nowS / kHour) % 24u); }

// present-and-reporting update shortcut (no away gate in play).
bool tick(SleepState& s, uint32_t nowS) {
  return s.update(true, hourOf(nowS), /*present=*/true, /*anyReporting=*/true, nowS);
}
}  // namespace

void setUp() {}
void tearDown() {}

// ---------- window basics ----------

static void test_enters_at_window_start_and_exits_at_end() {
  SleepState s;                          // default 00:00-06:00
  TEST_ASSERT_FALSE(tick(s, 23 * kHour));            // 23:00 -> awake
  TEST_ASSERT_TRUE(tick(s, 24 * kHour));             // 00:00 -> asleep
  TEST_ASSERT_TRUE(tick(s, 24 * kHour + 5 * kHour + 3599));  // 05:59 -> still asleep
  TEST_ASSERT_FALSE(tick(s, 30 * kHour));            // 06:00 -> awake
  TEST_ASSERT_FALSE(tick(s, 35 * kHour));            // 11:00 -> awake
}

static void test_wrapping_window_22_to_6() {
  SleepState::Config c; c.startHour = 22; c.endHour = 6;
  SleepState s(c);
  TEST_ASSERT_FALSE(tick(s, 21 * kHour));  // 21:00
  TEST_ASSERT_TRUE(tick(s, 22 * kHour));   // 22:00
  TEST_ASSERT_TRUE(tick(s, 26 * kHour));   // 02:00 (wrapped)
  TEST_ASSERT_FALSE(tick(s, 30 * kHour));  // 06:00
}

static void test_no_clock_fails_safe_never_asleep_and_exits() {
  SleepState s;
  TEST_ASSERT_TRUE(tick(s, 25 * kHour));  // 01:00 asleep
  // Clock lost mid-night -> exit (same fail-safe as the #86 blank).
  TEST_ASSERT_FALSE(s.update(false, -1, true, true, 25 * kHour + 60));
  TEST_ASSERT_FALSE(s.asleep());
}

// ---------- touch wake + self re-arm ----------

static void test_touch_exits_sleep_and_reenters_after_idle() {
  SleepState s;
  TEST_ASSERT_TRUE(tick(s, 26 * kHour));           // 02:00 asleep
  s.noteTouch(26 * kHour + 100);                   // someone taps the panel
  TEST_ASSERT_FALSE(tick(s, 26 * kHour + 101));    // awake immediately
  // Still awake before the idle guard elapses...
  TEST_ASSERT_FALSE(tick(s, 26 * kHour + 100 + kSleepIdleS - 1));
  // ...and back asleep on its own after kSleepIdleS untouched (still night).
  TEST_ASSERT_TRUE(tick(s, 26 * kHour + 100 + kSleepIdleS));
}

static void test_touch_just_before_window_delays_entry() {
  SleepState s;
  s.noteTouch(24 * kHour - 600);                   // 23:50 touch
  TEST_ASSERT_FALSE(tick(s, 24 * kHour));          // 00:00: idle guard not met
  TEST_ASSERT_TRUE(tick(s, 24 * kHour - 600 + kSleepIdleS));  // 00:20 -> asleep
}

// ---------- presence gate ----------

static void test_away_blocks_entry_until_someone_comes_home() {
  SleepState s;
  // 00:30, presence sensors reporting "nobody home" -> Away wins, no Sleep.
  TEST_ASSERT_FALSE(s.update(true, 0, /*present=*/false, /*anyReporting=*/true,
                             24 * kHour + 1800));
  // 02:00 someone arrives -> Sleep engages.
  TEST_ASSERT_TRUE(s.update(true, 2, true, true, 26 * kHour));
}

static void test_no_presence_sensors_window_still_drives_sleep() {
  SleepState s;
  TEST_ASSERT_TRUE(s.update(true, 1, /*present=*/false, /*anyReporting=*/false,
                            25 * kHour));
}

// ---------- HA override ----------

static void test_override_forces_asleep_and_awake() {
  SleepState s;
  s.setOverride(SleepOverride::kForceAsleep);
  TEST_ASSERT_TRUE(tick(s, 12 * kHour));           // noon, forced asleep
  s.setOverride(SleepOverride::kForceAwake);
  TEST_ASSERT_FALSE(tick(s, 25 * kHour));          // 01:00, forced awake
  s.setOverride(SleepOverride::kAuto);
  TEST_ASSERT_TRUE(tick(s, 25 * kHour + 60));      // auto again -> window rules
}

// ---------- away-timer freeze against the real presence ledger ----------

static PresenceState creditedPresence(SensorFusion& f, const SleepState& s,
                                      uint32_t nowS) {
  PresenceState p = f.presence(nowS);
  const uint32_t credit = s.awayCreditS(p.lastSeenAgeS, nowS);
  if (credit > 0) p = f.presenceWithin(kPresenceAwayS + credit, nowS);
  return p;
}

static void test_presence_does_not_decay_to_away_while_asleep() {
  SensorFusion f;
  f.registerSensor(1);
  SleepState s;
  // Last motion 23:30 (t = 23.5 h), then everyone goes to bed.
  const uint32_t seenS = 23 * kHour + 1800;
  f.updatePresence(1, /*occupied=*/false, seenS, /*hasLastSeen=*/true, seenS);
  // 00:00: still present (30 min old) -> Sleep enters.
  PresenceState p = creditedPresence(f, s, 24 * kHour);
  TEST_ASSERT_TRUE(p.present);
  TEST_ASSERT_TRUE(s.update(true, 0, p.present, p.anyReporting, 24 * kHour));
  // 04:00: raw age is 4.5 h > 3 h — raw presence would say Away...
  TEST_ASSERT_FALSE(f.presence(28 * kHour).present);
  // ...but the credited evaluation holds Present all night.
  p = creditedPresence(f, s, 28 * kHour);
  TEST_ASSERT_TRUE(p.present);
  TEST_ASSERT_TRUE(s.update(true, 4, p.present, p.anyReporting, 28 * kHour));
}

static void test_away_timer_resumes_where_it_paused_after_wake() {
  SensorFusion f;
  f.registerSensor(1);
  SleepState s;
  const uint32_t seenS = 23 * kHour + 1800;  // last seen 23:30
  f.updatePresence(1, false, seenS, true, seenS);
  PresenceState p = creditedPresence(f, s, 24 * kHour);
  TEST_ASSERT_TRUE(s.update(true, 0, p.present, p.anyReporting, 24 * kHour));  // 00:00 asleep
  // 06:00 wake (window end).
  p = creditedPresence(f, s, 30 * kHour);
  TEST_ASSERT_FALSE(s.update(true, 6, p.present, p.anyReporting, 30 * kHour));
  // Effective age resumed at 30 min: still Present 2 h into the morning...
  TEST_ASSERT_TRUE(creditedPresence(f, s, 32 * kHour).present);
  // ...and Away once 3 h of AWAKE time have passed since the last sighting
  // (30 min before the window + 2.5 h after = 3 h at 08:30).
  TEST_ASSERT_FALSE(creditedPresence(f, s, 32 * kHour + 1800 + 1).present);
}

static void test_motion_during_sleep_keeps_presence_fresh() {
  SensorFusion f;
  f.registerSensor(1);
  SleepState s;
  f.updatePresence(1, false, 23 * kHour, true, 23 * kHour);
  PresenceState p = creditedPresence(f, s, 24 * kHour);
  TEST_ASSERT_TRUE(s.update(true, 0, p.present, p.anyReporting, 24 * kHour));
  // 02:00 bathroom trip trips a sensor.
  f.updatePresence(1, true, 26 * kHour, true, 26 * kHour);
  // 06:00 wake; presence anchored at 02:00 -> Present until ~05:00 awake-time.
  p = creditedPresence(f, s, 30 * kHour);
  TEST_ASSERT_FALSE(s.update(true, 6, p.present, p.anyReporting, 30 * kHour));
  TEST_ASSERT_TRUE(creditedPresence(f, s, 32 * kHour).present);   // 08:00
  TEST_ASSERT_FALSE(creditedPresence(f, s, 33 * kHour + 1).present);  // >09:00 (3h awake after 02:00... frozen 4h)
}

static void test_no_credit_when_never_slept_or_never_seen() {
  SleepState s;
  TEST_ASSERT_EQUAL_UINT32(0, s.awayCreditS(1000, 5000));          // never slept
  tick(s, 25 * kHour);                                             // asleep
  TEST_ASSERT_EQUAL_UINT32(0, s.awayCreditS(0xFFFFFFFFu, 26 * kHour));  // never seen
}

// ---------- SleepPresetLink: reversible sleep-preset drive ----------

static ModeStateMachine makeSm() {
  ModeStateMachine sm;
  PresetDef defs[3] = {};
  strcpy(defs[0].name, "home");  defs[0].heatC = 21.0f; defs[0].coolC = 25.0f;
  strcpy(defs[1].name, "away");  defs[1].heatC = 16.0f; defs[1].coolC = 28.0f;
  strcpy(defs[2].name, "sleep"); defs[2].heatC = 18.0f; defs[2].coolC = 26.0f;
  sm.setPresetRoster(defs, 3);
  return sm;
}

static void test_link_applies_sleep_preset_and_restores_previous() {
  ModeStateMachine sm = makeSm();
  SleepPresetLink link;
  sm.applyPreset("home", 1000);
  link.onEdge(true, sm, 24 * kHour);  // enter Sleep
  TEST_ASSERT_TRUE(link.applied());
  TEST_ASSERT_EQUAL_STRING("sleep", sm.activePreset());
  TEST_ASSERT_EQUAL_FLOAT(18.0f, sm.heatSetpoint());
  link.onEdge(false, sm, 30 * kHour);  // exit Sleep, untouched night
  TEST_ASSERT_EQUAL_STRING("home", sm.activePreset());
  TEST_ASSERT_EQUAL_FLOAT(21.0f, sm.heatSetpoint());
  TEST_ASSERT_EQUAL_FLOAT(25.0f, sm.coolSetpoint());
}

static void test_link_restores_raw_setpoints_when_no_preset_was_active() {
  ModeStateMachine sm = makeSm();
  SleepPresetLink link;
  sm.setHeatSetpoint(20.5f);  // manual pair, no active preset
  sm.setCoolSetpoint(24.5f);
  link.onEdge(true, sm, 24 * kHour);
  TEST_ASSERT_TRUE(link.applied());
  link.onEdge(false, sm, 30 * kHour);
  TEST_ASSERT_EQUAL_FLOAT(20.5f, sm.heatSetpoint());
  TEST_ASSERT_EQUAL_FLOAT(24.5f, sm.coolSetpoint());
  TEST_ASSERT_EQUAL_STRING("", sm.activePreset());  // label cleared, no hold
  TEST_ASSERT_EQUAL(static_cast<int>(dettson::HoldType::kNone),
                    static_cast<int>(sm.activeHoldType()));
}

static void test_link_overnight_user_change_wins_no_restore() {
  ModeStateMachine sm = makeSm();
  SleepPresetLink link;
  sm.applyPreset("home", 1000);
  link.onEdge(true, sm, 24 * kHour);
  // 03:00: user bumps the heat setpoint on the wall (manual-change setter).
  sm.setHeatSetpoint(20.0f, 27 * kHour);
  link.onEdge(false, sm, 30 * kHour);  // exit: intervention detected
  TEST_ASSERT_EQUAL_FLOAT(20.0f, sm.heatSetpoint());  // user's value kept
  TEST_ASSERT_NOT_EQUAL(0, strcmp(sm.activePreset(), "home"));  // not restored
}

static void test_link_blocked_by_hold_applies_and_restores_nothing() {
  ModeStateMachine sm = makeSm();
  SleepPresetLink link;
  sm.applyPreset("home", 1000);
  sm.startHold(dettson::HoldType::kIndefinite, 2000);
  const float h0 = sm.heatSetpoint(), c0 = sm.coolSetpoint();
  link.onEdge(true, sm, 24 * kHour);   // blocked by the hold
  TEST_ASSERT_FALSE(link.applied());
  TEST_ASSERT_EQUAL_FLOAT(h0, sm.heatSetpoint());
  link.onEdge(false, sm, 30 * kHour);  // nothing to undo
  TEST_ASSERT_EQUAL_FLOAT(h0, sm.heatSetpoint());
  TEST_ASSERT_EQUAL_FLOAT(c0, sm.coolSetpoint());
}

static void test_link_no_sleep_preset_in_roster_is_inert() {
  ModeStateMachine sm;
  PresetDef defs[1] = {};
  strcpy(defs[0].name, "home"); defs[0].heatC = 21.0f; defs[0].coolC = 25.0f;
  sm.setPresetRoster(defs, 1);
  sm.applyPreset("home", 1000);
  SleepPresetLink link;
  link.onEdge(true, sm, 24 * kHour);
  TEST_ASSERT_FALSE(link.applied());
  TEST_ASSERT_EQUAL_STRING("home", sm.activePreset());
  TEST_ASSERT_EQUAL_FLOAT(21.0f, sm.heatSetpoint());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_enters_at_window_start_and_exits_at_end);
  RUN_TEST(test_wrapping_window_22_to_6);
  RUN_TEST(test_no_clock_fails_safe_never_asleep_and_exits);
  RUN_TEST(test_touch_exits_sleep_and_reenters_after_idle);
  RUN_TEST(test_touch_just_before_window_delays_entry);
  RUN_TEST(test_away_blocks_entry_until_someone_comes_home);
  RUN_TEST(test_no_presence_sensors_window_still_drives_sleep);
  RUN_TEST(test_override_forces_asleep_and_awake);
  RUN_TEST(test_presence_does_not_decay_to_away_while_asleep);
  RUN_TEST(test_away_timer_resumes_where_it_paused_after_wake);
  RUN_TEST(test_motion_during_sleep_keeps_presence_fresh);
  RUN_TEST(test_no_credit_when_never_slept_or_never_seen);
  RUN_TEST(test_link_applies_sleep_preset_and_restores_previous);
  RUN_TEST(test_link_restores_raw_setpoints_when_no_preset_was_active);
  RUN_TEST(test_link_overnight_user_change_wins_no_restore);
  RUN_TEST(test_link_no_sleep_preset_in_roster_is_inert);
  RUN_TEST(test_link_blocked_by_hold_applies_and_restores_nothing);
  return UNITY_END();
}
