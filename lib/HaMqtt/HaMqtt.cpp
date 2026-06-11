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
  return "dettson/sensors/" + sensorId + "/state";
}

std::string sensorAgeStateTopic(const std::string& sensorId) {
  return "dettson/state/sensor/" + sensorId + "/age";
}

std::string sensorParticipatingStateTopic(const std::string& sensorId) {
  return "dettson/state/sensor/" + sensorId + "/participating";
}

std::string discoveryTopic(const char* component, const std::string& objectId) {
  return std::string(topic::kDiscoveryPrefix) + "/" + component + "/dettson/" +
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

const char* toString(Preset p) {
  switch (p) {
    case Preset::kHome: return "home";
    case Preset::kAway: return "away";
    case Preset::kSleep: return "sleep";
  }
  return "home";
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

Parsed<Preset> parsePreset(const char* payloadStr) {
  Parsed<Preset> r;
  if (payloadStr == nullptr) return r;
  if (std::strcmp(payloadStr, "home") == 0) { r.ok = true; r.value = Preset::kHome; }
  else if (std::strcmp(payloadStr, "away") == 0) { r.ok = true; r.value = Preset::kAway; }
  else if (std::strcmp(payloadStr, "sleep") == 0) { r.ok = true; r.value = Preset::kSleep; }
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

std::string deviceJson() {
  return Obj()
      .raw("identifiers", strList({"dettson_esp32"}))
      .str("name", "Dettson ClimateTalk Thermostat")
      .str("manufacturer", "ElectricRV")
      .str("model", "ESP32-S3 CT-485")
      .close();
}

struct EntitySpec {
  std::string name;
  std::string uniqueId;
  std::string stateTopic;
  const char* unit = nullptr;
  const char* deviceClass = nullptr;
  bool diagnostic = false;
  bool binary = false;  // adds payload_on/payload_off ON/OFF
};

std::string entityDiscoveryJson(const EntitySpec& e) {
  Obj o;
  o.str("name", e.name).str("unique_id", e.uniqueId).str("state_topic", e.stateTopic);
  if (e.unit != nullptr) o.str("unit_of_measurement", e.unit);
  if (e.deviceClass != nullptr) o.str("device_class", e.deviceClass);
  if (e.diagnostic) o.str("entity_category", "diagnostic");
  if (e.binary) o.str("payload_on", payload::kOn).str("payload_off", payload::kOff);
  o.str("availability_topic", topic::kAvailability)
      .str("payload_available", payload::kOnline)
      .str("payload_not_available", payload::kOffline)
      .raw("device", deviceJson());
  return o.close();
}

}  // namespace

std::string climateDiscoveryJson() {
  return Obj()
      .str("name", "Dettson HVAC")
      .str("unique_id", "dettson_hvac")
      .raw("modes", strList({"off", "heat", "cool", "heat_cool"}))
      .raw("fan_modes", strList({"auto", "on", "circulate"}))
      .raw("preset_modes", strList({"home", "away", "sleep"}))
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
  e.name = "Dettson Active Equipment";
  e.uniqueId = "dettson_active_equipment";
  e.stateTopic = topic::kStateActiveEquipment;
  return entityDiscoveryJson(e);
}

std::string modulationDiscoveryJson() {
  EntitySpec e;
  e.name = "Dettson Gas Modulation";
  e.uniqueId = "dettson_modulation";
  e.stateTopic = topic::kStateModulation;
  e.unit = "%";
  return entityDiscoveryJson(e);
}

std::string blowerDiscoveryJson() {
  EntitySpec e;
  e.name = "Dettson Blower";
  e.uniqueId = "dettson_blower";
  e.stateTopic = topic::kStateBlower;
  return entityDiscoveryJson(e);
}

std::string faultDiscoveryJson() {
  EntitySpec e;
  e.name = "Dettson Fault";
  e.uniqueId = "dettson_fault";
  e.stateTopic = topic::kStateFault;
  return entityDiscoveryJson(e);
}

std::string healthDiscoveryJson() {
  EntitySpec e;
  e.name = "Dettson Health";
  e.uniqueId = "dettson_health";
  e.stateTopic = topic::kStateHealth;
  e.deviceClass = "problem";
  e.binary = true;
  return entityDiscoveryJson(e);
}

std::string lastErrorDiscoveryJson() {
  EntitySpec e;
  e.name = "Dettson Last Error";
  e.uniqueId = "dettson_last_error";
  e.stateTopic = topic::kStateLastError;
  e.diagnostic = true;
  return entityDiscoveryJson(e);
}

std::string compressorMinOffRemainingDiscoveryJson() {
  EntitySpec e;
  e.name = "Dettson Compressor Min-Off Remaining";
  e.uniqueId = "dettson_compressor_min_off_remaining";
  e.stateTopic = topic::kStateCompressorMinOffRemaining;
  e.unit = "s";
  e.diagnostic = true;
  return entityDiscoveryJson(e);
}

std::string compressorLockedOutDiscoveryJson() {
  EntitySpec e;
  e.name = "Dettson Compressor Locked Out";
  e.uniqueId = "dettson_compressor_locked_out";
  e.stateTopic = topic::kStateCompressorLockedOut;
  e.diagnostic = true;
  e.binary = true;
  return entityDiscoveryJson(e);
}

std::string sensorAgeDiscoveryJson(const std::string& sensorId) {
  EntitySpec e;
  e.name = "Dettson Sensor " + sensorId + " Age";
  e.uniqueId = "dettson_sensor_" + sensorId + "_age";
  e.stateTopic = sensorAgeStateTopic(sensorId);
  e.unit = "s";
  e.diagnostic = true;
  return entityDiscoveryJson(e);
}

std::string sensorParticipatingDiscoveryJson(const std::string& sensorId) {
  EntitySpec e;
  e.name = "Dettson Sensor " + sensorId + " Participating";
  e.uniqueId = "dettson_sensor_" + sensorId + "_participating";
  e.stateTopic = sensorParticipatingStateTopic(sensorId);
  e.diagnostic = true;
  e.binary = true;
  return entityDiscoveryJson(e);
}

}  // namespace hamqtt
}  // namespace dettson
