// HaMqtt.cpp — see HaMqtt.h. Pure C++17, no Arduino.
#include "HaMqtt.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace dettson {
namespace hamqtt {

// ---------- Topic helpers ----------

std::string sensorStateTopic(const std::string& sensorId) {
  return SLYTHERM_TOPIC_PREFIX "sensors/" + sensorId + "/state";
}

std::string sensorAgeStateTopic(const std::string& sensorId) {
  return SLYTHERM_TOPIC_PREFIX "state/sensor/" + sensorId + "/age";
}

std::string sensorParticipatingStateTopic(const std::string& sensorId) {
  return SLYTHERM_TOPIC_PREFIX "state/sensor/" + sensorId + "/participating";
}

std::string sensorOffsetCommandTopic(const std::string& sensorId) {
  return SLYTHERM_TOPIC_PREFIX "cmd/sensor/" + sensorId + "/offset";
}

std::string sensorOffsetStateTopic(const std::string& sensorId) {
  return SLYTHERM_TOPIC_PREFIX "state/sensor/" + sensorId + "/offset";
}

std::string discoveryTopic(const char* component, const std::string& objectId) {
  return std::string(topic::kDiscoveryPrefix) + "/" + component + "/slytherm/" +
         objectId + "/config";
}

// ---------- Enum strings ----------

const char* toString(Mode m) {
  switch (m) {
    case Mode::kOff: return "off";
    case Mode::kHeat: return "heat";
    case Mode::kCool: return "cool";
    case Mode::kHeatCool: return "heat_cool";
  }
  return "off";
}

const char* toString(FanMode f) {
  switch (f) {
    case FanMode::kAuto: return "auto";
    case FanMode::kOn: return "on";
    case FanMode::kCirculate: return "circulate";
  }
  return "auto";
}

const char* toString(LockState s) {
  switch (s) {
    case LockState::kUnlocked: return "unlocked";
    case LockState::kUserLocked: return "user_locked";
    case LockState::kInstallerLocked: return "installer_locked";
  }
  return "unlocked";
}

const char* toString(LockLevel l) {
  switch (l) {
    case LockLevel::kSettingsOnly: return "settings";
    case LockLevel::kSettingsAndSetpoints: return "settings_setpoints";
  }
  return "settings";
}

const char* toString(HoldType t) {
  switch (t) {
    case HoldType::kNone: return "none";
    case HoldType::kUntilNextPreset: return "until_next_preset";
    case HoldType::kTwoHours: return "two_hours";
    case HoldType::kFourHours: return "four_hours";
    case HoldType::kIndefinite: return "indefinite";
  }
  return "none";
}

// ---------- Inbound parsers ----------

Parsed<float> parseSetpoint(const char* payloadStr, float minC, float maxC) {
  Parsed<float> r;
  if (payloadStr == nullptr || *payloadStr == '\0') return r;
  char* end = nullptr;
  float v = std::strtof(payloadStr, &end);  // skips leading whitespace itself
  if (end == payloadStr) return r;          // no digits consumed
  while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
  if (*end != '\0') return r;               // trailing junk
  if (!std::isfinite(v)) return r;          // strtof accepts "nan"/"inf" text
  if (v < minC || v > maxC) return r;
  r.ok = true;
  r.value = v;
  return r;
}

Parsed<float> parseSensorOffset(const char* payloadStr) {
  return parseSetpoint(payloadStr, -kSensorOffsetMaxC, kSensorOffsetMaxC);
}

Parsed<Mode> parseMode(const char* payloadStr) {
  Parsed<Mode> r;
  if (payloadStr == nullptr) return r;
  if (std::strcmp(payloadStr, "off") == 0) { r.ok = true; r.value = Mode::kOff; }
  else if (std::strcmp(payloadStr, "heat") == 0) { r.ok = true; r.value = Mode::kHeat; }
  else if (std::strcmp(payloadStr, "cool") == 0) { r.ok = true; r.value = Mode::kCool; }
  else if (std::strcmp(payloadStr, "heat_cool") == 0) { r.ok = true; r.value = Mode::kHeatCool; }
  return r;
}

Parsed<FanMode> parseFanMode(const char* payloadStr) {
  Parsed<FanMode> r;
  if (payloadStr == nullptr) return r;
  if (std::strcmp(payloadStr, "auto") == 0) { r.ok = true; r.value = FanMode::kAuto; }
  else if (std::strcmp(payloadStr, "on") == 0) { r.ok = true; r.value = FanMode::kOn; }
  else if (std::strcmp(payloadStr, "circulate") == 0) { r.ok = true; r.value = FanMode::kCirculate; }
  return r;
}

Parsed<HoldCommand> parseHoldCommand(const char* payloadStr) {
  Parsed<HoldCommand> r;
  if (payloadStr == nullptr) return r;
  if (std::strcmp(payloadStr, "clear") == 0) {
    r.ok = true;
    r.value.clear = true;
  } else if (std::strcmp(payloadStr, "until_next_preset") == 0) {
    r.ok = true;
    r.value.type = HoldType::kUntilNextPreset;
  } else if (std::strcmp(payloadStr, "two_hours") == 0) {
    r.ok = true;
    r.value.type = HoldType::kTwoHours;
  } else if (std::strcmp(payloadStr, "four_hours") == 0) {
    r.ok = true;
    r.value.type = HoldType::kFourHours;
  } else if (std::strcmp(payloadStr, "indefinite") == 0) {
    r.ok = true;
    r.value.type = HoldType::kIndefinite;
  }
  // "none" deliberately rejected: ending a hold is the explicit "clear"
  return r;
}

Parsed<bool> parseEmHeatCommand(const char* payloadStr) {
  Parsed<bool> r;
  if (payloadStr == nullptr) return r;
  if (std::strcmp(payloadStr, payload::kOn) == 0) { r.ok = true; r.value = true; }
  else if (std::strcmp(payloadStr, payload::kOff) == 0) { r.ok = true; r.value = false; }
  return r;
}

Parsed<bool> parseLockClearCommand(const char* payloadStr) {
  // Exact-match only (see header: retained-safe contract). Empty payload is
  // the glue's own retained-delete tombstone — must parse as "not a command".
  Parsed<bool> r;
  if (payloadStr == nullptr) return r;
  if (std::strcmp(payloadStr, kLockClearPayload) == 0) {
    r.ok = true;
    r.value = true;
  }
  return r;
}

// ---------- Sensor JSON scanner ----------

namespace {

const char* skipWs(const char* p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
  return p;
}

// Returns a pointer to the first character of the value for "key": ..., or
// nullptr. Substring scan over quoted keys; see header for documented limits.
const char* findValue(const char* json, const char* key) {
  std::string pat = "\"";
  pat += key;
  pat += "\"";
  const char* p = std::strstr(json, pat.c_str());
  while (p != nullptr) {
    const char* q = skipWs(p + pat.size());
    if (*q == ':') return skipWs(q + 1);
    p = std::strstr(p + 1, pat.c_str());
  }
  return nullptr;
}

// Parses a bare JSON number token; the token must end at , } or end-of-input.
bool numberToken(const char* q, float& out) {
  char* end = nullptr;
  float v = std::strtof(q, &end);
  if (end == q || !std::isfinite(v)) return false;
  const char* t = skipWs(end);
  if (*t != ',' && *t != '}' && *t != '\0') return false;
  out = v;
  return true;
}

bool keywordToken(const char* q, const char* kw) {
  size_t n = std::strlen(kw);
  if (std::strncmp(q, kw, n) != 0) return false;
  char c = q[n];
  return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_');
}

// Parses a JSON string token at q. Only \" \\ and \/ escapes are supported
// (preset names are plain identifiers-with-spaces); anything else — control
// chars, \uXXXX, unterminated — fails. Empty strings fail.
bool stringToken(const char* q, std::string& out) {
  if (*q != '"') return false;
  ++q;
  out.clear();
  while (*q != '\0' && *q != '"') {
    if (*q == '\\') {
      ++q;
      if (*q == '"' || *q == '\\' || *q == '/') out += *q++;
      else return false;
    } else if (static_cast<unsigned char>(*q) < 0x20) {
      return false;
    } else {
      out += *q++;
    }
  }
  return *q == '"' && !out.empty();
}

// p at '{': returns pointer to the matching '}' (string-aware), or nullptr.
const char* objEnd(const char* p) {
  int depth = 0;
  bool inStr = false, esc = false;
  for (; *p != '\0'; ++p) {
    if (inStr) {
      if (esc) esc = false;
      else if (*p == '\\') esc = true;
      else if (*p == '"') inStr = false;
    } else if (*p == '"') {
      inStr = true;
    } else if (*p == '{') {
      ++depth;
    } else if (*p == '}') {
      if (--depth == 0) return p;
    }
  }
  return nullptr;
}

}  // namespace

bool parseSensorJson(const char* json, SensorReading& out) {
  out = SensorReading{};
  if (json == nullptr) return false;
  if (*skipWs(json) != '{') return false;

  float v = 0.0f;
  const char* t = findValue(json, "temp");
  if (t != nullptr && numberToken(t, v)) {
    out.hasTemp = true;
    out.tempC = v;
  }
  const char* o = findValue(json, "occ");
  if (o != nullptr) {
    if (keywordToken(o, "true")) { out.hasOcc = true; out.occupied = true; }
    else if (keywordToken(o, "false")) { out.hasOcc = true; out.occupied = false; }
    // null / malformed -> absent
  }
  const char* b = findValue(json, "bat");
  if (b != nullptr && numberToken(b, v) && v >= 0.0f && v <= 100.0f) {
    out.hasBat = true;
    out.batteryPct = static_cast<uint8_t>(v + 0.5f);
  }
  const char* h = findValue(json, "hum");
  if (h != nullptr && numberToken(h, v) && v >= 0.0f && v <= 100.0f) {
    out.hasHum = true;
    out.humidityPct = v;
  }
  return out.hasTemp;  // a reading without a valid temp is unusable
}

bool parseNextTargetJson(const char* json, NextTarget& out) {
  out = NextTarget{};
  if (json == nullptr) return false;
  if (*skipWs(json) != '{') return false;

  float temp = 0.0f;
  const char* t = findValue(json, "temp");
  if (t == nullptr || !numberToken(t, temp) || temp < kClimateMinTempC ||
      temp > kClimateMaxTempC) {
    return false;
  }
  const char* m = findValue(json, "mode");
  std::string modeStr;
  if (m == nullptr || !stringToken(m, modeStr)) return false;
  Mode mode;
  if (modeStr == "heat") mode = Mode::kHeat;
  else if (modeStr == "cool") mode = Mode::kCool;
  else return false;

  float inS = 0.0f;
  const char* i = findValue(json, "in_s");
  if (i == nullptr || !numberToken(i, inS) || inS < 0.0f ||
      inS > static_cast<float>(kNextTargetMaxInS)) {
    return false;
  }

  out.tempC = temp;  // assign only after every gate passed
  out.mode = mode;
  out.inS = static_cast<uint32_t>(inS);
  return true;
}

bool parsePresetRosterJson(const char* json, std::vector<PresetEntry>& out) {
  out.clear();
  if (json == nullptr) return false;
  if (*skipWs(json) != '{') return false;
  const char* p = findValue(json, "presets");
  if (p == nullptr || *p != '[') return false;
  p = skipWs(p + 1);
  while (*p != ']') {
    if (*p != '{') { out.clear(); return false; }  // structural junk
    const char* e = objEnd(p);
    if (e == nullptr) { out.clear(); return false; }
    const std::string entry(p, e + 1);  // nul-terminated slice for findValue

    PresetEntry pe;
    bool valid = true;
    const char* n = findValue(entry.c_str(), "name");
    if (n == nullptr || !stringToken(n, pe.name) ||
        pe.name.size() > kPresetNameMaxLen) {
      valid = false;
    }
    float v = 0.0f;
    const char* h = findValue(entry.c_str(), "heat");
    if (valid && h != nullptr && numberToken(h, v) && v >= kClimateMinTempC &&
        v <= kClimateMaxTempC) {
      pe.heatC = v;
    } else {
      valid = false;
    }
    const char* c = findValue(entry.c_str(), "cool");
    if (valid && c != nullptr && numberToken(c, v) && v >= kClimateMinTempC &&
        v <= kClimateMaxTempC) {
      pe.coolC = v;
    } else {
      valid = false;
    }
    if (valid) {
      for (const PresetEntry& prev : out) {
        if (prev.name == pe.name) { valid = false; break; }  // duplicate name
      }
    }
    if (valid && out.size() < kMaxPresets) out.push_back(pe);

    p = skipWs(e + 1);
    if (*p == ',') p = skipWs(p + 1);
    else if (*p != ']') { out.clear(); return false; }
  }
  return true;  // an empty roster is a legitimate "clear the presets"
}

bool parseSensorRosterJson(const char* json, std::vector<SensorRosterEntry>& out) {
  out.clear();
  if (json == nullptr) return false;
  if (*skipWs(json) != '{') return false;
  const char* p = findValue(json, "sensors");
  if (p == nullptr || *p != '[') return false;
  p = skipWs(p + 1);
  while (*p != ']') {
    if (*p != '{') { out.clear(); return false; }  // structural junk
    const char* e = objEnd(p);
    if (e == nullptr) { out.clear(); return false; }
    const std::string entry(p, e + 1);  // nul-terminated slice for findValue

    SensorRosterEntry se;
    bool valid = true;
    const char* n = findValue(entry.c_str(), "id");
    if (n == nullptr || !stringToken(n, se.id)) valid = false;
    float v = 0.0f;
    const char* m = findValue(entry.c_str(), "max_age_s");
    if (m != nullptr && numberToken(m, v) && v > 0.0f) {
      se.hasMaxAge = true;
      se.maxAgeS = static_cast<uint32_t>(v);
    }
    const char* o = findValue(entry.c_str(), "offset");
    if (o != nullptr && numberToken(o, v)) {
      se.offsetC = v > kSensorOffsetMaxC
                       ? kSensorOffsetMaxC
                       : (v < -kSensorOffsetMaxC ? -kSensorOffsetMaxC : v);
    }
    if (valid) {
      for (const SensorRosterEntry& prev : out) {
        if (prev.id == se.id) { valid = false; break; }  // duplicate id
      }
    }
    if (valid && out.size() < kSensorRosterMax) out.push_back(se);

    p = skipWs(e + 1);
    if (*p == ',') p = skipWs(p + 1);
    else if (*p != ']') { out.clear(); return false; }
  }
  return true;  // an empty roster is a legitimate "clear the sensors"
}

// ---------- JSON building ----------

std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char ch : s) {
    unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  return out;
}

namespace {

std::string numStr(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

// Minimal append-only JSON object builder; all string values escaped.
class Obj {
 public:
  Obj() : s_("{") {}
  Obj& str(const char* key, const std::string& val) {
    addKey(key);
    s_ += '"';
    s_ += jsonEscape(val);
    s_ += '"';
    return *this;
  }
  Obj& num(const char* key, double val) {
    addKey(key);
    s_ += numStr(val);
    return *this;
  }
  Obj& raw(const char* key, const std::string& alreadyJson) {
    addKey(key);
    s_ += alreadyJson;
    return *this;
  }
  std::string close() {
    s_ += '}';
    return s_;
  }

 private:
  void addKey(const char* key) {
    if (!first_) s_ += ',';
    first_ = false;
    s_ += '"';
    s_ += jsonEscape(key);
    s_ += "\":";
  }
  std::string s_;
  bool first_ = true;
};

std::string strList(std::initializer_list<const char*> items) {
  std::string s = "[";
  bool first = true;
  for (const char* it : items) {
    if (!first) s += ',';
    first = false;
    s += '"';
    s += jsonEscape(it);
    s += '"';
  }
  s += ']';
  return s;
}

std::string strList(const std::vector<std::string>& items) {
  std::string s = "[";
  bool first = true;
  for (const std::string& it : items) {
    if (!first) s += ',';
    first = false;
    s += '"';
    s += jsonEscape(it);
    s += '"';
  }
  s += ']';
  return s;
}

std::string deviceJson() {
  return Obj()
      .raw("identifiers", strList({"slytherm_esp32"}))
      .str("name", "SlyTherm ClimateTalk Thermostat")
      .str("manufacturer", "ElectricRV")
      .str("model", "ESP32-S3 CT-485")
      .close();
}

struct EntitySpec {
  std::string name;
  std::string uniqueId;
  std::string stateTopic;
  std::string commandTopic;  // non-empty for editable entities (number)
  const char* unit = nullptr;
  const char* deviceClass = nullptr;
  const char* valueTemplate = nullptr;
  const char* jsonAttributesTopic = nullptr;
  bool diagnostic = false;
  bool config = false;  // entity_category config (HA-editable settings)
  bool binary = false;  // adds payload_on/payload_off ON/OFF
  bool hasRange = false;  // adds min/max/step (number entities)
  float minV = 0.0f, maxV = 0.0f, stepV = 0.0f;
  const char* optionsJson = nullptr;  // pre-built JSON array for a select entity
};

std::string entityDiscoveryJson(const EntitySpec& e) {
  Obj o;
  // object_id pins a clean entity_id (sensor.slytherm_<x>) instead of HA
  // auto-prefixing the device name (docs/06; matches ha/packages).
  o.str("name", e.name).str("unique_id", e.uniqueId).str("object_id", e.uniqueId)
      .str("state_topic", e.stateTopic);
  if (!e.commandTopic.empty()) o.str("command_topic", e.commandTopic);
  if (e.hasRange) o.num("min", e.minV).num("max", e.maxV).num("step", e.stepV);
  if (e.optionsJson != nullptr) o.raw("options", e.optionsJson);
  if (e.unit != nullptr) o.str("unit_of_measurement", e.unit);
  if (e.deviceClass != nullptr) o.str("device_class", e.deviceClass);
  if (e.valueTemplate != nullptr) o.str("value_template", e.valueTemplate);
  if (e.jsonAttributesTopic != nullptr) o.str("json_attributes_topic", e.jsonAttributesTopic);
  if (e.diagnostic) o.str("entity_category", "diagnostic");
  else if (e.config) o.str("entity_category", "config");
  if (e.binary) o.str("payload_on", payload::kOn).str("payload_off", payload::kOff);
  o.str("availability_topic", topic::kAvailability)
      .str("payload_available", payload::kOnline)
      .str("payload_not_available", payload::kOffline)
      .raw("device", deviceJson());
  return o.close();
}

}  // namespace

std::string climateDiscoveryJson(const std::vector<std::string>& presetModes) {
  return Obj()
      .str("name", "SlyTherm HVAC")
      .str("unique_id", "slytherm_hvac")
      .str("object_id", "slytherm_hvac")
      .raw("modes", strList({"off", "heat", "cool", "heat_cool"}))
      .raw("fan_modes", strList({"auto", "on", "circulate"}))
      .raw("preset_modes", strList(presetModes))  // built from the roster
      .num("min_temp", kClimateMinTempC)
      .num("max_temp", kClimateMaxTempC)
      .num("temp_step", kClimateTempStepC)
      .str("temperature_unit", "C")
      .str("current_temperature_topic", topic::kStateCurrentTemp)
      .str("temperature_command_topic", topic::kCmdSetpoint)
      .str("temperature_state_topic", topic::kStateSetpoint)
      .str("temperature_low_command_topic", topic::kCmdTargetTempLow)
      .str("temperature_low_state_topic", topic::kStateTargetTempLow)
      .str("temperature_high_command_topic", topic::kCmdTargetTempHigh)
      .str("temperature_high_state_topic", topic::kStateTargetTempHigh)
      .str("mode_command_topic", topic::kCmdMode)
      .str("mode_state_topic", topic::kStateMode)
      .str("fan_mode_command_topic", topic::kCmdFanMode)
      .str("fan_mode_state_topic", topic::kStateFanMode)
      .str("preset_mode_command_topic", topic::kCmdPreset)
      .str("preset_mode_state_topic", topic::kStatePreset)
      .str("action_topic", topic::kStateAction)
      .str("availability_topic", topic::kAvailability)
      .str("payload_available", payload::kOnline)
      .str("payload_not_available", payload::kOffline)
      .raw("device", deviceJson())
      .close();
}

std::string activeEquipmentDiscoveryJson() {
  EntitySpec e;
  e.name = "SlyTherm Active Equipment";
  e.uniqueId = "slytherm_active_equipment";
  e.stateTopic = topic::kStateActiveEquipment;
  return entityDiscoveryJson(e);
}

std::string modulationDiscoveryJson() {
  EntitySpec e;
  e.name = "SlyTherm Gas Modulation";
  e.uniqueId = "slytherm_modulation";
  e.stateTopic = topic::kStateModulation;
  e.unit = "%";
  return entityDiscoveryJson(e);
}

std::string blowerDiscoveryJson() {
  EntitySpec e;
  e.name = "SlyTherm Blower";
  e.uniqueId = "slytherm_blower";
  e.stateTopic = topic::kStateBlower;
  return entityDiscoveryJson(e);
}

std::string faultDiscoveryJson() {
  EntitySpec e;
  e.name = "SlyTherm Fault";
  e.uniqueId = "slytherm_fault";
  e.stateTopic = topic::kStateFault;
  return entityDiscoveryJson(e);
}

std::string healthDiscoveryJson() {
  EntitySpec e;
  e.name = "SlyTherm Health";
  e.uniqueId = "slytherm_health";
  e.stateTopic = topic::kStateHealth;
  e.deviceClass = "problem";
  e.binary = true;
  return entityDiscoveryJson(e);
}

std::string lastErrorDiscoveryJson() {
  EntitySpec e;
  e.name = "SlyTherm Last Error";
  e.uniqueId = "slytherm_last_error";
  e.stateTopic = topic::kStateLastError;
  e.diagnostic = true;
  return entityDiscoveryJson(e);
}

std::string compressorMinOffRemainingDiscoveryJson() {
  EntitySpec e;
  e.name = "SlyTherm Compressor Min-Off Remaining";
  e.uniqueId = "slytherm_compressor_min_off_remaining";
  e.stateTopic = topic::kStateCompressorMinOffRemaining;
  e.unit = "s";
  e.diagnostic = true;
  return entityDiscoveryJson(e);
}

std::string compressorLockedOutDiscoveryJson() {
  EntitySpec e;
  e.name = "SlyTherm Compressor Locked Out";
  e.uniqueId = "slytherm_compressor_locked_out";
  e.stateTopic = topic::kStateCompressorLockedOut;
  e.diagnostic = true;
  e.binary = true;
  return entityDiscoveryJson(e);
}

std::string holdDiscoveryJson() {
  EntitySpec e;
  e.name = "SlyTherm Hold";
  e.uniqueId = "slytherm_hold";
  e.stateTopic = topic::kStateHold;
  e.valueTemplate = "{{ value_json.type }}";
  e.jsonAttributesTopic = topic::kStateHold;
  e.diagnostic = true;
  return entityDiscoveryJson(e);
}

// Hold-duration select (issue #81): set/read the active hold from HA. The
// command topic (kCmdHold) already accepts the wire strings; "none" resumes
// the schedule (mapped to a clear in the glue, since the parser reserves
// "clear"). Options mirror the HoldType<->string map so state round-trips.
std::string holdSelectDiscoveryJson() {
  EntitySpec e;
  e.name = "SlyTherm Hold Duration";
  e.uniqueId = "slytherm_hold_duration";
  e.stateTopic = topic::kStateHold;
  e.commandTopic = topic::kCmdHold;
  e.valueTemplate = "{{ value_json.type }}";
  e.optionsJson =
      "[\"none\",\"until_next_preset\",\"two_hours\",\"four_hours\",\"indefinite\"]";
  e.config = true;
  return entityDiscoveryJson(e);
}

std::string emHeatDiscoveryJson() {
  EntitySpec e;
  e.name = "SlyTherm Emergency Heat";
  e.uniqueId = "slytherm_em_heat";
  e.stateTopic = topic::kStateEmHeat;
  e.commandTopic = topic::kCmdEmHeat;
  e.binary = true;  // ON/OFF payloads, both directions
  return entityDiscoveryJson(e);
}

std::string holdStateJson(HoldType t, uint32_t remainingS) {
  return Obj().str("type", toString(t)).num("remaining", remainingS).close();
}

std::string lockStateJson(LockState s, LockLevel l, bool userPinSet) {
  return Obj()
      .str("state", toString(s))
      .str("level", toString(l))
      .raw("pin_set", userPinSet ? "true" : "false")
      .close();
}

std::string lockDiscoveryJson() {
  EntitySpec e;
  e.name = "SlyTherm Screen Lock";
  e.uniqueId = "slytherm_lock";
  e.stateTopic = topic::kStateLock;
  e.valueTemplate = "{{ value_json.state }}";
  e.jsonAttributesTopic = topic::kStateLock;
  e.diagnostic = true;
  return entityDiscoveryJson(e);
}

std::string outdoorTempDiscoveryJson() {
  EntitySpec e;
  e.name = "SlyTherm Outdoor Temperature";
  e.uniqueId = "slytherm_outdoor_temp";
  e.stateTopic = topic::kStateOutdoorTemp;
  e.unit = "°C";
  e.deviceClass = "temperature";
  // Not diagnostic: the accessory blueprints automate on it (docs/06).
  return entityDiscoveryJson(e);
}

std::string outdoorSourceDiscoveryJson() {
  EntitySpec e;
  e.name = "SlyTherm Outdoor Source";
  e.uniqueId = "slytherm_outdoor_source";
  e.stateTopic = topic::kStateOutdoorSource;
  e.diagnostic = true;
  return entityDiscoveryJson(e);
}

std::string fusionDiscoveryJson() {
  EntitySpec e;
  e.name = "SlyTherm Fusion";
  e.uniqueId = "slytherm_fusion";
  e.stateTopic = topic::kStateFusion;
  e.unit = "°C";
  e.deviceClass = "temperature";
  e.valueTemplate = "{{ value_json.temp }}";
  e.jsonAttributesTopic = topic::kStateFusion;
  e.diagnostic = true;  // the climate entity already carries current_temp
  return entityDiscoveryJson(e);
}

std::string changeoverReasonDiscoveryJson() {
  EntitySpec e;
  e.name = "SlyTherm Changeover Reason";
  e.uniqueId = "slytherm_changeover_reason";
  e.stateTopic = topic::kStateChangeoverReason;
  e.diagnostic = true;
  return entityDiscoveryJson(e);
}

std::string sensorAgeDiscoveryJson(const std::string& sensorId) {
  EntitySpec e;
  e.name = "SlyTherm Sensor " + sensorId + " Age";
  e.uniqueId = "slytherm_sensor_" + sensorId + "_age";
  e.stateTopic = sensorAgeStateTopic(sensorId);
  e.unit = "s";
  e.diagnostic = true;
  return entityDiscoveryJson(e);
}

std::string sensorOffsetDiscoveryJson(const std::string& sensorId) {
  EntitySpec e;
  e.name = "SlyTherm Sensor " + sensorId + " Offset";
  e.uniqueId = "slytherm_sensor_" + sensorId + "_offset";
  e.stateTopic = sensorOffsetStateTopic(sensorId);
  e.commandTopic = sensorOffsetCommandTopic(sensorId);
  e.unit = "°C";
  e.config = true;
  e.hasRange = true;
  e.minV = -kSensorOffsetMaxC;
  e.maxV = kSensorOffsetMaxC;
  e.stepV = 0.1f;
  return entityDiscoveryJson(e);
}

std::string sensorParticipatingDiscoveryJson(const std::string& sensorId) {
  EntitySpec e;
  e.name = "SlyTherm Sensor " + sensorId + " Participating";
  e.uniqueId = "slytherm_sensor_" + sensorId + "_participating";
  e.stateTopic = sensorParticipatingStateTopic(sensorId);
  e.diagnostic = true;
  e.binary = true;
  return entityDiscoveryJson(e);
}

}  // namespace hamqtt
}  // namespace dettson
