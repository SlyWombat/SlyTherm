// Unit tests for lib/HaMqtt — topic constants, discovery JSON builders,
// inbound command validation, and the remote-sensor JSON tolerance matrix
// (docs/06-home-assistant.md; docs/04: invalid inbound never mutates state).
#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "HaMqtt.h"

using namespace dettson::hamqtt;

void setUp() {}
void tearDown() {}

// ---------- helpers ----------

static bool has(const std::string& s, const char* sub) {
  return s.find(sub) != std::string::npos;
}

// Structural coherence: object/array nesting balanced outside strings, all
// strings terminated, no raw control characters, starts '{' ends '}'.
static void assertCoherentJson(const std::string& s) {
  TEST_ASSERT_FALSE(s.empty());
  TEST_ASSERT_EQUAL_CHAR('{', s.front());
  TEST_ASSERT_EQUAL_CHAR('}', s.back());
  int obj = 0, arr = 0;
  bool inStr = false, esc = false;
  for (char c : s) {
    if (inStr) {
      if (esc) {
        esc = false;
      } else if (c == '\\') {
        esc = true;
      } else if (c == '"') {
        inStr = false;
      } else {
        TEST_ASSERT_TRUE_MESSAGE(static_cast<unsigned char>(c) >= 0x20,
                                 "raw control char inside JSON string");
      }
    } else {
      if (c == '"') inStr = true;
      else if (c == '{') ++obj;
      else if (c == '}') --obj;
      else if (c == '[') ++arr;
      else if (c == ']') --arr;
      TEST_ASSERT_TRUE_MESSAGE(obj >= 0 && arr >= 0, "close before open");
    }
  }
  TEST_ASSERT_FALSE_MESSAGE(inStr, "unterminated string");
  TEST_ASSERT_FALSE_MESSAGE(esc, "dangling escape");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, obj, "unbalanced braces");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, arr, "unbalanced brackets");
}

// ---------- topics ----------

static void test_topic_map_matches_docs() {
  TEST_ASSERT_EQUAL_STRING("dettson/cmd/setpoint", topic::kCmdSetpoint);
  TEST_ASSERT_EQUAL_STRING("dettson/cmd/target_temp_low", topic::kCmdTargetTempLow);
  TEST_ASSERT_EQUAL_STRING("dettson/cmd/target_temp_high", topic::kCmdTargetTempHigh);
  TEST_ASSERT_EQUAL_STRING("dettson/cmd/mode", topic::kCmdMode);
  TEST_ASSERT_EQUAL_STRING("dettson/cmd/fan_mode", topic::kCmdFanMode);
  TEST_ASSERT_EQUAL_STRING("dettson/cmd/preset", topic::kCmdPreset);
  TEST_ASSERT_EQUAL_STRING("dettson/config/sensors", topic::kConfigSensors);
  TEST_ASSERT_EQUAL_STRING("dettson/state/current_temp", topic::kStateCurrentTemp);
  TEST_ASSERT_EQUAL_STRING("dettson/state/setpoint", topic::kStateSetpoint);
  TEST_ASSERT_EQUAL_STRING("dettson/state/target_temp_low", topic::kStateTargetTempLow);
  TEST_ASSERT_EQUAL_STRING("dettson/state/target_temp_high", topic::kStateTargetTempHigh);
  TEST_ASSERT_EQUAL_STRING("dettson/state/mode", topic::kStateMode);
  TEST_ASSERT_EQUAL_STRING("dettson/state/fan_mode", topic::kStateFanMode);
  TEST_ASSERT_EQUAL_STRING("dettson/state/preset", topic::kStatePreset);
  TEST_ASSERT_EQUAL_STRING("dettson/state/action", topic::kStateAction);
  TEST_ASSERT_EQUAL_STRING("dettson/state/active_equipment", topic::kStateActiveEquipment);
  TEST_ASSERT_EQUAL_STRING("dettson/state/modulation", topic::kStateModulation);
  TEST_ASSERT_EQUAL_STRING("dettson/state/outdoor_temp", topic::kStateOutdoorTemp);
  TEST_ASSERT_EQUAL_STRING("dettson/state/outdoor_source", topic::kStateOutdoorSource);
  TEST_ASSERT_EQUAL_STRING("dettson/state/fusion", topic::kStateFusion);
  TEST_ASSERT_EQUAL_STRING("dettson/state/compressor_min_off_remaining",
                           topic::kStateCompressorMinOffRemaining);
  TEST_ASSERT_EQUAL_STRING("dettson/state/compressor_locked_out",
                           topic::kStateCompressorLockedOut);
  TEST_ASSERT_EQUAL_STRING("dettson/state/changeover_reason", topic::kStateChangeoverReason);
  TEST_ASSERT_EQUAL_STRING("dettson/state/fault", topic::kStateFault);
  TEST_ASSERT_EQUAL_STRING("dettson/availability", topic::kAvailability);
}

static void test_topic_helpers() {
  TEST_ASSERT_EQUAL_STRING("dettson/sensors/kitchen/state",
                           sensorStateTopic("kitchen").c_str());
  TEST_ASSERT_EQUAL_STRING("dettson/state/sensor/kitchen/age",
                           sensorAgeStateTopic("kitchen").c_str());
  TEST_ASSERT_EQUAL_STRING("dettson/state/sensor/kitchen/participating",
                           sensorParticipatingStateTopic("kitchen").c_str());
  TEST_ASSERT_EQUAL_STRING("homeassistant/climate/dettson/hvac/config",
                           discoveryTopic("climate", "hvac").c_str());
}

// ---------- climate discovery ----------

static void test_climate_discovery_content() {
  std::string j = climateDiscoveryJson();
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"name\":\"Dettson HVAC\""));
  TEST_ASSERT_TRUE(has(j, "\"unique_id\":\"dettson_hvac\""));
  TEST_ASSERT_TRUE(has(j, "\"modes\":[\"off\",\"heat\",\"cool\",\"heat_cool\"]"));
  TEST_ASSERT_TRUE(has(j, "\"fan_modes\":[\"auto\",\"on\",\"circulate\"]"));
  TEST_ASSERT_TRUE(has(j, "\"preset_modes\":[\"home\",\"away\",\"sleep\"]"));
  TEST_ASSERT_TRUE(has(j, "\"min_temp\":10"));
  TEST_ASSERT_TRUE(has(j, "\"max_temp\":30"));
  TEST_ASSERT_TRUE(has(j, "\"temp_step\":0.5"));
  TEST_ASSERT_TRUE(has(j, "\"temperature_unit\":\"C\""));
  TEST_ASSERT_TRUE(has(j, "\"current_temperature_topic\":\"dettson/state/current_temp\""));
  TEST_ASSERT_TRUE(has(j, "\"temperature_command_topic\":\"dettson/cmd/setpoint\""));
  TEST_ASSERT_TRUE(has(j, "\"temperature_state_topic\":\"dettson/state/setpoint\""));
  TEST_ASSERT_TRUE(has(j, "\"temperature_low_command_topic\":\"dettson/cmd/target_temp_low\""));
  TEST_ASSERT_TRUE(has(j, "\"temperature_low_state_topic\":\"dettson/state/target_temp_low\""));
  TEST_ASSERT_TRUE(has(j, "\"temperature_high_command_topic\":\"dettson/cmd/target_temp_high\""));
  TEST_ASSERT_TRUE(has(j, "\"temperature_high_state_topic\":\"dettson/state/target_temp_high\""));
  TEST_ASSERT_TRUE(has(j, "\"mode_command_topic\":\"dettson/cmd/mode\""));
  TEST_ASSERT_TRUE(has(j, "\"mode_state_topic\":\"dettson/state/mode\""));
  TEST_ASSERT_TRUE(has(j, "\"fan_mode_command_topic\":\"dettson/cmd/fan_mode\""));
  TEST_ASSERT_TRUE(has(j, "\"preset_mode_command_topic\":\"dettson/cmd/preset\""));
  TEST_ASSERT_TRUE(has(j, "\"action_topic\":\"dettson/state/action\""));
  TEST_ASSERT_TRUE(has(j, "\"availability_topic\":\"dettson/availability\""));
  TEST_ASSERT_TRUE(has(j, "\"payload_available\":\"online\""));
  TEST_ASSERT_TRUE(has(j, "\"payload_not_available\":\"offline\""));
  TEST_ASSERT_TRUE(has(j, "\"device\":{\"identifiers\":[\"dettson_esp32\"]"));
  TEST_ASSERT_TRUE(has(j, "\"manufacturer\":\"ElectricRV\""));
  TEST_ASSERT_TRUE(has(j, "\"model\":\"ESP32-S3 CT-485\""));
}

// ---------- diagnostic entity discovery ----------

static void test_diagnostic_discovery_builders() {
  struct Case {
    std::string json;
    const char* uniqueId;
    const char* stateTopic;
    bool diagnostic;
    bool binary;
  };
  std::vector<Case> cases = {
      {activeEquipmentDiscoveryJson(), "dettson_active_equipment",
       "dettson/state/active_equipment", false, false},
      {modulationDiscoveryJson(), "dettson_modulation", "dettson/state/modulation",
       false, false},
      {blowerDiscoveryJson(), "dettson_blower", "dettson/state/blower", false, false},
      {faultDiscoveryJson(), "dettson_fault", "dettson/state/fault", false, false},
      {healthDiscoveryJson(), "dettson_health", "dettson/state/health", false, true},
      {lastErrorDiscoveryJson(), "dettson_last_error", "dettson/state/last_error",
       true, false},
      {compressorMinOffRemainingDiscoveryJson(),
       "dettson_compressor_min_off_remaining",
       "dettson/state/compressor_min_off_remaining", true, false},
      {compressorLockedOutDiscoveryJson(), "dettson_compressor_locked_out",
       "dettson/state/compressor_locked_out", true, true},
      {sensorAgeDiscoveryJson("kitchen"), "dettson_sensor_kitchen_age",
       "dettson/state/sensor/kitchen/age", true, false},
      {sensorParticipatingDiscoveryJson("kitchen"),
       "dettson_sensor_kitchen_participating",
       "dettson/state/sensor/kitchen/participating", true, true},
  };
  for (const Case& c : cases) {
    assertCoherentJson(c.json);
    std::string uid = std::string("\"unique_id\":\"") + c.uniqueId + "\"";
    std::string st = std::string("\"state_topic\":\"") + c.stateTopic + "\"";
    TEST_ASSERT_TRUE_MESSAGE(has(c.json, uid.c_str()), c.uniqueId);
    TEST_ASSERT_TRUE_MESSAGE(has(c.json, st.c_str()), c.uniqueId);
    TEST_ASSERT_TRUE_MESSAGE(
        has(c.json, "\"availability_topic\":\"dettson/availability\""), c.uniqueId);
    TEST_ASSERT_TRUE_MESSAGE(has(c.json, "\"device\":{"), c.uniqueId);
    TEST_ASSERT_EQUAL_MESSAGE(c.diagnostic,
                              has(c.json, "\"entity_category\":\"diagnostic\""),
                              c.uniqueId);
    TEST_ASSERT_EQUAL_MESSAGE(c.binary, has(c.json, "\"payload_on\":\"ON\""), c.uniqueId);
    TEST_ASSERT_EQUAL_MESSAGE(c.binary, has(c.json, "\"payload_off\":\"OFF\""), c.uniqueId);
  }
  TEST_ASSERT_TRUE(has(modulationDiscoveryJson(), "\"unit_of_measurement\":\"%\""));
  TEST_ASSERT_TRUE(has(compressorMinOffRemainingDiscoveryJson(),
                       "\"unit_of_measurement\":\"s\""));
  TEST_ASSERT_TRUE(has(healthDiscoveryJson(), "\"device_class\":\"problem\""));
}

// ---------- escaping ----------

static void test_json_escaping() {
  TEST_ASSERT_EQUAL_STRING("a\\\"b\\\\c", jsonEscape("a\"b\\c").c_str());
  TEST_ASSERT_EQUAL_STRING("line\\nbreak\\ttab", jsonEscape("line\nbreak\ttab").c_str());
  TEST_ASSERT_EQUAL_STRING("nul\\u0001ctl", jsonEscape("nul\x01" "ctl").c_str());

  // A hostile sensor id must come out escaped and structurally coherent.
  std::string j = sensorAgeDiscoveryJson("evil\"id\\x");
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "evil\\\"id\\\\x"));
  TEST_ASSERT_FALSE(has(j, "evil\"id"));  // no unescaped quote leaks through
}

// ---------- inbound setpoint validation ----------

static void test_parse_setpoint_accepts_valid() {
  Parsed<float> r = parseSetpoint("21.5");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.5f, r.value);

  r = parseSetpoint("  18 \n");  // surrounding whitespace tolerated
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 18.0f, r.value);

  TEST_ASSERT_TRUE(parseSetpoint("10").ok);   // inclusive bounds
  TEST_ASSERT_TRUE(parseSetpoint("30").ok);
  TEST_ASSERT_TRUE(parseSetpoint("-5", -20.0f, 0.0f).ok);  // custom range
}

static void test_parse_setpoint_rejects_invalid() {
  TEST_ASSERT_FALSE(parseSetpoint(nullptr).ok);
  TEST_ASSERT_FALSE(parseSetpoint("").ok);
  TEST_ASSERT_FALSE(parseSetpoint(" ").ok);
  TEST_ASSERT_FALSE(parseSetpoint("nan").ok);
  TEST_ASSERT_FALSE(parseSetpoint("NaN").ok);
  TEST_ASSERT_FALSE(parseSetpoint("inf").ok);
  TEST_ASSERT_FALSE(parseSetpoint("-inf").ok);
  TEST_ASSERT_FALSE(parseSetpoint("9.9").ok);    // below min_temp 10
  TEST_ASSERT_FALSE(parseSetpoint("30.1").ok);   // above max_temp 30
  TEST_ASSERT_FALSE(parseSetpoint("1e6").ok);    // numeric but absurd
  TEST_ASSERT_FALSE(parseSetpoint("abc").ok);
  TEST_ASSERT_FALSE(parseSetpoint("21.5abc").ok);  // trailing junk
  TEST_ASSERT_FALSE(parseSetpoint("21,5").ok);     // locale comma = trailing junk
  TEST_ASSERT_FALSE(parseSetpoint("{\"v\":21}").ok);
}

// ---------- inbound enum validation ----------

static void test_parse_mode() {
  TEST_ASSERT_TRUE(parseMode("off").ok);
  TEST_ASSERT_TRUE(parseMode("heat").ok);
  TEST_ASSERT_TRUE(parseMode("cool").ok);
  Parsed<Mode> r = parseMode("heat_cool");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.value == Mode::kHeatCool);

  TEST_ASSERT_FALSE(parseMode(nullptr).ok);
  TEST_ASSERT_FALSE(parseMode("").ok);
  TEST_ASSERT_FALSE(parseMode("Heat").ok);       // case-sensitive wire strings
  TEST_ASSERT_FALSE(parseMode("auto").ok);       // HA name for it is heat_cool
  TEST_ASSERT_FALSE(parseMode("heat_cool ").ok); // no trailing junk
  TEST_ASSERT_FALSE(parseMode("emergency").ok);
}

static void test_parse_fan_mode() {
  Parsed<FanMode> r = parseFanMode("circulate");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.value == FanMode::kCirculate);
  TEST_ASSERT_TRUE(parseFanMode("auto").ok);
  TEST_ASSERT_TRUE(parseFanMode("on").ok);
  TEST_ASSERT_FALSE(parseFanMode("off").ok);
  TEST_ASSERT_FALSE(parseFanMode("ON").ok);
  TEST_ASSERT_FALSE(parseFanMode(nullptr).ok);
}

static void test_parse_preset() {
  Parsed<Preset> r = parsePreset("sleep");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.value == Preset::kSleep);
  TEST_ASSERT_TRUE(parsePreset("home").ok);
  TEST_ASSERT_TRUE(parsePreset("away").ok);
  TEST_ASSERT_FALSE(parsePreset("vacation").ok);
  TEST_ASSERT_FALSE(parsePreset("Home").ok);
  TEST_ASSERT_FALSE(parsePreset(nullptr).ok);
}

static void test_enum_round_trip() {
  TEST_ASSERT_TRUE(parseMode(toString(Mode::kHeatCool)).value == Mode::kHeatCool);
  TEST_ASSERT_TRUE(parseFanMode(toString(FanMode::kCirculate)).value == FanMode::kCirculate);
  TEST_ASSERT_TRUE(parsePreset(toString(Preset::kAway)).value == Preset::kAway);
}

// ---------- sensor JSON tolerance matrix ----------

static void test_sensor_json_full_payload() {
  SensorReading r;
  TEST_ASSERT_TRUE(parseSensorJson(
      "{\"temp\":21.5,\"occ\":true,\"bat\":87,\"hum\":45.5}", r));
  TEST_ASSERT_TRUE(r.hasTemp);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.5f, r.tempC);
  TEST_ASSERT_TRUE(r.hasOcc);
  TEST_ASSERT_TRUE(r.occupied);
  TEST_ASSERT_TRUE(r.hasBat);
  TEST_ASSERT_EQUAL_UINT8(87, r.batteryPct);
  TEST_ASSERT_TRUE(r.hasHum);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 45.5f, r.humidityPct);
}

static void test_sensor_json_missing_and_null_fields_tolerated() {
  SensorReading r;
  TEST_ASSERT_TRUE(parseSensorJson("{\"temp\":19.0}", r));
  TEST_ASSERT_TRUE(r.hasTemp);
  TEST_ASSERT_FALSE(r.hasOcc);
  TEST_ASSERT_FALSE(r.hasBat);
  TEST_ASSERT_FALSE(r.hasHum);

  TEST_ASSERT_TRUE(parseSensorJson(
      "{\"temp\":19.0,\"occ\":null,\"bat\":null,\"hum\":null}", r));
  TEST_ASSERT_TRUE(r.hasTemp);
  TEST_ASSERT_FALSE(r.hasOcc);
  TEST_ASSERT_FALSE(r.hasBat);
  TEST_ASSERT_FALSE(r.hasHum);

  // occ:false is a present value, not an absent one.
  TEST_ASSERT_TRUE(parseSensorJson("{\"temp\":19.0,\"occ\":false}", r));
  TEST_ASSERT_TRUE(r.hasOcc);
  TEST_ASSERT_FALSE(r.occupied);

  // Whitespace and key order variations.
  TEST_ASSERT_TRUE(parseSensorJson(
      " {\n  \"occ\" : true ,\n  \"temp\" : -12.5\n} ", r));
  TEST_ASSERT_TRUE(r.hasTemp);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -12.5f, r.tempC);
  TEST_ASSERT_TRUE(r.occupied);

  // Unknown extra keys ignored.
  TEST_ASSERT_TRUE(parseSensorJson(
      "{\"temp\":20.0,\"rssi\":-70,\"name\":\"kitchen\"}", r));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, r.tempC);
}

static void test_sensor_json_invalid_temp_rejected() {
  SensorReading r;
  TEST_ASSERT_FALSE(parseSensorJson(nullptr, r));
  TEST_ASSERT_FALSE(parseSensorJson("", r));
  TEST_ASSERT_FALSE(parseSensorJson("not json", r));
  TEST_ASSERT_FALSE(parseSensorJson("{}", r));
  TEST_ASSERT_FALSE(parseSensorJson("{\"occ\":true}", r));            // no temp
  TEST_ASSERT_FALSE(parseSensorJson("{\"temp\":null}", r));
  TEST_ASSERT_FALSE(parseSensorJson("{\"temp\":\"21.5\"}", r));       // string, not number
  TEST_ASSERT_FALSE(parseSensorJson("{\"temp\":true}", r));
  TEST_ASSERT_FALSE(parseSensorJson("{\"temp\":21.5junk}", r));       // dirty token
  TEST_ASSERT_FALSE(parseSensorJson("\"temp\":21.5", r));             // not an object
  // Rejection must leave the output zeroed — never a half-applied reading.
  TEST_ASSERT_FALSE(r.hasTemp);
  TEST_ASSERT_FALSE(r.hasOcc);
  TEST_ASSERT_FALSE(r.hasBat);
  TEST_ASSERT_FALSE(r.hasHum);
}

static void test_sensor_json_bad_optional_fields_dropped_not_fatal() {
  SensorReading r;
  // bat out of contract range 0-100 -> field absent, reading still usable.
  TEST_ASSERT_TRUE(parseSensorJson("{\"temp\":21.0,\"bat\":150}", r));
  TEST_ASSERT_TRUE(r.hasTemp);
  TEST_ASSERT_FALSE(r.hasBat);
  TEST_ASSERT_TRUE(parseSensorJson("{\"temp\":21.0,\"bat\":-5}", r));
  TEST_ASSERT_FALSE(r.hasBat);
  // Non-boolean occ -> absent.
  TEST_ASSERT_TRUE(parseSensorJson("{\"temp\":21.0,\"occ\":\"yes\"}", r));
  TEST_ASSERT_FALSE(r.hasOcc);
  TEST_ASSERT_TRUE(parseSensorJson("{\"temp\":21.0,\"occ\":1}", r));
  TEST_ASSERT_FALSE(r.hasOcc);
  // Out-of-range hum -> absent.
  TEST_ASSERT_TRUE(parseSensorJson("{\"temp\":21.0,\"hum\":120}", r));
  TEST_ASSERT_FALSE(r.hasHum);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_topic_map_matches_docs);
  RUN_TEST(test_topic_helpers);
  RUN_TEST(test_climate_discovery_content);
  RUN_TEST(test_diagnostic_discovery_builders);
  RUN_TEST(test_json_escaping);
  RUN_TEST(test_parse_setpoint_accepts_valid);
  RUN_TEST(test_parse_setpoint_rejects_invalid);
  RUN_TEST(test_parse_mode);
  RUN_TEST(test_parse_fan_mode);
  RUN_TEST(test_parse_preset);
  RUN_TEST(test_enum_round_trip);
  RUN_TEST(test_sensor_json_full_payload);
  RUN_TEST(test_sensor_json_missing_and_null_fields_tolerated);
  RUN_TEST(test_sensor_json_invalid_temp_rejected);
  RUN_TEST(test_sensor_json_bad_optional_fields_dropped_not_fatal);
  return UNITY_END();
}
