// Unit tests for Ct485Parser: name lookups, Set Control Command dual-variant
// demand decoding, system switch, diagnostics fault strings, sensor TLV walk,
// byte grid, field dictionary — including lying-length / truncation faults.
#include <unity.h>

#include <cstring>
#include <initializer_list>
#include <string>

#include "Ct485Core.h"
#include "Ct485Parser.h"

using namespace ct485;

void setUp() {}
void tearDown() {}

// sendMethod defaults to kByPriority, the method every OEM demand uses.
static Frame mkFrame(uint8_t msgType, std::initializer_list<uint8_t> payload,
                     uint8_t sendParamHi = 0,
                     uint8_t sendMethod = static_cast<uint8_t>(SendMethod::kByPriority)) {
  Frame f;
  f.dst = 0x02;
  f.src = kAddrThermostat;
  f.subnet = kSubnetV2;
  f.sendMethod = sendMethod;
  f.sendParamHi = sendParamHi;
  f.srcNodeType = static_cast<uint8_t>(NodeType::kThermostat);
  f.msgType = msgType;
  f.payloadLen = static_cast<uint8_t>(payload.size());
  size_t i = 0;
  for (uint8_t b : payload) f.payload[i++] = b;
  return f;
}

static bool contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

// ---------- name lookups ----------

static void test_msg_type_names() {
  TEST_ASSERT_EQUAL_STRING("R2R", msgTypeName(0x00).c_str());
  TEST_ASSERT_EQUAL_STRING("GET_STATUS", msgTypeName(0x02).c_str());
  TEST_ASSERT_EQUAL_STRING("SET_CONTROL_COMMAND", msgTypeName(0x03).c_str());
  TEST_ASSERT_EQUAL_STRING("GET_DIAGNOSTICS", msgTypeName(0x06).c_str());
  TEST_ASSERT_EQUAL_STRING("GET_SENSOR_DATA", msgTypeName(0x07).c_str());
  TEST_ASSERT_EQUAL_STRING("DMA_READ", msgTypeName(0x1D).c_str());
  TEST_ASSERT_EQUAL_STRING("TOKEN_OFFER", msgTypeName(0x77).c_str());
  TEST_ASSERT_EQUAL_STRING("VERSION_ANNOUNCEMENT", msgTypeName(0x78).c_str());
  TEST_ASSERT_EQUAL_STRING("NODE_DISCOVERY", msgTypeName(0x79).c_str());
  TEST_ASSERT_EQUAL_STRING("SET_ADDRESS", msgTypeName(0x7A).c_str());
  TEST_ASSERT_EQUAL_STRING("GET_NODE_ID", msgTypeName(0x7B).c_str());
  TEST_ASSERT_EQUAL_STRING("ECHO", msgTypeName(0x5A).c_str());
  // Every remaining enum value resolves to a non-hex name.
  for (uint8_t t : {0x01, 0x04, 0x05, 0x0D, 0x0E, 0x1E, 0x1F, 0x20, 0x41, 0x42, 0x75}) {
    TEST_ASSERT_FALSE_MESSAGE(contains(msgTypeName(t), "0x"), "enum value unmapped");
  }
}

static void test_msg_type_response_names() {
  TEST_ASSERT_EQUAL_STRING("GET_STATUS_RESPONSE", msgTypeName(0x82).c_str());
  TEST_ASSERT_EQUAL_STRING("GET_DIAGNOSTICS_RESPONSE", msgTypeName(0x86).c_str());
  TEST_ASSERT_EQUAL_STRING("GET_SENSOR_DATA_RESPONSE", msgTypeName(0x87).c_str());
  TEST_ASSERT_EQUAL_STRING("DMA_READ_RESPONSE", msgTypeName(0x9D).c_str());
  TEST_ASSERT_EQUAL_STRING("NODE_DISCOVERY_RESPONSE", msgTypeName(0xF9).c_str());
}

static void test_msg_type_unknown_is_hex() {
  TEST_ASSERT_EQUAL_STRING("0x4C", msgTypeName(0x4C).c_str());
  TEST_ASSERT_EQUAL_STRING("0xCC", msgTypeName(0xCC).c_str());  // unknown base, resp bit
}

static void test_command_names() {
  TEST_ASSERT_EQUAL_STRING("HEAT_DEMAND", commandName(0x64).c_str());
  TEST_ASSERT_EQUAL_STRING("COOL_DEMAND", commandName(0x65).c_str());
  TEST_ASSERT_EQUAL_STRING("FAN_DEMAND", commandName(0x66).c_str());
  TEST_ASSERT_EQUAL_STRING("BACKUP_HEAT_DEMAND", commandName(0x67).c_str());
  TEST_ASSERT_EQUAL_STRING("DEFROST_DEMAND", commandName(0x68).c_str());
  TEST_ASSERT_EQUAL_STRING("AUX_HEAT_DEMAND", commandName(0x69).c_str());
  TEST_ASSERT_EQUAL_STRING("SYSTEM_SWITCH_MODIFY", commandName(0x05).c_str());
  TEST_ASSERT_EQUAL_STRING("HEAT_SET_POINT_MODIFY", commandName(0x01).c_str());
  TEST_ASSERT_EQUAL_STRING("SET_MOTOR_TORQUE_PERCENT", commandName(0x70).c_str());
  for (uint8_t c : {0x02, 0x07, 0x47, 0x5D, 0x5E, 0x60, 0x61, 0x62, 0x63, 0x6A, 0x6B, 0x6C}) {
    TEST_ASSERT_FALSE_MESSAGE(contains(commandName(c), "0x"), "command unmapped");
  }
  TEST_ASSERT_EQUAL_STRING("0xEE", commandName(0xEE).c_str());
}

static void test_system_switch_names() {
  TEST_ASSERT_EQUAL_STRING("OFF", systemSwitchName(0x00).c_str());
  TEST_ASSERT_EQUAL_STRING("COOL", systemSwitchName(0x01).c_str());
  TEST_ASSERT_EQUAL_STRING("AUTO", systemSwitchName(0x02).c_str());
  TEST_ASSERT_EQUAL_STRING("HEAT", systemSwitchName(0x03).c_str());
  TEST_ASSERT_EQUAL_STRING("BACKUP_HEAT", systemSwitchName(0x04).c_str());
  TEST_ASSERT_EQUAL_STRING("0x09", systemSwitchName(0x09).c_str());
}

// ---------- Set Control Command: dual demand variants ----------

static void test_heat_demand_both_variants() {
  // frame[10]=command 0x64, [11]=0x00, [12]=0x12, [13]=0x50, [14]=0x60
  Frame f = mkFrame(0x03, {0x64, 0x00, 0x12, 0x50, 0x60}, 0x64);
  SetControlDecode d = decodeSetControl(f);
  TEST_ASSERT_TRUE(d.isSetControl);
  TEST_ASSERT_FALSE(d.isResponse);
  TEST_ASSERT_EQUAL_UINT8(0x64, d.commandCode);
  TEST_ASSERT_EQUAL_STRING("HEAT_DEMAND", d.command.c_str());
  TEST_ASSERT_TRUE(d.hasCommand);
  TEST_ASSERT_TRUE(d.sendParamMatches);
  TEST_ASSERT_FALSE(d.routedByNodeType);

  // Variant A: timer at [12], demand at [13] — must NOT pick a winner.
  TEST_ASSERT_TRUE(d.varA.valid);
  TEST_ASSERT_EQUAL_UINT8(0x12, d.varA.timerRaw);
  TEST_ASSERT_EQUAL_UINT8(0x50, d.varA.demandRaw);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, d.varA.demandPct);  // 0x50=80 -> 40%
  TEST_ASSERT_EQUAL_UINT8(1, d.varA.timerMinutes);
  TEST_ASSERT_EQUAL_UINT8(2, d.varA.timerUnits);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 67.5f, d.varA.timerTotalS);

  // Variant B: timer at [13], demand at [14] — reported alongside.
  TEST_ASSERT_TRUE(d.varB.valid);
  TEST_ASSERT_EQUAL_UINT8(0x50, d.varB.timerRaw);
  TEST_ASSERT_EQUAL_UINT8(0x60, d.varB.demandRaw);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 48.0f, d.varB.demandPct);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 300.0f, d.varB.timerTotalS);  // 0x50: 5 min

  TEST_ASSERT_EQUAL_size_t(kDemandTimerOffsetVarA, d.varA.timerFrameOffset);
  TEST_ASSERT_EQUAL_size_t(kDemandValueOffsetVarB, d.varB.valueFrameOffset);
}

static void test_demand_short_payloads() {
  // 4-byte payload: variant A decodable, variant B falls off the end.
  Frame f4 = mkFrame(0x03, {0x64, 0x00, 0x12, 0x50}, 0x64);
  SetControlDecode d4 = decodeSetControl(f4);
  TEST_ASSERT_TRUE(d4.varA.valid);
  TEST_ASSERT_FALSE(d4.varB.valid);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, d4.varB.demandPct);  // invalid -> zero, never garbage

  // Command bytes only: neither variant valid.
  Frame f2 = mkFrame(0x03, {0x64, 0x00}, 0x64);
  SetControlDecode d2 = decodeSetControl(f2);
  TEST_ASSERT_TRUE(d2.hasCommand);
  TEST_ASSERT_FALSE(d2.varA.valid);
  TEST_ASSERT_FALSE(d2.varB.valid);

  // Single byte: the command is still readable, no demand, no crash.
  Frame f1 = mkFrame(0x03, {0x64}, 0x64);
  SetControlDecode d1 = decodeSetControl(f1);
  TEST_ASSERT_TRUE(d1.hasCommand);
  TEST_ASSERT_EQUAL_UINT8(0x64, d1.commandCode);
  TEST_ASSERT_FALSE(d1.varA.valid);

  // Empty payload: no command at all.
  Frame f0 = mkFrame(0x03, {}, 0x64);
  SetControlDecode d0 = decodeSetControl(f0);
  TEST_ASSERT_TRUE(d0.isSetControl);
  TEST_ASSERT_FALSE(d0.hasCommand);
  TEST_ASSERT_FALSE(d0.isSystemSwitch);
}

static void test_demand_payload_len_lies() {
  // payloadLen claims 255 (> kMaxPayload buffer of 240): decoders must clamp.
  Frame f = mkFrame(0x03, {0x64, 0x00, 0x12, 0x50, 0x60}, 0x64);
  f.payloadLen = 255;
  TEST_ASSERT_EQUAL_size_t(kMaxPayload, effectivePayloadLen(f));
  SetControlDecode d = decodeSetControl(f);  // reads stay inside payload[240]
  TEST_ASSERT_TRUE(d.varA.valid);
  TEST_ASSERT_TRUE(d.varB.valid);
  TEST_ASSERT_EQUAL_UINT8(0x50, d.varA.demandRaw);
}

static void test_send_param_mismatch_flagged() {
  // kByPriority, but the header says HEAT and the payload says COOL: the
  // payload wins and the disagreement is surfaced.
  Frame f = mkFrame(0x03, {0x65, 0x00, 0x12, 0x50, 0x60}, 0x64);
  SetControlDecode d = decodeSetControl(f);
  TEST_ASSERT_EQUAL_UINT8(0x65, d.commandCode);
  TEST_ASSERT_EQUAL_STRING("COOL_DEMAND", d.command.c_str());
  TEST_ASSERT_EQUAL_UINT8(0x64, d.sendParamHi);
  TEST_ASSERT_FALSE(d.sendParamMatches);
  TEST_ASSERT_TRUE(contains(summarize(f), "MISMATCH"));
}

// #209: real OEM frames from the 2026-09-21 capture. Under sendMethod 0x02
// sendParamHi is the TARGET node type, and the command is payload[0].
static void test_hum_demand_routed_by_node_type() {
  // [ct485] FF>02 t03 l4 sn02 sm02 sp02 nt01 pk20 63 00 60 A0
  Frame f = mkFrame(0x03, {0x63, 0x00, 0x60, 0xA0}, 0x02,
                    static_cast<uint8_t>(SendMethod::kByNodeType));
  SetControlDecode d = decodeSetControl(f);
  TEST_ASSERT_TRUE(d.hasCommand);
  TEST_ASSERT_EQUAL_UINT8(0x63, d.commandCode);
  TEST_ASSERT_EQUAL_STRING("HUMIDIFICATION_DEMAND", d.command.c_str());
  TEST_ASSERT_TRUE(d.routedByNodeType);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(NodeType::kGasFurnace), d.targetNodeType);
  TEST_ASSERT_FALSE(d.sendParamMatches);  // not a kByPriority frame
  TEST_ASSERT_TRUE(d.varA.valid);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 80.0f, d.varA.demandPct);  // 0xA0 -> 80%
  // Before #209 this frame was named COOL_SET_POINT_MODIFY after sp02.
  const std::string s = summarize(f);
  TEST_ASSERT_TRUE(contains(s, "HUMIDIFICATION_DEMAND"));
  TEST_ASSERT_FALSE(contains(s, "COOL_SET_POINT_MODIFY"));
  TEST_ASSERT_TRUE(contains(s, "routed by node type to 0x02"));
}

static void test_subsystem_busy_routed_by_node_type() {
  // [ct485] 02>FF t03 l4 sn02 sm02 sp01 nt02 pk20 61 00 60 00
  Frame f = mkFrame(0x03, {0x61, 0x00, 0x60, 0x00}, 0x01,
                    static_cast<uint8_t>(SendMethod::kByNodeType));
  f.src = 0x02;
  f.dst = kAddrCoordinator;
  f.srcNodeType = static_cast<uint8_t>(NodeType::kGasFurnace);
  SetControlDecode d = decodeSetControl(f);
  TEST_ASSERT_EQUAL_UINT8(0x61, d.commandCode);
  TEST_ASSERT_EQUAL_STRING("SUBSYSTEM_BUSY_STATUS", d.command.c_str());
  TEST_ASSERT_TRUE(d.routedByNodeType);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(NodeType::kThermostat), d.targetNodeType);
  // Before #209 this frame was named HEAT_SET_POINT_MODIFY after sp01.
  TEST_ASSERT_FALSE(contains(summarize(f), "HEAT_SET_POINT_MODIFY"));
}

static void test_set_control_response_flag() {
  Frame f = mkFrame(0x83, {0x64, 0x00, kAck1}, 0x64);
  SetControlDecode d = decodeSetControl(f);
  TEST_ASSERT_TRUE(d.isSetControl);
  TEST_ASSERT_TRUE(d.isResponse);
}

static void test_non_set_control_rejected() {
  Frame f = mkFrame(0x82, {0x01, 0x02, 0x03}, 0x64);
  SetControlDecode d = decodeSetControl(f);
  TEST_ASSERT_FALSE(d.isSetControl);
  TEST_ASSERT_FALSE(d.varA.valid);
  TEST_ASSERT_FALSE(d.varB.valid);
}

// ---------- SYSTEM_SWITCH_MODIFY ----------

static void test_system_switch_decode() {
  Frame f = mkFrame(0x03, {0x05, 0x00, 0x03}, 0x05);
  SetControlDecode d = decodeSetControl(f);
  TEST_ASSERT_TRUE(d.isSystemSwitch);
  TEST_ASSERT_TRUE(d.hasSwitchValue);
  TEST_ASSERT_EQUAL_UINT8(0x03, d.switchRaw);
  TEST_ASSERT_TRUE(d.switchKnown);
  TEST_ASSERT_EQUAL_STRING("HEAT", d.switchName.c_str());
}

static void test_system_switch_unknown_value() {
  Frame f = mkFrame(0x03, {0x05, 0x00, 0x09}, 0x05);
  SetControlDecode d = decodeSetControl(f);
  TEST_ASSERT_TRUE(d.hasSwitchValue);
  TEST_ASSERT_FALSE(d.switchKnown);
  TEST_ASSERT_EQUAL_STRING("0x09", d.switchName.c_str());
}

static void test_system_switch_missing_value() {
  Frame f = mkFrame(0x03, {0x05, 0x00}, 0x05);  // echo only, value truncated
  SetControlDecode d = decodeSetControl(f);
  TEST_ASSERT_TRUE(d.isSystemSwitch);
  TEST_ASSERT_FALSE(d.hasSwitchValue);
}

// ---------- Get Diagnostics response (0x86) ----------

static void test_diagnostics_fault_strings() {
  Frame f = mkFrame(0x86, {'L', 'O', 'W', ' ', 'F', 'L', 'A', 'M', 'E', 0,
                           'P', 'S', 'W', 0});
  auto faults = decodeDiagnostics(f);
  TEST_ASSERT_EQUAL_size_t(2, faults.size());
  TEST_ASSERT_EQUAL_STRING("LOW FLAME", faults[0].c_str());
  TEST_ASSERT_EQUAL_STRING("PSW", faults[1].c_str());
}

static void test_diagnostics_no_trailing_null() {
  Frame f = mkFrame(0x86, {'E', '1', 0, 'E', '2'});  // last string unterminated
  auto faults = decodeDiagnostics(f);
  TEST_ASSERT_EQUAL_size_t(2, faults.size());
  TEST_ASSERT_EQUAL_STRING("E2", faults[1].c_str());
}

static void test_diagnostics_consecutive_and_leading_nulls() {
  Frame f = mkFrame(0x86, {0, 'E', '1', 0, 0, 0, 'E', '2', 0, 0});
  auto faults = decodeDiagnostics(f);
  TEST_ASSERT_EQUAL_size_t(2, faults.size());
  TEST_ASSERT_EQUAL_STRING("E1", faults[0].c_str());
  TEST_ASSERT_EQUAL_STRING("E2", faults[1].c_str());
}

static void test_diagnostics_empty_payload() {
  Frame f = mkFrame(0x86, {});
  TEST_ASSERT_EQUAL_size_t(0, decodeDiagnostics(f).size());
}

static void test_diagnostics_payload_len_lies() {
  Frame f = mkFrame(0x86, {});
  memset(f.payload, 'A', kMaxPayload);  // no null anywhere in the buffer
  f.payloadLen = 255;                   // lies past the 240-byte buffer
  auto faults = decodeDiagnostics(f);
  TEST_ASSERT_EQUAL_size_t(1, faults.size());
  TEST_ASSERT_EQUAL_size_t(kMaxPayload, faults[0].size());  // bounded, no overrun
}

// ---------- Get Sensor Data response (0x87) TLV ----------

static void test_sensor_tlv_walk() {
  // id 0 (len 2), id 5 (len 1), id 7 (len 0)
  Frame f = mkFrame(0x87, {0x00, 0x02, 0x01, 0x20, 0x05, 0x01, 0x7F, 0x07, 0x00});
  SensorDataDecode d = decodeSensorData(f);
  TEST_ASSERT_FALSE(d.truncated);
  TEST_ASSERT_EQUAL_size_t(3, d.records.size());

  TEST_ASSERT_EQUAL_UINT8(0, d.records[0].dbId);
  TEST_ASSERT_EQUAL_size_t(2, d.records[0].data.size());
  TEST_ASSERT_EQUAL_UINT8(0x01, d.records[0].data[0]);
  TEST_ASSERT_EQUAL_UINT8(0x20, d.records[0].data[1]);
  // MDI id 0 flagged as the OAT candidate, explicitly unconfirmed (docs/02 5b).
  TEST_ASSERT_EQUAL_STRING("OAT candidate (unconfirmed)", d.records[0].label.c_str());

  TEST_ASSERT_EQUAL_UINT8(5, d.records[1].dbId);
  TEST_ASSERT_EQUAL_size_t(0, d.records[1].label.size());  // only id 0 gets the label
  TEST_ASSERT_EQUAL_UINT8(7, d.records[2].dbId);
  TEST_ASSERT_EQUAL_size_t(0, d.records[2].data.size());
}

static void test_sensor_tlv_declared_len_overruns() {
  // Record claims 200 data bytes; only 2 exist after the TLV header.
  Frame f = mkFrame(0x87, {0x03, 200, 0xAA, 0xBB});
  SensorDataDecode d = decodeSensorData(f);
  TEST_ASSERT_TRUE(d.truncated);
  TEST_ASSERT_EQUAL_size_t(1, d.records.size());
  TEST_ASSERT_TRUE(d.records[0].truncated);
  TEST_ASSERT_EQUAL_UINT8(200, d.records[0].dbLen);
  TEST_ASSERT_EQUAL_size_t(2, d.records[0].data.size());  // only what existed
}

static void test_sensor_tlv_lone_id_byte() {
  Frame f = mkFrame(0x87, {0x00, 0x01, 0x42, 0x09});  // good record, then bare id
  SensorDataDecode d = decodeSensorData(f);
  TEST_ASSERT_TRUE(d.truncated);
  TEST_ASSERT_EQUAL_size_t(1, d.records.size());  // bare id yields no record
}

static void test_sensor_tlv_payload_len_lies() {
  Frame f = mkFrame(0x87, {});
  // Fill the whole buffer with zero-length records: (id=0x01, len=0x00) pairs.
  for (size_t i = 0; i < kMaxPayload; i += 2) {
    f.payload[i] = 0x01;
    f.payload[i + 1] = 0x00;
  }
  f.payloadLen = 255;  // lies; walk must stop at kMaxPayload
  SensorDataDecode d = decodeSensorData(f);
  TEST_ASSERT_EQUAL_size_t(kMaxPayload / 2, d.records.size());
  TEST_ASSERT_FALSE(d.truncated);
}

// ---------- byteGrid ----------

static void test_byte_grid_frame_offset_labels() {
  Frame f = mkFrame(0x82, {0xA2, 0x01, 0xFF, 0x02, 0x00, 0x00, 0x00, 0xA5,
                           0x00, 0xA0});
  std::string g = byteGrid(f);
  TEST_ASSERT_TRUE(contains(g, "[ 10]"));  // payload[0] is frame offset 10
  TEST_ASSERT_TRUE(contains(g, "[ 18]"));  // second row of 8
  TEST_ASSERT_TRUE(contains(g, "a2 01 ff 02 00 00 00 a5"));
  TEST_ASSERT_TRUE(contains(g, "00 a0"));
  TEST_ASSERT_TRUE(contains(g, "+0 +1 +2 +3 +4 +5 +6 +7"));
}

static void test_byte_grid_empty_payload() {
  Frame f = mkFrame(0x00, {});
  TEST_ASSERT_EQUAL_STRING("(no payload)\n", byteGrid(f).c_str());
}

// ---------- FieldDictionary ----------

static void test_field_dictionary_starter_set() {
  FieldDictionary d = FieldDictionary::withStarterSet();
  TEST_ASSERT_TRUE(d.size() >= 8);
  for (const auto& e : d.entries()) {
    TEST_ASSERT_TRUE_MESSAGE(e.provisional, "starter entries must be provisional");
  }
  // Both demand offset variants must be represented (the §5a ambiguity).
  TEST_ASSERT_NOT_NULL(d.find(0x03, 0x64, 12));
  TEST_ASSERT_NOT_NULL(d.find(0x03, 0x64, 13));
  TEST_ASSERT_NOT_NULL(d.find(0x03, 0x64, 14));
  TEST_ASSERT_NOT_NULL(d.find(0x03, 0x05, 12));  // system switch value
  TEST_ASSERT_NOT_NULL(d.find(0x87, 0x00, 10));  // sensor TLV / OAT candidate
  TEST_ASSERT_NULL(d.find(0x99, 0x00, 0));
}

static void test_field_dictionary_add_and_replace() {
  FieldDictionary d = FieldDictionary::withStarterSet();
  const size_t before = d.size();
  // Capture work confirms variant A: replace, don't duplicate.
  d.add({0x03, 0x64, 12, "HEAT_DEMAND refresh timer", "confirmed from capture 2026-06-11", false});
  TEST_ASSERT_EQUAL_size_t(before, d.size());
  const FieldEntry* e = d.find(0x03, 0x64, 12);
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_FALSE(e->provisional);
  d.add({0x82, 0x00, 11, "status block start", "", true});
  TEST_ASSERT_EQUAL_size_t(before + 1, d.size());
}

static void test_field_dictionary_table() {
  FieldDictionary d = FieldDictionary::withStarterSet();
  std::string t = d.toTable();
  TEST_ASSERT_TRUE(contains(t, "msgType"));
  TEST_ASSERT_TRUE(contains(t, "offset"));
  TEST_ASSERT_TRUE(contains(t, "PROVISIONAL"));
  TEST_ASSERT_TRUE(contains(t, "HEAT_DEMAND"));
  TEST_ASSERT_TRUE(contains(t, "OAT candidate (unconfirmed)"));
}

// ---------- summarize ----------

static void test_summarize_set_control_shows_both_variants() {
  Frame f = mkFrame(0x03, {0x64, 0x00, 0x12, 0x50, 0x60}, 0x64);
  std::string s = summarize(f);
  TEST_ASSERT_TRUE(contains(s, "SET_CONTROL_COMMAND"));
  TEST_ASSERT_TRUE(contains(s, "HEAT_DEMAND"));
  TEST_ASSERT_TRUE(contains(s, "PROVISIONAL"));
  TEST_ASSERT_TRUE(contains(s, "[12/13]"));  // variant A surfaced
  TEST_ASSERT_TRUE(contains(s, "[13/14]"));  // variant B surfaced
  TEST_ASSERT_TRUE(contains(s, "40.0%"));
  TEST_ASSERT_TRUE(contains(s, "48.0%"));
  TEST_ASSERT_TRUE(contains(s, "[ 10]"));    // byte grid included
}

static void test_summarize_diagnostics_and_sensor() {
  Frame fd = mkFrame(0x86, {'E', '1', 0});
  TEST_ASSERT_TRUE(contains(summarize(fd), "fault: E1"));
  TEST_ASSERT_TRUE(contains(summarize(fd), "GET_DIAGNOSTICS_RESPONSE"));

  Frame fs = mkFrame(0x87, {0x00, 0x01, 0x42});
  std::string s = summarize(fs);
  TEST_ASSERT_TRUE(contains(s, "GET_SENSOR_DATA_RESPONSE"));
  TEST_ASSERT_TRUE(contains(s, "OAT candidate (unconfirmed)"));
}

static void test_summarize_lying_len_warns() {
  Frame f = mkFrame(0x82, {0x01});
  f.payloadLen = 255;
  TEST_ASSERT_TRUE(contains(summarize(f), "clamped"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_msg_type_names);
  RUN_TEST(test_msg_type_response_names);
  RUN_TEST(test_msg_type_unknown_is_hex);
  RUN_TEST(test_command_names);
  RUN_TEST(test_system_switch_names);
  RUN_TEST(test_heat_demand_both_variants);
  RUN_TEST(test_demand_short_payloads);
  RUN_TEST(test_demand_payload_len_lies);
  RUN_TEST(test_send_param_mismatch_flagged);
  RUN_TEST(test_hum_demand_routed_by_node_type);
  RUN_TEST(test_subsystem_busy_routed_by_node_type);
  RUN_TEST(test_set_control_response_flag);
  RUN_TEST(test_non_set_control_rejected);
  RUN_TEST(test_system_switch_decode);
  RUN_TEST(test_system_switch_unknown_value);
  RUN_TEST(test_system_switch_missing_value);
  RUN_TEST(test_diagnostics_fault_strings);
  RUN_TEST(test_diagnostics_no_trailing_null);
  RUN_TEST(test_diagnostics_consecutive_and_leading_nulls);
  RUN_TEST(test_diagnostics_empty_payload);
  RUN_TEST(test_diagnostics_payload_len_lies);
  RUN_TEST(test_sensor_tlv_walk);
  RUN_TEST(test_sensor_tlv_declared_len_overruns);
  RUN_TEST(test_sensor_tlv_lone_id_byte);
  RUN_TEST(test_sensor_tlv_payload_len_lies);
  RUN_TEST(test_byte_grid_frame_offset_labels);
  RUN_TEST(test_byte_grid_empty_payload);
  RUN_TEST(test_field_dictionary_starter_set);
  RUN_TEST(test_field_dictionary_add_and_replace);
  RUN_TEST(test_field_dictionary_table);
  RUN_TEST(test_summarize_set_control_shows_both_variants);
  RUN_TEST(test_summarize_diagnostics_and_sensor);
  RUN_TEST(test_summarize_lying_len_warns);
  return UNITY_END();
}
