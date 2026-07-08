// RemoteLink tests (#102): parse the Controller's retained echo + status
// (fixtures include a payload captured verbatim from the live broker), build
// intent JSON byte-exact against the #104 parseRemoteIntentJson contract,
// and reject malformed/partial echoes whole (never a half-applied authority
// state).
#include <unity.h>

#include <cstring>

#include "RemoteLink.h"

using namespace remote_link;

void setUp() {}
void tearDown() {}

// ---------- echo parse ----------

static void test_parse_live_broker_fixture() {
  // Captured verbatim from the running Controller (2026-07-07, cid 8d82f4).
  const char* j =
      "{\"heatC\":19.2,\"coolC\":22,\"mode\":\"cool\",\"emHeat\":false,"
      "\"hold\":\"none\",\"holdRemainS\":0,\"activePreset\":\"none\","
      "\"fusedTempC\":22.8746,\"fusedTempValid\":true}";
  ControllerEcho e;
  TEST_ASSERT_TRUE(parseRemoteStateJson(j, e));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 19.2f, e.heatC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 22.0f, e.coolC);
  TEST_ASSERT_EQUAL(static_cast<int>(Mode::kCool), static_cast<int>(e.mode));
  TEST_ASSERT_FALSE(e.emHeat);
  TEST_ASSERT_EQUAL(static_cast<int>(Hold::kNone), static_cast<int>(e.hold));
  TEST_ASSERT_EQUAL_UINT32(0, e.holdRemainS);
  TEST_ASSERT_TRUE(e.activePreset.empty());  // "none" normalized to ""
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 22.8746f, e.fusedTempC);
  TEST_ASSERT_TRUE(e.fusedTempValid);
}

static void test_parse_full_featured_echo() {
  const char* j =
      "{\"heatC\":21.0,\"coolC\":25.0,\"mode\":\"heat_cool\",\"emHeat\":true,"
      "\"hold\":\"two_hours\",\"holdRemainS\":7032,\"activePreset\":\"home\","
      "\"fusedTempC\":21.3,\"fusedTempValid\":true}";
  ControllerEcho e;
  TEST_ASSERT_TRUE(parseRemoteStateJson(j, e));
  TEST_ASSERT_EQUAL(static_cast<int>(Mode::kHeatCool), static_cast<int>(e.mode));
  TEST_ASSERT_TRUE(e.emHeat);
  TEST_ASSERT_EQUAL(static_cast<int>(Hold::kTwoHours), static_cast<int>(e.hold));
  TEST_ASSERT_EQUAL_UINT32(7032, e.holdRemainS);
  TEST_ASSERT_EQUAL_STRING("home", e.activePreset.c_str());
}

static void test_parse_rejects_missing_field_whole() {
  // No "mode" — the WHOLE echo must be rejected, out untouched-empty.
  const char* j =
      "{\"heatC\":21.0,\"coolC\":25.0,\"emHeat\":false,"
      "\"hold\":\"none\",\"holdRemainS\":0,\"activePreset\":\"none\","
      "\"fusedTempC\":21.3,\"fusedTempValid\":true}";
  ControllerEcho e;
  e.heatC = 99.0f;
  TEST_ASSERT_FALSE(parseRemoteStateJson(j, e));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, e.heatC);  // reset, not half-applied
}

static void test_parse_rejects_bad_tokens() {
  ControllerEcho e;
  TEST_ASSERT_FALSE(parseRemoteStateJson(nullptr, e));
  TEST_ASSERT_FALSE(parseRemoteStateJson("", e));
  TEST_ASSERT_FALSE(parseRemoteStateJson("not json", e));
  TEST_ASSERT_FALSE(parseRemoteStateJson(
      "{\"heatC\":\"NaN\",\"coolC\":25,\"mode\":\"heat\",\"emHeat\":false,"
      "\"hold\":\"none\",\"holdRemainS\":0,\"activePreset\":\"none\","
      "\"fusedTempC\":21,\"fusedTempValid\":true}", e));
  TEST_ASSERT_FALSE(parseRemoteStateJson(  // unknown mode string
      "{\"heatC\":21,\"coolC\":25,\"mode\":\"emergency\",\"emHeat\":false,"
      "\"hold\":\"none\",\"holdRemainS\":0,\"activePreset\":\"none\","
      "\"fusedTempC\":21,\"fusedTempValid\":true}", e));
  TEST_ASSERT_FALSE(parseRemoteStateJson(  // unknown hold string
      "{\"heatC\":21,\"coolC\":25,\"mode\":\"heat\",\"emHeat\":false,"
      "\"hold\":\"forever\",\"holdRemainS\":0,\"activePreset\":\"none\","
      "\"fusedTempC\":21,\"fusedTempValid\":true}", e));
}

static void test_parse_echo_extras_and_old_format() {  // #116/#118
  // NEW-format echo: extras present.
  const char* j =
      "{\"heatC\":19.2,\"coolC\":22,\"mode\":\"cool\",\"emHeat\":false,"
      "\"hold\":\"none\",\"holdRemainS\":0,\"activePreset\":\"none\","
      "\"fusedTempC\":22.8,\"fusedTempValid\":true,"
      "\"action\":\"cooling\",\"equipment\":\"cool\",\"alarmN\":1,"
      "\"alarm1\":\"OAT stale\",\"vacation\":true,\"vacBanner\":\"Vacation until Jul 12\"}";
  ControllerEcho e;
  TEST_ASSERT_TRUE(parseRemoteStateJson(j, e));
  TEST_ASSERT_EQUAL_STRING("cooling", e.action.c_str());
  TEST_ASSERT_EQUAL_STRING("cool", e.equipment.c_str());
  TEST_ASSERT_EQUAL_UINT8(1, e.alarmN);
  TEST_ASSERT_EQUAL_STRING("OAT stale", e.alarm1.c_str());
  TEST_ASSERT_TRUE(e.vacationActive);
  TEST_ASSERT_EQUAL_STRING("Vacation until Jul 12", e.vacBanner.c_str());
  // OLD-format echo (pre-#116 Controller): extras absent -> defaults, still
  // parses — the mixed-version OTA-window guarantee.
  const char* old =
      "{\"heatC\":19.2,\"coolC\":22,\"mode\":\"cool\",\"emHeat\":false,"
      "\"hold\":\"none\",\"holdRemainS\":0,\"activePreset\":\"none\","
      "\"fusedTempC\":22.8,\"fusedTempValid\":true}";
  TEST_ASSERT_TRUE(parseRemoteStateJson(old, e));
  TEST_ASSERT_EQUAL_STRING("idle", e.action.c_str());
  TEST_ASSERT_EQUAL_UINT8(0, e.alarmN);
  TEST_ASSERT_FALSE(e.vacationActive);
}

static void test_parse_fusion_and_presence() {  // #117
  FusionView f;
  TEST_ASSERT_TRUE(parseFusionJson(
      "{\"temp\":22.87,\"tier\":\"fused_remotes\",\"participants\":[\"living\",\"bedroom\"],"
      "\"occupied\":false,\"dominant\":\"living\"}", f));
  TEST_ASSERT_EQUAL_UINT8(2, f.participantCount);
  TEST_ASSERT_EQUAL_STRING("living", f.participants[0]);
  TEST_ASSERT_EQUAL_STRING("bedroom", f.participants[1]);
  TEST_ASSERT_FALSE(f.occupied);
  TEST_ASSERT_EQUAL_STRING("living", f.dominant.c_str());
  // dominant optional (older Controller).
  TEST_ASSERT_TRUE(parseFusionJson(
      "{\"temp\":21.0,\"tier\":\"local\",\"participants\":[],\"occupied\":true}", f));
  TEST_ASSERT_EQUAL_UINT8(0, f.participantCount);
  TEST_ASSERT_TRUE(f.occupied);
  TEST_ASSERT_TRUE(f.dominant.empty());
  PresenceView pv;
  TEST_ASSERT_TRUE(parsePresenceJson("{\"occupied\":false, \"last_seen\":1783481508}", pv));
  TEST_ASSERT_FALSE(pv.occupied);
  TEST_ASSERT_EQUAL_UINT32(1783481508u, pv.lastSeenEpoch);
}

static void test_intent_vacation_ack_bytes() {  // #118 — byte-exact vs the #104 parser
  TEST_ASSERT_EQUAL_STRING(
      "{\"id\":21,\"type\":\"vacation\",\"startDays\":2,\"nights\":7,\"heatC\":16.0,\"coolC\":28.0}",
      intentVacationJson(21, 2, 7, 16.0f, 28.0f).c_str());
  TEST_ASSERT_EQUAL_STRING("{\"id\":22,\"type\":\"clear_vacation\"}",
                           intentClearVacationJson(22).c_str());
  TEST_ASSERT_EQUAL_STRING("{\"id\":23,\"type\":\"ack_alarms\"}",
                           intentAckAlarmsJson(23).c_str());
}

// ---------- controller status ----------

static void test_parse_controller_status() {
  ControllerStatus c;
  TEST_ASSERT_TRUE(parseControllerStatusJson(
      "{\"cid\":\"8d82f4\",\"status\":\"online\",\"version\":\"Jul  7 2026\"}", c));
  TEST_ASSERT_EQUAL_STRING("8d82f4", c.cid.c_str());
  TEST_ASSERT_TRUE(c.online);
  TEST_ASSERT_EQUAL_STRING("Jul  7 2026", c.version.c_str());
  TEST_ASSERT_FALSE(parseControllerStatusJson("{\"status\":\"online\"}", c));  // cid required
}

// ---------- intent builders (byte-exact vs the #104 parser's expectations) ----------

static void test_intent_setpoints_bytes() {
  TEST_ASSERT_EQUAL_STRING(
      "{\"id\":7,\"type\":\"setpoints\",\"heatC\":20.5,\"coolC\":24.0}",
      intentSetpointsJson(7, 20.5f, 24.0f).c_str());
}

static void test_intent_mode_bytes() {
  TEST_ASSERT_EQUAL_STRING("{\"id\":8,\"type\":\"mode\",\"mode\":\"heat_cool\"}",
                           intentModeJson(8, Mode::kHeatCool).c_str());
  TEST_ASSERT_EQUAL_STRING("{\"id\":9,\"type\":\"mode\",\"mode\":\"off\"}",
                           intentModeJson(9, Mode::kOff).c_str());
}

static void test_intent_preset_hold_clear_bytes() {
  TEST_ASSERT_EQUAL_STRING("{\"id\":10,\"type\":\"preset\",\"preset\":\"sleep\"}",
                           intentPresetJson(10, "sleep").c_str());
  TEST_ASSERT_EQUAL_STRING("{\"id\":11,\"type\":\"hold\",\"hold\":\"four_hours\"}",
                           intentHoldJson(11, Hold::kFourHours).c_str());
  TEST_ASSERT_EQUAL_STRING("{\"id\":12,\"type\":\"clear_hold\"}",
                           intentClearHoldJson(12).c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parse_live_broker_fixture);
  RUN_TEST(test_parse_full_featured_echo);
  RUN_TEST(test_parse_rejects_missing_field_whole);
  RUN_TEST(test_parse_rejects_bad_tokens);
  RUN_TEST(test_parse_controller_status);
  RUN_TEST(test_parse_echo_extras_and_old_format);
  RUN_TEST(test_parse_fusion_and_presence);
  RUN_TEST(test_intent_vacation_ack_bytes);
  RUN_TEST(test_intent_setpoints_bytes);
  RUN_TEST(test_intent_mode_bytes);
  RUN_TEST(test_intent_preset_hold_clear_bytes);
  return UNITY_END();
}
