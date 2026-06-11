// HaMqtt.h — Home Assistant MQTT contract for the Dettson thermostat.
//
// Pure string/JSON builders, topic constants, and inbound-payload parsers
// mirroring docs/06-home-assistant.md. NO network code — the Arduino
// PubSubClient glue lives in src/ and calls into this module.
//
// Safety (docs/04-safety.md): HA is a comfort/visibility layer, never a
// safety layer. Every inbound parser here returns a validated-or-rejected
// result; a rejected payload must never mutate controller state. Loss of
// MQTT/HA is handled elsewhere (fallback profile) — nothing in this module
// raises demand.
//
// Pure C++17, no Arduino dependencies, host-testable.

#pragma once
#include <cstdint>
#include <string>

namespace dettson {
namespace hamqtt {

// ---------- Topic map (docs/06 "Topic map") ----------
namespace topic {

// HA -> ESP32 (commands)
constexpr const char* kCmdSetpoint       = "dettson/cmd/setpoint";
constexpr const char* kCmdTargetTempLow  = "dettson/cmd/target_temp_low";
constexpr const char* kCmdTargetTempHigh = "dettson/cmd/target_temp_high";
constexpr const char* kCmdMode           = "dettson/cmd/mode";
constexpr const char* kCmdFanMode        = "dettson/cmd/fan_mode";
constexpr const char* kCmdPreset         = "dettson/cmd/preset";

// HA -> ESP32 (remote-sensor contract; see sensorStateTopic() for per-id state)
constexpr const char* kConfigSensors     = "dettson/config/sensors";  // retained roster

// ESP32 -> HA (state)
constexpr const char* kStateCurrentTemp     = "dettson/state/current_temp";
constexpr const char* kStateSetpoint        = "dettson/state/setpoint";
constexpr const char* kStateTargetTempLow   = "dettson/state/target_temp_low";
constexpr const char* kStateTargetTempHigh  = "dettson/state/target_temp_high";
constexpr const char* kStateMode            = "dettson/state/mode";
constexpr const char* kStateFanMode         = "dettson/state/fan_mode";
constexpr const char* kStatePreset          = "dettson/state/preset";
constexpr const char* kStateAction          = "dettson/state/action";
constexpr const char* kStateActiveEquipment = "dettson/state/active_equipment";
constexpr const char* kStateModulation      = "dettson/state/modulation";
constexpr const char* kStateOutdoorTemp     = "dettson/state/outdoor_temp";
constexpr const char* kStateOutdoorSource   = "dettson/state/outdoor_source";
constexpr const char* kStateFusion          = "dettson/state/fusion";
constexpr const char* kStateCompressorMinOffRemaining =
    "dettson/state/compressor_min_off_remaining";
constexpr const char* kStateCompressorLockedOut = "dettson/state/compressor_locked_out";
constexpr const char* kStateChangeoverReason    = "dettson/state/changeover_reason";
constexpr const char* kStateFault               = "dettson/state/fault";
constexpr const char* kAvailability             = "dettson/availability";  // LWT = offline

// Diagnostic entities listed in docs/06 "Entities" without an explicit topic-map
// row; they follow the same dettson/state/ pattern.
constexpr const char* kStateBlower    = "dettson/state/blower";
constexpr const char* kStateHealth    = "dettson/state/health";
constexpr const char* kStateLastError = "dettson/state/last_error";

constexpr const char* kDiscoveryPrefix = "homeassistant";

}  // namespace topic

// Per-id topic helpers.
std::string sensorStateTopic(const std::string& sensorId);  // dettson/sensors/<id>/state
std::string sensorAgeStateTopic(const std::string& sensorId);  // dettson/state/sensor/<id>/age
std::string sensorParticipatingStateTopic(const std::string& sensorId);
// homeassistant/<component>/dettson/<objectId>/config
std::string discoveryTopic(const char* component, const std::string& objectId);

// ---------- Fixed payload strings ----------
namespace payload {
constexpr const char* kOnline  = "online";
constexpr const char* kOffline = "offline";
constexpr const char* kOn      = "ON";   // HA binary_sensor default payloads
constexpr const char* kOff     = "OFF";
}  // namespace payload

// Climate entity limits from the docs/06 discovery sketch (not yet in
// DettsonConfig.h — promotion candidates if other modules need them).
constexpr float kClimateMinTempC  = 10.0f;
constexpr float kClimateMaxTempC  = 30.0f;
constexpr float kClimateTempStepC = 0.5f;

// ---------- Inbound command enums (exact lowercase wire strings) ----------
enum class Mode : uint8_t { kOff, kHeat, kCool, kHeatCool };
enum class FanMode : uint8_t { kAuto, kOn, kCirculate };
enum class Preset : uint8_t { kHome, kAway, kSleep };

const char* toString(Mode m);
const char* toString(FanMode f);
const char* toString(Preset p);

template <typename T>
struct Parsed {
  bool ok = false;
  T value{};
};

// Strict float parse: leading/trailing whitespace allowed, otherwise the whole
// payload must be a finite number within [minC, maxC]. NaN/inf/junk/partial
// numbers are rejected. Rejection never carries a value the caller should use.
Parsed<float> parseSetpoint(const char* payloadStr,
                            float minC = kClimateMinTempC,
                            float maxC = kClimateMaxTempC);
Parsed<Mode> parseMode(const char* payloadStr);
Parsed<FanMode> parseFanMode(const char* payloadStr);
Parsed<Preset> parsePreset(const char* payloadStr);

// ---------- Remote-sensor JSON contract (docs/06 "Remote sensors") ----------
// {"temp": C, "occ": bool|null, "bat": 0-100|null, "hum": %|null}
struct SensorReading {
  bool  hasTemp = false;
  float tempC = 0.0f;
  bool  hasOcc = false;
  bool  occupied = false;
  bool  hasBat = false;
  uint8_t batteryPct = 0;
  bool  hasHum = false;
  float humidityPct = 0.0f;
};

// Tolerant flat-object scanner, NOT a general JSON parser. Known limits:
//  - flat objects only (no nested objects/arrays, no unicode escapes);
//  - a quoted key appearing inside a *string value* would be matched —
//    acceptable because all contract values are number/bool/null;
//  - unknown extra keys are ignored.
// "temp" is required and must be a finite number, else returns false (the
// reading is unusable; caller treats it as a missing sample — never demand).
// occ/bat/hum: missing, null, or malformed/out-of-range -> field marked absent.
// Range gating of temp itself (5-40 C etc.) belongs to SensorFusion.
bool parseSensorJson(const char* json, SensorReading& out);

// ---------- JSON building ----------
// Escapes ", \, and control characters for embedding in a JSON string.
std::string jsonEscape(const std::string& s);

// MQTT Discovery payload for climate.dettson_hvac (docs/06 sketch, complete).
std::string climateDiscoveryJson();

// Discovery payloads for the diagnostic entities (docs/06 "Entities" table).
std::string activeEquipmentDiscoveryJson();      // sensor
std::string modulationDiscoveryJson();           // sensor, %
std::string blowerDiscoveryJson();               // sensor
std::string faultDiscoveryJson();                // sensor
std::string healthDiscoveryJson();               // binary_sensor, device_class problem
std::string lastErrorDiscoveryJson();            // sensor, entity_category diagnostic
std::string compressorMinOffRemainingDiscoveryJson();  // sensor, s, diagnostic
std::string compressorLockedOutDiscoveryJson();        // binary_sensor, diagnostic
std::string sensorAgeDiscoveryJson(const std::string& sensorId);            // diagnostic
std::string sensorParticipatingDiscoveryJson(const std::string& sensorId);  // diagnostic

}  // namespace hamqtt
}  // namespace dettson
