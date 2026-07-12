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
  TEST_ASSERT_EQUAL_STRING("slytherm/cmd/setpoint", topic::kCmdSetpoint);
  TEST_ASSERT_EQUAL_STRING("slytherm/cmd/target_temp_low", topic::kCmdTargetTempLow);
  TEST_ASSERT_EQUAL_STRING("slytherm/cmd/target_temp_high", topic::kCmdTargetTempHigh);
  TEST_ASSERT_EQUAL_STRING("slytherm/cmd/mode", topic::kCmdMode);
  TEST_ASSERT_EQUAL_STRING("slytherm/cmd/fan_mode", topic::kCmdFanMode);
  TEST_ASSERT_EQUAL_STRING("slytherm/cmd/preset", topic::kCmdPreset);
  TEST_ASSERT_EQUAL_STRING("slytherm/cmd/hold", topic::kCmdHold);
  TEST_ASSERT_EQUAL_STRING("slytherm/config/sensors", topic::kConfigSensors);
  TEST_ASSERT_EQUAL_STRING("slytherm/config/presets", topic::kConfigPresets);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/hold", topic::kStateHold);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/current_temp", topic::kStateCurrentTemp);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/setpoint", topic::kStateSetpoint);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/target_temp_low", topic::kStateTargetTempLow);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/target_temp_high", topic::kStateTargetTempHigh);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/mode", topic::kStateMode);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/fan_mode", topic::kStateFanMode);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/preset", topic::kStatePreset);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/action", topic::kStateAction);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/active_equipment", topic::kStateActiveEquipment);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/modulation", topic::kStateModulation);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/outdoor_temp", topic::kStateOutdoorTemp);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/outdoor_source", topic::kStateOutdoorSource);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/fusion", topic::kStateFusion);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/compressor_min_off_remaining",
                           topic::kStateCompressorMinOffRemaining);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/compressor_locked_out",
                           topic::kStateCompressorLockedOut);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/changeover_reason", topic::kStateChangeoverReason);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/fault", topic::kStateFault);
  TEST_ASSERT_EQUAL_STRING("slytherm/availability", topic::kAvailability);
}

static void test_topic_helpers() {
  TEST_ASSERT_EQUAL_STRING("slytherm/sensors/kitchen/state",
                           sensorStateTopic("kitchen").c_str());
  TEST_ASSERT_EQUAL_STRING("slytherm/state/sensor/kitchen/age",
                           sensorAgeStateTopic("kitchen").c_str());
  TEST_ASSERT_EQUAL_STRING("slytherm/state/sensor/kitchen/participating",
                           sensorParticipatingStateTopic("kitchen").c_str());
  TEST_ASSERT_EQUAL_STRING("homeassistant/climate/slytherm/hvac/config",
                           discoveryTopic("climate", "hvac").c_str());
}

// ---------- climate discovery ----------

static void test_climate_discovery_content() {
  std::string j = climateDiscoveryJson();
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"name\":\"SlyTherm HVAC\""));
  TEST_ASSERT_TRUE(has(j, "\"unique_id\":\"slytherm_hvac\""));
  TEST_ASSERT_TRUE(has(j, "\"modes\":[\"off\",\"heat\",\"cool\",\"heat_cool\"]"));
  TEST_ASSERT_TRUE(has(j, "\"fan_modes\":[\"auto\",\"on\",\"circulate\"]"));
  TEST_ASSERT_TRUE(has(j, "\"preset_modes\":[\"home\",\"away\",\"sleep\"]"));
  TEST_ASSERT_TRUE(has(j, "\"min_temp\":10"));
  TEST_ASSERT_TRUE(has(j, "\"max_temp\":30"));
  TEST_ASSERT_TRUE(has(j, "\"temp_step\":0.5"));
  TEST_ASSERT_TRUE(has(j, "\"temperature_unit\":\"C\""));
  TEST_ASSERT_TRUE(has(j, "\"current_temperature_topic\":\"slytherm/state/current_temp\""));
  TEST_ASSERT_TRUE(has(j, "\"temperature_command_topic\":\"slytherm/cmd/setpoint\""));
  TEST_ASSERT_TRUE(has(j, "\"temperature_state_topic\":\"slytherm/state/setpoint\""));
  TEST_ASSERT_TRUE(has(j, "\"temperature_low_command_topic\":\"slytherm/cmd/target_temp_low\""));
  TEST_ASSERT_TRUE(has(j, "\"temperature_low_state_topic\":\"slytherm/state/target_temp_low\""));
  TEST_ASSERT_TRUE(has(j, "\"temperature_high_command_topic\":\"slytherm/cmd/target_temp_high\""));
  TEST_ASSERT_TRUE(has(j, "\"temperature_high_state_topic\":\"slytherm/state/target_temp_high\""));
  TEST_ASSERT_TRUE(has(j, "\"mode_command_topic\":\"slytherm/cmd/mode\""));
  TEST_ASSERT_TRUE(has(j, "\"mode_state_topic\":\"slytherm/state/mode\""));
  TEST_ASSERT_TRUE(has(j, "\"fan_mode_command_topic\":\"slytherm/cmd/fan_mode\""));
  TEST_ASSERT_TRUE(has(j, "\"preset_mode_command_topic\":\"slytherm/cmd/preset\""));
  TEST_ASSERT_TRUE(has(j, "\"action_topic\":\"slytherm/state/action\""));
  TEST_ASSERT_TRUE(has(j, "\"availability_topic\":\"slytherm/availability\""));
  TEST_ASSERT_TRUE(has(j, "\"payload_available\":\"online\""));
  TEST_ASSERT_TRUE(has(j, "\"payload_not_available\":\"offline\""));
  TEST_ASSERT_TRUE(has(j, "\"device\":{\"identifiers\":[\"slytherm_esp32\"]"));
  TEST_ASSERT_TRUE(has(j, "\"manufacturer\":\"ElectricRV\""));
  TEST_ASSERT_TRUE(has(j, "\"model\":\"ESP32-S3 CT-485\""));
  // #113: device block carries the firmware version (host builds see the
  // SLYTHERM_FW_VERSION fallback; firmware builds get the injected value).
  TEST_ASSERT_TRUE(has(j, "\"sw_version\":\""));
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
      {activeEquipmentDiscoveryJson(), "slytherm_active_equipment",
       "slytherm/state/active_equipment", false, false},
      {modulationDiscoveryJson(), "slytherm_modulation", "slytherm/state/modulation",
       false, false},
      {blowerDiscoveryJson(), "slytherm_blower", "slytherm/state/blower", false, false},
      {faultDiscoveryJson(), "slytherm_fault", "slytherm/state/fault", false, false},
      {healthDiscoveryJson(), "slytherm_health", "slytherm/state/health", false, true},
      {lastErrorDiscoveryJson(), "slytherm_last_error", "slytherm/state/last_error",
       true, false},
      {compressorMinOffRemainingDiscoveryJson(),
       "slytherm_compressor_min_off_remaining",
       "slytherm/state/compressor_min_off_remaining", true, false},
      {compressorLockedOutDiscoveryJson(), "slytherm_compressor_locked_out",
       "slytherm/state/compressor_locked_out", true, true},
      {sensorAgeDiscoveryJson("kitchen"), "slytherm_sensor_kitchen_age",
       "slytherm/state/sensor/kitchen/age", true, false},
      {sensorParticipatingDiscoveryJson("kitchen"),
       "slytherm_sensor_kitchen_participating",
       "slytherm/state/sensor/kitchen/participating", true, true},
  };
  for (const Case& c : cases) {
    assertCoherentJson(c.json);
    std::string uid = std::string("\"unique_id\":\"") + c.uniqueId + "\"";
    std::string st = std::string("\"state_topic\":\"") + c.stateTopic + "\"";
    TEST_ASSERT_TRUE_MESSAGE(has(c.json, uid.c_str()), c.uniqueId);
    TEST_ASSERT_TRUE_MESSAGE(has(c.json, st.c_str()), c.uniqueId);
    TEST_ASSERT_TRUE_MESSAGE(
        has(c.json, "\"availability_topic\":\"slytherm/availability\""), c.uniqueId);
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

// Presets have no fixed enum/parser: slytherm/cmd/preset strings are validated
// against the configured roster by ModeStateMachine::applyPreset (see HaMqtt.h).

static void test_enum_round_trip() {
  TEST_ASSERT_TRUE(parseMode(toString(Mode::kHeatCool)).value == Mode::kHeatCool);
  TEST_ASSERT_TRUE(parseFanMode(toString(FanMode::kCirculate)).value == FanMode::kCirculate);
}

// ---------- hold command parsing (G4) ----------

static void test_parse_hold_command_accepts_types_and_clear() {
  using dettson::HoldType;
  Parsed<HoldCommand> r = parseHoldCommand("until_next_preset");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_FALSE(r.value.clear);
  TEST_ASSERT_TRUE(r.value.type == HoldType::kUntilNextPreset);
  TEST_ASSERT_TRUE(parseHoldCommand("two_hours").value.type == HoldType::kTwoHours);
  TEST_ASSERT_TRUE(parseHoldCommand("four_hours").value.type == HoldType::kFourHours);
  TEST_ASSERT_TRUE(parseHoldCommand("indefinite").value.type == HoldType::kIndefinite);
  r = parseHoldCommand("clear");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.value.clear);
}

static void test_parse_hold_command_rejects_junk() {
  TEST_ASSERT_FALSE(parseHoldCommand(nullptr).ok);
  TEST_ASSERT_FALSE(parseHoldCommand("").ok);
  TEST_ASSERT_FALSE(parseHoldCommand("none").ok);   // state-only string
  TEST_ASSERT_FALSE(parseHoldCommand("Two_Hours").ok);
  TEST_ASSERT_FALSE(parseHoldCommand("2h").ok);
  TEST_ASSERT_FALSE(parseHoldCommand("clear ").ok);  // no trailing junk
  TEST_ASSERT_FALSE(parseHoldCommand("hold").ok);
}

static void test_hold_state_json_and_round_trip() {
  using dettson::HoldType;
  std::string j = holdStateJson(HoldType::kTwoHours, 7032);
  assertCoherentJson(j);
  TEST_ASSERT_EQUAL_STRING("{\"type\":\"two_hours\",\"remaining\":7032}", j.c_str());
  TEST_ASSERT_EQUAL_STRING("{\"type\":\"none\",\"remaining\":0}",
                           holdStateJson(HoldType::kNone, 0).c_str());
  // Wire strings round-trip through the command parser (except "none").
  TEST_ASSERT_TRUE(parseHoldCommand(toString(HoldType::kFourHours)).value.type ==
                   HoldType::kFourHours);
}

static void test_hold_discovery_json() {
  std::string j = holdDiscoveryJson();
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"unique_id\":\"slytherm_hold\""));
  TEST_ASSERT_TRUE(has(j, "\"state_topic\":\"slytherm/state/hold\""));
  TEST_ASSERT_TRUE(has(j, "\"value_template\":\"{{ value_json.type }}\""));
  TEST_ASSERT_TRUE(has(j, "\"json_attributes_topic\":\"slytherm/state/hold\""));
  TEST_ASSERT_TRUE(has(j, "\"entity_category\":\"diagnostic\""));
}

// ---------- EM HEAT switch (G15) ----------

static void test_em_heat_topics() {
  TEST_ASSERT_EQUAL_STRING("slytherm/cmd/em_heat", topic::kCmdEmHeat);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/em_heat", topic::kStateEmHeat);
}

static void test_parse_em_heat_command() {
  auto r = parseEmHeatCommand("ON");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.value);
  r = parseEmHeatCommand("OFF");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_FALSE(r.value);
  // Strict: HA switch default payloads only — rejection never mutates state.
  TEST_ASSERT_FALSE(parseEmHeatCommand(nullptr).ok);
  TEST_ASSERT_FALSE(parseEmHeatCommand("").ok);
  TEST_ASSERT_FALSE(parseEmHeatCommand("on").ok);
  TEST_ASSERT_FALSE(parseEmHeatCommand("off").ok);
  TEST_ASSERT_FALSE(parseEmHeatCommand("ON ").ok);
  TEST_ASSERT_FALSE(parseEmHeatCommand("1").ok);
  TEST_ASSERT_FALSE(parseEmHeatCommand("emergency_heat").ok);
}

static void test_em_heat_discovery_json_and_not_a_mode_or_preset() {
  std::string j = emHeatDiscoveryJson();
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"unique_id\":\"slytherm_em_heat\""));
  TEST_ASSERT_TRUE(has(j, "\"command_topic\":\"slytherm/cmd/em_heat\""));
  TEST_ASSERT_TRUE(has(j, "\"state_topic\":\"slytherm/state/em_heat\""));
  TEST_ASSERT_TRUE(has(j, "\"payload_on\":\"ON\""));
  TEST_ASSERT_TRUE(has(j, "\"payload_off\":\"OFF\""));
  TEST_ASSERT_TRUE(has(j, "\"availability_topic\":\"slytherm/availability\""));
  // Mutual exclusion with comfort presets is structural: EM HEAT never
  // appears as an hvac mode or in preset_modes (docs/06).
  std::string climate = climateDiscoveryJson();
  TEST_ASSERT_FALSE(has(climate, "emergency_heat"));
  TEST_ASSERT_FALSE(has(climate, "em_heat"));
}

// ---------- preset roster config + dynamic discovery (G4) ----------

static void test_preset_roster_json_round_trip() {
  std::vector<PresetEntry> out;
  TEST_ASSERT_TRUE(parsePresetRosterJson(
      "{\"presets\":[{\"name\":\"home\",\"heat\":21,\"cool\":25},"
      "{\"name\":\"movie night\",\"heat\":20.5,\"cool\":24.5}]}", out));
  TEST_ASSERT_EQUAL(2, out.size());
  TEST_ASSERT_EQUAL_STRING("home", out[0].name.c_str());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f, out[0].heatC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, out[0].coolC);
  TEST_ASSERT_EQUAL_STRING("movie night", out[1].name.c_str());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 24.5f, out[1].coolC);

  // Whitespace, key order, unknown extra keys tolerated; names feed straight
  // into the dynamic discovery list.
  TEST_ASSERT_TRUE(parsePresetRosterJson(
      " {\n \"presets\" : [ { \"cool\" : 28 , \"heat\" : 16 ,"
      " \"name\" : \"away\" , \"icon\" : \"mdi:leaf\" } ]\n} ", out));
  TEST_ASSERT_EQUAL(1, out.size());
  std::string j = climateDiscoveryJson({out[0].name});
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"preset_modes\":[\"away\"]"));

  // Empty roster is valid (clears the presets).
  TEST_ASSERT_TRUE(parsePresetRosterJson("{\"presets\":[]}", out));
  TEST_ASSERT_EQUAL(0, out.size());
}

static void test_preset_roster_json_rejects_structural_junk() {
  std::vector<PresetEntry> out;
  TEST_ASSERT_FALSE(parsePresetRosterJson(nullptr, out));
  TEST_ASSERT_FALSE(parsePresetRosterJson("", out));
  TEST_ASSERT_FALSE(parsePresetRosterJson("not json", out));
  TEST_ASSERT_FALSE(parsePresetRosterJson("{}", out));
  TEST_ASSERT_FALSE(parsePresetRosterJson("{\"presets\":42}", out));
  TEST_ASSERT_FALSE(parsePresetRosterJson("{\"presets\":[42]}", out));
  TEST_ASSERT_FALSE(parsePresetRosterJson(
      "{\"presets\":[{\"name\":\"x\",\"heat\":21,\"cool\":25}", out));  // unterminated
  TEST_ASSERT_EQUAL(0, out.size());  // rejection leaves nothing half-applied
}

static void test_preset_roster_json_skips_invalid_entries_and_caps() {
  std::vector<PresetEntry> out;
  TEST_ASSERT_TRUE(parsePresetRosterJson(
      "{\"presets\":["
      "{\"heat\":21,\"cool\":25},"                                   // no name
      "{\"name\":\"\",\"heat\":21,\"cool\":25},"                     // empty name
      "{\"name\":\"hot\",\"heat\":35,\"cool\":36},"                  // heat > climate max 30
      "{\"name\":\"nocool\",\"heat\":21},"                           // missing cool
      "{\"name\":\"ok\",\"heat\":21,\"cool\":25},"
      "{\"name\":\"ok\",\"heat\":18,\"cool\":26},"                   // duplicate
      "{\"name\":\"a-name-longer-than-23-chars\",\"heat\":21,\"cool\":25}"
      "]}", out));
  TEST_ASSERT_EQUAL(1, out.size());
  TEST_ASSERT_EQUAL_STRING("ok", out[0].name.c_str());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f, out[0].heatC);  // first def wins

  // 10 valid entries -> capped at kMaxPresets (8).
  std::string big = "{\"presets\":[";
  for (int i = 0; i < 10; ++i) {
    if (i) big += ',';
    big += "{\"name\":\"p" + std::to_string(i) + "\",\"heat\":20,\"cool\":25}";
  }
  big += "]}";
  TEST_ASSERT_TRUE(parsePresetRosterJson(big.c_str(), out));
  TEST_ASSERT_EQUAL(dettson::kMaxPresets, out.size());
}

// ---------- sensor calibration offsets (issue #49, gap G6) ----------

static void test_sensor_offset_topics_and_discovery() {
  TEST_ASSERT_EQUAL_STRING("slytherm/cmd/sensor/kitchen/offset",
                           sensorOffsetCommandTopic("kitchen").c_str());
  TEST_ASSERT_EQUAL_STRING("slytherm/state/sensor/kitchen/offset",
                           sensorOffsetStateTopic("kitchen").c_str());
  TEST_ASSERT_EQUAL_STRING("local", kLocalSensorId);

  std::string j = sensorOffsetDiscoveryJson("kitchen");
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"unique_id\":\"slytherm_sensor_kitchen_offset\""));
  TEST_ASSERT_TRUE(has(j, "\"state_topic\":\"slytherm/state/sensor/kitchen/offset\""));
  TEST_ASSERT_TRUE(has(j, "\"command_topic\":\"slytherm/cmd/sensor/kitchen/offset\""));
  TEST_ASSERT_TRUE(has(j, "\"min\":-5"));
  TEST_ASSERT_TRUE(has(j, "\"max\":5"));
  TEST_ASSERT_TRUE(has(j, "\"step\":0.1"));
  TEST_ASSERT_TRUE(has(j, "\"entity_category\":\"config\""));
  TEST_ASSERT_TRUE(has(j, "\"availability_topic\":\"slytherm/availability\""));
  TEST_ASSERT_TRUE(has(j, "\"device\":{"));

  // The local DS18B20 fallback follows the same pattern.
  std::string l = sensorOffsetDiscoveryJson(kLocalSensorId);
  assertCoherentJson(l);
  TEST_ASSERT_TRUE(has(l, "\"unique_id\":\"slytherm_sensor_local_offset\""));
  TEST_ASSERT_TRUE(has(l, "\"command_topic\":\"slytherm/cmd/sensor/local/offset\""));
}

// #128: fan-circulate runtime tunables — scalar cmd/state topics + the two
// HA-editable number discovery entities.
static void test_fan_circulate_topics_and_discovery() {
  TEST_ASSERT_EQUAL_STRING("slytherm/cmd/fan_circulate_min", topic::kCmdFanCirculateMin);
  TEST_ASSERT_EQUAL_STRING("slytherm/cmd/fan_circulate_pct", topic::kCmdFanCirculatePct);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/fan_circulate_min", topic::kStateFanCirculateMin);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/fan_circulate_pct", topic::kStateFanCirculatePct);

  std::string m = fanCirculateMinDiscoveryJson();
  assertCoherentJson(m);
  TEST_ASSERT_TRUE(has(m, "\"unique_id\":\"slytherm_fan_circulate_min\""));
  TEST_ASSERT_TRUE(has(m, "\"state_topic\":\"slytherm/state/fan_circulate_min\""));
  TEST_ASSERT_TRUE(has(m, "\"command_topic\":\"slytherm/cmd/fan_circulate_min\""));
  TEST_ASSERT_TRUE(has(m, "\"min\":0"));
  TEST_ASSERT_TRUE(has(m, "\"max\":60"));
  TEST_ASSERT_TRUE(has(m, "\"entity_category\":\"config\""));

  std::string p = fanCirculatePctDiscoveryJson();
  assertCoherentJson(p);
  TEST_ASSERT_TRUE(has(p, "\"unique_id\":\"slytherm_fan_circulate_pct\""));
  TEST_ASSERT_TRUE(has(p, "\"command_topic\":\"slytherm/cmd/fan_circulate_pct\""));
  TEST_ASSERT_TRUE(has(p, "\"step\":25"));
}

static void test_parse_sensor_offset_bounds() {
  TEST_ASSERT_TRUE(parseSensorOffset("1.5").ok);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.5f, parseSensorOffset("1.5").value);
  TEST_ASSERT_TRUE(parseSensorOffset("-5").ok);
  TEST_ASSERT_TRUE(parseSensorOffset("5").ok);
  TEST_ASSERT_TRUE(parseSensorOffset("0").ok);
  TEST_ASSERT_FALSE(parseSensorOffset("5.1").ok);   // rejected, never clamped
  TEST_ASSERT_FALSE(parseSensorOffset("-5.1").ok);
  TEST_ASSERT_FALSE(parseSensorOffset("nan").ok);
  TEST_ASSERT_FALSE(parseSensorOffset("").ok);
  TEST_ASSERT_FALSE(parseSensorOffset(nullptr).ok);
}

static void test_sensor_roster_json_with_and_without_offset() {
  std::vector<SensorRosterEntry> out;
  TEST_ASSERT_TRUE(parseSensorRosterJson(
      "{\"sensors\":[{\"id\":\"kitchen\",\"name\":\"Kitchen\",\"max_age_s\":600,\"offset\":-1.5},"
      "{\"id\":\"hall\"}]}", out));
  TEST_ASSERT_EQUAL(2, out.size());
  TEST_ASSERT_EQUAL_STRING("kitchen", out[0].id.c_str());
  TEST_ASSERT_EQUAL_STRING("Kitchen", out[0].name.c_str());  // #85: friendly display name
  TEST_ASSERT_TRUE(out[0].hasMaxAge);
  TEST_ASSERT_EQUAL_UINT32(600, out[0].maxAgeS);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.5f, out[0].offsetC);
  TEST_ASSERT_EQUAL_STRING("hall", out[1].id.c_str());
  TEST_ASSERT_EQUAL_STRING("", out[1].name.c_str());  // #85: absent name -> empty (caller falls back to id)
  TEST_ASSERT_FALSE(out[1].hasMaxAge);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, out[1].offsetC);  // default 0

  // Empty roster is valid (clears the sensors).
  TEST_ASSERT_TRUE(parseSensorRosterJson("{\"sensors\":[]}", out));
  TEST_ASSERT_EQUAL(0, out.size());
}

static void test_sensor_roster_offset_tolerance_and_clamp() {
  std::vector<SensorRosterEntry> out;
  // null/malformed offset -> 0; out-of-range -> clamped to ±kSensorOffsetMaxC
  TEST_ASSERT_TRUE(parseSensorRosterJson(
      "{\"sensors\":[{\"id\":\"a\",\"offset\":null},"
      "{\"id\":\"b\",\"offset\":\"x\"},"
      "{\"id\":\"c\",\"offset\":9},"
      "{\"id\":\"d\",\"offset\":-9}]}", out));
  TEST_ASSERT_EQUAL(4, out.size());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, out[0].offsetC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, out[1].offsetC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, out[2].offsetC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -5.0f, out[3].offsetC);

  // Missing/empty/duplicate ids skipped; structural junk rejected whole.
  TEST_ASSERT_TRUE(parseSensorRosterJson(
      "{\"sensors\":[{\"offset\":1},{\"id\":\"\"},{\"id\":\"a\"},{\"id\":\"a\"}]}",
      out));
  TEST_ASSERT_EQUAL(1, out.size());
  TEST_ASSERT_EQUAL_STRING("a", out[0].id.c_str());
  TEST_ASSERT_FALSE(parseSensorRosterJson("{\"sensors\":42}", out));
  TEST_ASSERT_FALSE(parseSensorRosterJson("{}", out));
  TEST_ASSERT_FALSE(parseSensorRosterJson(nullptr, out));
  TEST_ASSERT_EQUAL(0, out.size());
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

// ---------- retained presence JSON (issue #88) ----------

static void test_parse_presence_json() {
  PresenceReading p;
  // Full message.
  TEST_ASSERT_TRUE(parsePresenceJson("{\"occupied\":true,\"last_seen\":1751731200}", p));
  TEST_ASSERT_TRUE(p.hasOccupied);
  TEST_ASSERT_TRUE(p.occupied);
  TEST_ASSERT_TRUE(p.hasLastSeen);
  TEST_ASSERT_EQUAL_UINT32(1751731200u, p.lastSeen);   // full integer precision, no float rounding

  // Vacant + key order + whitespace + unknown keys.
  TEST_ASSERT_TRUE(parsePresenceJson("{ \"last_seen\": 1700000000 , \"occupied\": false, \"x\":1 }", p));
  TEST_ASSERT_TRUE(p.hasOccupied);
  TEST_ASSERT_FALSE(p.occupied);
  TEST_ASSERT_EQUAL_UINT32(1700000000u, p.lastSeen);

  // occupied-only (no timestamp) still usable.
  TEST_ASSERT_TRUE(parsePresenceJson("{\"occupied\":true}", p));
  TEST_ASSERT_TRUE(p.hasOccupied);
  TEST_ASSERT_FALSE(p.hasLastSeen);

  // Rejections / absences.
  TEST_ASSERT_FALSE(parsePresenceJson(nullptr, p));
  TEST_ASSERT_FALSE(parsePresenceJson("", p));
  TEST_ASSERT_FALSE(parsePresenceJson("{}", p));                    // neither field
  // occupied:null -> absent; last_seen:0 -> treated as absent -> nothing usable.
  TEST_ASSERT_FALSE(parsePresenceJson("{\"occupied\":null,\"last_seen\":0}", p));
  TEST_ASSERT_FALSE(p.hasOccupied);
  TEST_ASSERT_FALSE(p.hasLastSeen);
}

// ---------- smart recovery next_target (issue #50) ----------

static void test_next_target_topic() {
  TEST_ASSERT_EQUAL_STRING("slytherm/cmd/next_target", topic::kCmdNextTarget);
}

static void test_parse_next_target_valid_and_tolerant() {
  NextTarget t;
  TEST_ASSERT_TRUE(parseNextTargetJson(
      "{\"temp\":21.0,\"mode\":\"heat\",\"in_s\":5400}", t));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f, t.tempC);
  TEST_ASSERT_TRUE(t.mode == Mode::kHeat);
  TEST_ASSERT_EQUAL_UINT32(5400, t.inS);

  // Key order, whitespace, unknown extra keys, fractional in_s truncated.
  TEST_ASSERT_TRUE(parseNextTargetJson(
      " {\n \"in_s\" : 90.7 , \"src\" : \"scheduler\" ,"
      " \"mode\" : \"cool\" , \"temp\" : 24.5\n} ", t));
  TEST_ASSERT_TRUE(t.mode == Mode::kCool);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 24.5f, t.tempC);
  TEST_ASSERT_EQUAL_UINT32(90, t.inS);

  // Boundaries: climate limits and in_s 0 / one-week cap are inclusive.
  TEST_ASSERT_TRUE(parseNextTargetJson(
      "{\"temp\":10,\"mode\":\"heat\",\"in_s\":0}", t));
  TEST_ASSERT_EQUAL_UINT32(0, t.inS);
  TEST_ASSERT_TRUE(parseNextTargetJson(
      "{\"temp\":30,\"mode\":\"cool\",\"in_s\":604800}", t));
}

static void test_parse_next_target_rejects_junk() {
  NextTarget t;
  TEST_ASSERT_FALSE(parseNextTargetJson(nullptr, t));
  TEST_ASSERT_FALSE(parseNextTargetJson("", t));
  TEST_ASSERT_FALSE(parseNextTargetJson("not json", t));
  TEST_ASSERT_FALSE(parseNextTargetJson("{}", t));
  // All three keys required.
  TEST_ASSERT_FALSE(parseNextTargetJson("{\"mode\":\"heat\",\"in_s\":60}", t));
  TEST_ASSERT_FALSE(parseNextTargetJson("{\"temp\":21,\"in_s\":60}", t));
  TEST_ASSERT_FALSE(parseNextTargetJson("{\"temp\":21,\"mode\":\"heat\"}", t));
  // Out-of-range / wrong-typed values.
  TEST_ASSERT_FALSE(parseNextTargetJson(
      "{\"temp\":9.9,\"mode\":\"heat\",\"in_s\":60}", t));   // below climate min
  TEST_ASSERT_FALSE(parseNextTargetJson(
      "{\"temp\":30.1,\"mode\":\"cool\",\"in_s\":60}", t));  // above climate max
  TEST_ASSERT_FALSE(parseNextTargetJson(
      "{\"temp\":\"21\",\"mode\":\"heat\",\"in_s\":60}", t));
  TEST_ASSERT_FALSE(parseNextTargetJson(
      "{\"temp\":21,\"mode\":\"heat_cool\",\"in_s\":60}", t));
  TEST_ASSERT_FALSE(parseNextTargetJson(
      "{\"temp\":21,\"mode\":\"HEAT\",\"in_s\":60}", t));
  TEST_ASSERT_FALSE(parseNextTargetJson(
      "{\"temp\":21,\"mode\":null,\"in_s\":60}", t));
  TEST_ASSERT_FALSE(parseNextTargetJson(
      "{\"temp\":21,\"mode\":\"heat\",\"in_s\":-1}", t));
  TEST_ASSERT_FALSE(parseNextTargetJson(
      "{\"temp\":21,\"mode\":\"heat\",\"in_s\":604801}", t));  // > 7 days
  TEST_ASSERT_FALSE(parseNextTargetJson(
      "{\"temp\":21,\"mode\":\"heat\",\"in_s\":null}", t));
  // Rejection leaves the output zeroed — never a half-applied target.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, t.tempC);
  TEST_ASSERT_TRUE(t.mode == Mode::kHeat);
  TEST_ASSERT_EQUAL_UINT32(0, t.inS);
}

// ---------- energy prices (#143, docs/13 §1) ----------

static void test_energy_prices_and_cop_proxy_topics() {
  TEST_ASSERT_EQUAL_STRING("slytherm/cmd/energy_prices", topic::kCmdEnergyPrices);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/cop_proxy", topic::kStateCopProxy);
}

static void test_parse_energy_prices_valid_and_tolerant() {
  EnergyPrices ep;
  TEST_ASSERT_TRUE(parseEnergyPricesJson("{\"elecKwh\":0.15,\"gasM3\":0.45}", ep));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.15f, ep.elecPerKwh);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.45f, ep.gasPerM3);

  // Key order, whitespace, unknown extra keys (a TOU automation may tag the
  // window), integer-valued numbers.
  TEST_ASSERT_TRUE(parseEnergyPricesJson(
      " {\n \"gasM3\" : 1 , \"window\" : \"offpeak\" , \"elecKwh\" : 0.028\n} ",
      ep));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.028f, ep.elecPerKwh);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, ep.gasPerM3);

  // Ceiling is inclusive.
  TEST_ASSERT_TRUE(parseEnergyPricesJson("{\"elecKwh\":10,\"gasM3\":10}", ep));
}

static void test_parse_energy_prices_rejects_junk() {
  EnergyPrices ep;
  TEST_ASSERT_FALSE(parseEnergyPricesJson(nullptr, ep));
  TEST_ASSERT_FALSE(parseEnergyPricesJson("", ep));
  TEST_ASSERT_FALSE(parseEnergyPricesJson("not json", ep));
  TEST_ASSERT_FALSE(parseEnergyPricesJson("{}", ep));
  // Both keys required.
  TEST_ASSERT_FALSE(parseEnergyPricesJson("{\"elecKwh\":0.15}", ep));
  TEST_ASSERT_FALSE(parseEnergyPricesJson("{\"gasM3\":0.45}", ep));
  // Zero / negative / above the sanity ceiling / wrong-typed / null / cut off.
  TEST_ASSERT_FALSE(parseEnergyPricesJson("{\"elecKwh\":0,\"gasM3\":0.45}", ep));
  TEST_ASSERT_FALSE(parseEnergyPricesJson("{\"elecKwh\":-0.15,\"gasM3\":0.45}", ep));
  TEST_ASSERT_FALSE(parseEnergyPricesJson("{\"elecKwh\":0.15,\"gasM3\":10.01}", ep));
  TEST_ASSERT_FALSE(parseEnergyPricesJson("{\"elecKwh\":\"0.15\",\"gasM3\":0.45}", ep));
  TEST_ASSERT_FALSE(parseEnergyPricesJson("{\"elecKwh\":null,\"gasM3\":0.45}", ep));
  TEST_ASSERT_FALSE(parseEnergyPricesJson("{\"elecKwh\":0.15,\"gasM3\":}", ep));
  // Rejection leaves the output zeroed — a rejected payload must never move
  // the switchover point.
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, ep.elecPerKwh);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, ep.gasPerM3);
}

// ---------- outdoor/fusion/changeover discovery (issue #50 cleanup) ----------

static void test_outdoor_fusion_changeover_discovery_builders() {
  std::string j = outdoorTempDiscoveryJson();
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"unique_id\":\"slytherm_outdoor_temp\""));
  TEST_ASSERT_TRUE(has(j, "\"state_topic\":\"slytherm/state/outdoor_temp\""));
  TEST_ASSERT_TRUE(has(j, "\"unit_of_measurement\":\"°C\""));
  TEST_ASSERT_TRUE(has(j, "\"device_class\":\"temperature\""));
  // Automation input (accessory blueprints), not a diagnostic.
  TEST_ASSERT_FALSE(has(j, "\"entity_category\""));

  j = outdoorSourceDiscoveryJson();
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"unique_id\":\"slytherm_outdoor_source\""));
  TEST_ASSERT_TRUE(has(j, "\"state_topic\":\"slytherm/state/outdoor_source\""));
  TEST_ASSERT_TRUE(has(j, "\"entity_category\":\"diagnostic\""));

  j = fusionDiscoveryJson();
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"unique_id\":\"slytherm_fusion\""));
  TEST_ASSERT_TRUE(has(j, "\"state_topic\":\"slytherm/state/fusion\""));
  TEST_ASSERT_TRUE(has(j, "\"value_template\":\"{{ value_json.temp }}\""));
  TEST_ASSERT_TRUE(has(j, "\"json_attributes_topic\":\"slytherm/state/fusion\""));
  TEST_ASSERT_TRUE(has(j, "\"unit_of_measurement\":\"°C\""));
  TEST_ASSERT_TRUE(has(j, "\"device_class\":\"temperature\""));
  TEST_ASSERT_TRUE(has(j, "\"entity_category\":\"diagnostic\""));

  j = changeoverReasonDiscoveryJson();
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"unique_id\":\"slytherm_changeover_reason\""));
  TEST_ASSERT_TRUE(has(j, "\"state_topic\":\"slytherm/state/changeover_reason\""));
  TEST_ASSERT_TRUE(has(j, "\"entity_category\":\"diagnostic\""));

  // Shared envelope: availability + device block on all four.
  for (const std::string& d :
       {outdoorTempDiscoveryJson(), outdoorSourceDiscoveryJson(),
        fusionDiscoveryJson(), changeoverReasonDiscoveryJson()}) {
    TEST_ASSERT_TRUE(has(d, "\"availability_topic\":\"slytherm/availability\""));
    TEST_ASSERT_TRUE(has(d, "\"device\":{"));
  }
}

// ---------- screen lock (issue #45) ----------

static void test_lock_clear_command_retained_safe() {
  // Exact magic payload only — the forgotten-PIN recovery must not be
  // triggerable by generic switch payloads or by the retained-delete
  // tombstone (empty payload) replayed from the broker.
  Parsed<bool> r = parseLockClearCommand(kLockClearPayload);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.value);
  TEST_ASSERT_EQUAL_STRING("clear_user_pin", kLockClearPayload);

  TEST_ASSERT_FALSE(parseLockClearCommand(nullptr).ok);
  TEST_ASSERT_FALSE(parseLockClearCommand("").ok);        // retained tombstone
  TEST_ASSERT_FALSE(parseLockClearCommand("ON").ok);
  TEST_ASSERT_FALSE(parseLockClearCommand("1").ok);
  TEST_ASSERT_FALSE(parseLockClearCommand("clear").ok);
  TEST_ASSERT_FALSE(parseLockClearCommand("CLEAR_USER_PIN").ok);
  TEST_ASSERT_FALSE(parseLockClearCommand("clear_user_pin ").ok);
  TEST_ASSERT_FALSE(parseLockClearCommand("clear_installer_pin").ok);
}

static void test_lock_state_json_and_strings() {
  TEST_ASSERT_EQUAL_STRING("unlocked", toString(LockState::kUnlocked));
  TEST_ASSERT_EQUAL_STRING("user_locked", toString(LockState::kUserLocked));
  TEST_ASSERT_EQUAL_STRING("installer_locked", toString(LockState::kInstallerLocked));
  TEST_ASSERT_EQUAL_STRING("settings", toString(LockLevel::kSettingsOnly));
  TEST_ASSERT_EQUAL_STRING("settings_setpoints",
                           toString(LockLevel::kSettingsAndSetpoints));

  std::string j =
      lockStateJson(LockState::kUserLocked, LockLevel::kSettingsOnly, true);
  assertCoherentJson(j);
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"user_locked\",\"level\":\"settings\",\"pin_set\":true}",
      j.c_str());
  j = lockStateJson(LockState::kUnlocked, LockLevel::kSettingsAndSetpoints, false);
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"state\":\"unlocked\""));
  TEST_ASSERT_TRUE(has(j, "\"level\":\"settings_setpoints\""));
  TEST_ASSERT_TRUE(has(j, "\"pin_set\":false"));
}

static void test_lock_topics_and_discovery() {
  TEST_ASSERT_EQUAL_STRING("slytherm/cmd/lock_clear", topic::kCmdLockClear);
  TEST_ASSERT_EQUAL_STRING("slytherm/state/lock", topic::kStateLock);

  std::string d = lockDiscoveryJson();
  assertCoherentJson(d);
  TEST_ASSERT_TRUE(has(d, "\"state_topic\":\"slytherm/state/lock\""));
  TEST_ASSERT_TRUE(has(d, "{{ value_json.state }}"));
  TEST_ASSERT_TRUE(has(d, "\"json_attributes_topic\":\"slytherm/state/lock\""));
  TEST_ASSERT_TRUE(has(d, "\"entity_category\":\"diagnostic\""));
  TEST_ASSERT_TRUE(has(d, "\"unique_id\":\"slytherm_lock\""));
  // Display entity only — clearing goes through the dedicated cmd topic,
  // never a discovery command_topic (no accidental retained switch).
  TEST_ASSERT_FALSE(has(d, "command_topic"));
}

// ---------- Remote link (issue #104) ----------

static void test_remote_link_topics() {
  TEST_ASSERT_EQUAL_STRING("slytherm/controller/status", topic::kControllerStatus);
  TEST_ASSERT_EQUAL_STRING("slytherm/remote/state", topic::kRemoteState);
  TEST_ASSERT_EQUAL_STRING("slytherm/remote/", topic::kRemoteIntentTopicPrefix);
  TEST_ASSERT_EQUAL_STRING("/intent", topic::kRemoteIntentTopicSuffix);
  TEST_ASSERT_EQUAL_STRING("slytherm/remote/+/intent",
                           topic::kRemoteIntentSubscribeWildcard);
}

static void test_controller_status_json() {
  std::string j = controllerStatusJson("8d82f4", true, "2026-07-07");
  assertCoherentJson(j);
  TEST_ASSERT_EQUAL_STRING(
      "{\"cid\":\"8d82f4\",\"status\":\"online\",\"version\":\"2026-07-07\"}",
      j.c_str());
  j = controllerStatusJson("8d82f4", false, "2026-07-07");
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"status\":\"offline\""));
}

static void test_remote_state_json_round_trip() {
  using dettson::HoldType;
  std::string j = remoteStateJson(21.0f, 25.5f, Mode::kHeat, false,
                                   HoldType::kTwoHours, 7032, "home",
                                   21.3f, true);
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"heatC\":21"));
  TEST_ASSERT_TRUE(has(j, "\"coolC\":25.5"));
  TEST_ASSERT_TRUE(has(j, "\"mode\":\"heat\""));
  TEST_ASSERT_TRUE(has(j, "\"emHeat\":false"));
  TEST_ASSERT_TRUE(has(j, "\"hold\":\"two_hours\""));
  TEST_ASSERT_TRUE(has(j, "\"holdRemainS\":7032"));
  TEST_ASSERT_TRUE(has(j, "\"activePreset\":\"home\""));
  TEST_ASSERT_TRUE(has(j, "\"fusedTempC\":21.3"));
  TEST_ASSERT_TRUE(has(j, "\"fusedTempValid\":true"));

  // Retained-echo churn guard: fusedTempC is quantized to 0.1 °C so fusion
  // wobble below display granularity serializes identically (the glue
  // diff-suppresses on the string).
  std::string a = remoteStateJson(21.0f, 25.5f, Mode::kHeat, false,
                                   HoldType::kTwoHours, 7032, "home",
                                   23.2933f, true);
  std::string b = remoteStateJson(21.0f, 25.5f, Mode::kHeat, false,
                                   HoldType::kTwoHours, 7032, "home",
                                   23.3141f, true);
  TEST_ASSERT_TRUE(has(a, "\"fusedTempC\":23.3"));
  TEST_ASSERT_EQUAL_STRING(a.c_str(), b.c_str());

  j = remoteStateJson(18.0f, 24.0f, Mode::kOff, true, HoldType::kNone, 0,
                      "none", 0.0f, false);
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"mode\":\"off\""));
  TEST_ASSERT_TRUE(has(j, "\"emHeat\":true"));
  TEST_ASSERT_TRUE(has(j, "\"hold\":\"none\""));
  TEST_ASSERT_TRUE(has(j, "\"fusedTempValid\":false"));
}

static void test_ota_topics_and_state_json() {
  TEST_ASSERT_EQUAL_STRING("slytherm/state/ota", topic::kStateOta);
  TEST_ASSERT_EQUAL_STRING("slytherm/cmd/ota_check", topic::kCmdOtaCheck);
  TEST_ASSERT_EQUAL_STRING("slytherm/cmd/ota_apply", topic::kCmdOtaApply);

  std::string j = otaStateJson("downloading", 42, "0.3.0", "0.4.0", "");
  assertCoherentJson(j);
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"downloading\",\"progress\":42,\"running\":\"0.3.0\","
      "\"available\":\"0.4.0\",\"error\":\"\"}",
      j.c_str());

  // Progress clamped; null state falls back to idle; error carried through.
  j = otaStateJson(nullptr, 200, "0.3.0", "", "verify: signature rejected");
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"state\":\"idle\""));
  TEST_ASSERT_TRUE(has(j, "\"progress\":100"));
  TEST_ASSERT_TRUE(has(j, "\"error\":\"verify: signature rejected\""));
}

static void test_remote_state_json_extras() {  // #116/#118
  using dettson::HoldType;
  std::string j = remoteStateJson(21.0f, 25.5f, Mode::kHeat, false,
                                   HoldType::kNone, 0, "home", 21.3f, true,
                                   "heating", "gas_heat", 2,
                                   "OAT stale: HP paused", "sensor stale",
                                   true, "Vacation until Jul 12");
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"action\":\"heating\""));
  TEST_ASSERT_TRUE(has(j, "\"equipment\":\"gas_heat\""));
  TEST_ASSERT_TRUE(has(j, "\"alarmN\":2"));
  TEST_ASSERT_TRUE(has(j, "\"alarm1\":\"OAT stale: HP paused\""));
  TEST_ASSERT_TRUE(has(j, "\"alarm2\":\"sensor stale\""));
  TEST_ASSERT_TRUE(has(j, "\"vacation\":true"));
  TEST_ASSERT_TRUE(has(j, "\"vacBanner\":\"Vacation until Jul 12\""));
  // Defaults (an old-style call): extras still emitted, idle/0/none.
  j = remoteStateJson(21.0f, 25.5f, Mode::kHeat, false, HoldType::kNone, 0,
                      "home", 21.3f, true);
  TEST_ASSERT_TRUE(has(j, "\"action\":\"idle\""));
  TEST_ASSERT_TRUE(has(j, "\"alarmN\":0"));
  TEST_ASSERT_FALSE(has(j, "\"alarm1\""));  // no texts when none active
  TEST_ASSERT_TRUE(has(j, "\"vacation\":false"));
}

static void test_boot_status_json() {  // #123/#145
  BootStatus s;
  s.reason = "panic";
  s.coredump = true;
  s.prevUptimeS = 8130;
  s.version = "0.5.0";
  s.bootCount = 3;
  s.rawReason = 4;
  s.rtcReason0 = 1;
  s.rtcReason1 = 14;
  s.lastAliveUptimeS = 28458;
  s.lastAliveEpoch = 1783148392;
  s.uptimeS = 7;
  std::string j = bootStatusJson(s);
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"reason\":\"panic\""));
  TEST_ASSERT_TRUE(has(j, "\"coredump\":true"));
  TEST_ASSERT_TRUE(has(j, "\"prevUptimeS\":8130"));
  TEST_ASSERT_TRUE(has(j, "\"version\":\"0.5.0\""));
  TEST_ASSERT_TRUE(has(j, "\"bootCount\":3"));
  TEST_ASSERT_TRUE(has(j, "\"rawReason\":4"));
  TEST_ASSERT_TRUE(has(j, "\"rtcReason0\":1"));
  TEST_ASSERT_TRUE(has(j, "\"rtcReason1\":14"));
  TEST_ASSERT_TRUE(has(j, "\"lastAliveUptimeS\":28458"));
  TEST_ASSERT_TRUE(has(j, "\"lastAliveEpoch\":1783148392"));
  TEST_ASSERT_TRUE(has(j, "\"uptimeS\":7"));
  BootStatus d;  // defaults: nulls read unknown/empty, extras zero
  d.reason = nullptr;
  d.version = nullptr;
  j = bootStatusJson(d);
  assertCoherentJson(j);
  TEST_ASSERT_TRUE(has(j, "\"reason\":\"unknown\""));
  TEST_ASSERT_TRUE(has(j, "\"coredump\":false"));
  TEST_ASSERT_TRUE(has(j, "\"uptimeS\":0"));
}

static void test_parse_remote_intent_vacation_and_ack() {  // #118
  RemoteIntent r;
  TEST_ASSERT_TRUE(parseRemoteIntentJson(
      "{\"id\":21,\"type\":\"vacation\",\"startDays\":2,\"nights\":7,"
      "\"heatC\":16.0,\"coolC\":28.0}", r));
  TEST_ASSERT_EQUAL(static_cast<int>(RemoteIntentType::kVacation), static_cast<int>(r.type));
  TEST_ASSERT_EQUAL_UINT16(2, r.vacStartDays);
  TEST_ASSERT_EQUAL_UINT16(7, r.vacNights);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 16.0f, r.heatC);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 28.0f, r.coolC);
  TEST_ASSERT_TRUE(parseRemoteIntentJson("{\"id\":22,\"type\":\"clear_vacation\"}", r));
  TEST_ASSERT_EQUAL(static_cast<int>(RemoteIntentType::kClearVacation), static_cast<int>(r.type));
  TEST_ASSERT_TRUE(parseRemoteIntentJson("{\"id\":23,\"type\":\"ack_alarms\"}", r));
  TEST_ASSERT_EQUAL(static_cast<int>(RemoteIntentType::kAckAlarms), static_cast<int>(r.type));
  // Bounds: nights 0 and startDays 31 reject whole.
  TEST_ASSERT_FALSE(parseRemoteIntentJson(
      "{\"id\":24,\"type\":\"vacation\",\"startDays\":0,\"nights\":0,"
      "\"heatC\":16.0,\"coolC\":28.0}", r));
  TEST_ASSERT_FALSE(parseRemoteIntentJson(
      "{\"id\":25,\"type\":\"vacation\",\"startDays\":31,\"nights\":7,"
      "\"heatC\":16.0,\"coolC\":28.0}", r));
}

static void test_parse_remote_intent_setpoints_and_mode() {
  RemoteIntent ri;
  TEST_ASSERT_TRUE(parseRemoteIntentJson(
      "{\"id\":1,\"type\":\"setpoints\",\"heatC\":21.0,\"coolC\":25.0}", ri));
  TEST_ASSERT_EQUAL_UINT32(1, ri.id);
  TEST_ASSERT_TRUE(ri.type == RemoteIntentType::kSetpoints);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f, ri.heatC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, ri.coolC);

  TEST_ASSERT_TRUE(parseRemoteIntentJson(
      "{\"id\":2,\"type\":\"mode\",\"mode\":\"cool\"}", ri));
  TEST_ASSERT_TRUE(ri.type == RemoteIntentType::kMode);
  TEST_ASSERT_TRUE(ri.mode == Mode::kCool);
}

static void test_parse_remote_intent_preset_hold_and_clear() {
  using dettson::HoldType;
  RemoteIntent ri;
  TEST_ASSERT_TRUE(parseRemoteIntentJson(
      "{\"id\":3,\"type\":\"preset\",\"preset\":\"away\"}", ri));
  TEST_ASSERT_TRUE(ri.type == RemoteIntentType::kPreset);
  TEST_ASSERT_EQUAL_STRING("away", ri.preset.c_str());

  TEST_ASSERT_TRUE(parseRemoteIntentJson(
      "{\"id\":4,\"type\":\"hold\",\"hold\":\"indefinite\"}", ri));
  TEST_ASSERT_TRUE(ri.type == RemoteIntentType::kHold);
  TEST_ASSERT_TRUE(ri.hold == HoldType::kIndefinite);

  TEST_ASSERT_TRUE(parseRemoteIntentJson("{\"id\":5,\"type\":\"clear_hold\"}", ri));
  TEST_ASSERT_TRUE(ri.type == RemoteIntentType::kClearHold);
}

static void test_parse_remote_intent_rejects_junk() {
  RemoteIntent ri;
  TEST_ASSERT_FALSE(parseRemoteIntentJson(nullptr, ri));
  TEST_ASSERT_FALSE(parseRemoteIntentJson("", ri));
  TEST_ASSERT_FALSE(parseRemoteIntentJson("not json", ri));
  TEST_ASSERT_FALSE(parseRemoteIntentJson("{}", ri));
  // id required, and must be > 0 (0 is the Controller-side dedupe sentinel).
  TEST_ASSERT_FALSE(parseRemoteIntentJson("{\"type\":\"clear_hold\"}", ri));
  TEST_ASSERT_FALSE(parseRemoteIntentJson(
      "{\"id\":0,\"type\":\"clear_hold\"}", ri));
  // type required / unknown type rejected.
  TEST_ASSERT_FALSE(parseRemoteIntentJson("{\"id\":1}", ri));
  TEST_ASSERT_FALSE(parseRemoteIntentJson(
      "{\"id\":1,\"type\":\"vacation\"}", ri));
  // Missing/out-of-range type-specific fields.
  TEST_ASSERT_FALSE(parseRemoteIntentJson(
      "{\"id\":1,\"type\":\"setpoints\",\"heatC\":21.0}", ri));  // coolC missing
  TEST_ASSERT_FALSE(parseRemoteIntentJson(
      "{\"id\":1,\"type\":\"setpoints\",\"heatC\":9.9,\"coolC\":25.0}", ri));  // below min
  TEST_ASSERT_FALSE(parseRemoteIntentJson(
      "{\"id\":1,\"type\":\"mode\",\"mode\":\"sideways\"}", ri));
  TEST_ASSERT_FALSE(parseRemoteIntentJson("{\"id\":1,\"type\":\"preset\"}", ri));
  TEST_ASSERT_FALSE(parseRemoteIntentJson(
      "{\"id\":1,\"type\":\"hold\",\"hold\":\"forever\"}", ri));
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
  RUN_TEST(test_enum_round_trip);
  RUN_TEST(test_parse_hold_command_accepts_types_and_clear);
  RUN_TEST(test_parse_hold_command_rejects_junk);
  RUN_TEST(test_hold_state_json_and_round_trip);
  RUN_TEST(test_hold_discovery_json);
  RUN_TEST(test_em_heat_topics);
  RUN_TEST(test_parse_em_heat_command);
  RUN_TEST(test_em_heat_discovery_json_and_not_a_mode_or_preset);
  RUN_TEST(test_preset_roster_json_round_trip);
  RUN_TEST(test_preset_roster_json_rejects_structural_junk);
  RUN_TEST(test_preset_roster_json_skips_invalid_entries_and_caps);
  RUN_TEST(test_sensor_offset_topics_and_discovery);
  RUN_TEST(test_fan_circulate_topics_and_discovery);
  RUN_TEST(test_parse_sensor_offset_bounds);
  RUN_TEST(test_sensor_roster_json_with_and_without_offset);
  RUN_TEST(test_sensor_roster_offset_tolerance_and_clamp);
  RUN_TEST(test_sensor_json_full_payload);
  RUN_TEST(test_sensor_json_missing_and_null_fields_tolerated);
  RUN_TEST(test_sensor_json_invalid_temp_rejected);
  RUN_TEST(test_sensor_json_bad_optional_fields_dropped_not_fatal);
  RUN_TEST(test_parse_presence_json);
  RUN_TEST(test_next_target_topic);
  RUN_TEST(test_parse_next_target_valid_and_tolerant);
  RUN_TEST(test_parse_next_target_rejects_junk);
  RUN_TEST(test_energy_prices_and_cop_proxy_topics);
  RUN_TEST(test_parse_energy_prices_valid_and_tolerant);
  RUN_TEST(test_parse_energy_prices_rejects_junk);
  RUN_TEST(test_outdoor_fusion_changeover_discovery_builders);
  RUN_TEST(test_lock_clear_command_retained_safe);
  RUN_TEST(test_lock_state_json_and_strings);
  RUN_TEST(test_lock_topics_and_discovery);
  RUN_TEST(test_remote_link_topics);
  RUN_TEST(test_controller_status_json);
  RUN_TEST(test_remote_state_json_round_trip);
  RUN_TEST(test_remote_state_json_extras);
  RUN_TEST(test_boot_status_json);
  RUN_TEST(test_parse_remote_intent_vacation_and_ack);
  RUN_TEST(test_ota_topics_and_state_json);
  RUN_TEST(test_parse_remote_intent_setpoints_and_mode);
  RUN_TEST(test_parse_remote_intent_preset_hold_and_clear);
  RUN_TEST(test_parse_remote_intent_rejects_junk);
  return UNITY_END();
}
