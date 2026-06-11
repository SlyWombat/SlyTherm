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
#include <vector>

#include "DettsonConfig.h"

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
constexpr const char* kCmdHold           = "dettson/cmd/hold";  // hold type or "clear"
constexpr const char* kCmdEmHeat         = "dettson/cmd/em_heat";  // switch "ON"/"OFF" (G15)

// HA -> ESP32 (remote-sensor contract; see sensorStateTopic() for per-id state)
constexpr const char* kConfigSensors     = "dettson/config/sensors";  // retained roster
constexpr const char* kConfigPresets     = "dettson/config/presets";  // retained roster

// ESP32 -> HA (state)
constexpr const char* kStateCurrentTemp     = "dettson/state/current_temp";
constexpr const char* kStateSetpoint        = "dettson/state/setpoint";
constexpr const char* kStateTargetTempLow   = "dettson/state/target_temp_low";
constexpr const char* kStateTargetTempHigh  = "dettson/state/target_temp_high";
constexpr const char* kStateMode            = "dettson/state/mode";
constexpr const char* kStateFanMode         = "dettson/state/fan_mode";
constexpr const char* kStatePreset          = "dettson/state/preset";
constexpr const char* kStateHold            = "dettson/state/hold";  // {type, remaining}
constexpr const char* kStateAction          = "dettson/state/action";
constexpr const char* kStateActiveEquipment = "dettson/state/active_equipment";
constexpr const char* kStateModulation      = "dettson/state/modulation";
constexpr const char* kStateOutdoorTemp     = "dettson/state/outdoor_temp";
constexpr const char* kStateOutdoorSource   = "dettson/state/outdoor_source";
constexpr const char* kStateFusion          = "dettson/state/fusion";
constexpr const char* kStateCompressorMinOffRemaining =
    "dettson/state/compressor_min_off_remaining";
constexpr const char* kStateCompressorLockedOut = "dettson/state/compressor_locked_out";
constexpr const char* kStateEmHeat              = "dettson/state/em_heat";  // "ON"/"OFF"
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
std::string sensorOffsetCommandTopic(const std::string& sensorId);  // dettson/cmd/sensor/<id>/offset
std::string sensorOffsetStateTopic(const std::string& sensorId);    // dettson/state/sensor/<id>/offset

// Sensor id of the local DS18B20 fallback for the per-id helpers above (its
// calibration offset is exposed like any remote's — docs/07 G6).
constexpr const char* kLocalSensorId = "local";
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
// HoldType wire strings: none / until_next_preset / two_hours / four_hours /
// indefinite ("none" is state-only — commands clear via "clear").
const char* toString(HoldType t);

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
// dettson/cmd/sensor/<id>/offset: same strict-float rules, bounded to
// ±kSensorOffsetMaxC. Out-of-range is rejected (never clamped here);
// SensorFusion::setSensorOffsetC clamps again defensively.
Parsed<float> parseSensorOffset(const char* payloadStr);
Parsed<Mode> parseMode(const char* payloadStr);
Parsed<FanMode> parseFanMode(const char* payloadStr);
Parsed<Preset> parsePreset(const char* payloadStr);

// dettson/cmd/hold: a hold-type wire string starts a hold, "clear" ends one.
struct HoldCommand {
  bool clear = false;
  HoldType type = HoldType::kNone;  // valid when !clear
};
Parsed<HoldCommand> parseHoldCommand(const char* payloadStr);

// dettson/cmd/em_heat (docs/07 G15): exactly "ON"/"OFF" (HA switch defaults).
// EM HEAT is a dedicated switch, NOT an hvac mode (HA's climate schema only
// accepts the standard modes — "emergency_heat" would be rejected) and NOT a
// preset (the next scheduled comfort-preset write must never silently
// disengage it). true = engage -> ModeStateMachine::setEmergencyHeat(true);
// while engaged the mode state topic keeps reporting "heat".
Parsed<bool> parseEmHeatCommand(const char* payloadStr);

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

// ---------- Preset roster config (docs/06 "Schedules and presets") ----------
// Retained JSON at dettson/config/presets:
//   {"presets":[{"name":"home","heat":21.0,"cool":25.0}, ...]}
struct PresetEntry {
  std::string name;
  float heatC = 0.0f;
  float coolC = 0.0f;
};

// Same tolerance philosophy as parseSensorJson (and the same scanner limits):
// returns false (out empty) when the payload is not an object with a
// "presets" array; individual entries that are invalid — missing/empty/
// duplicate/over-long (> kPresetNameMaxLen) name, missing or out-of-climate-
// range heat/cool — are skipped, and the list is capped at kMaxPresets.
bool parsePresetRosterJson(const char* json, std::vector<PresetEntry>& out);

// ---------- Sensor roster config (docs/06 "Remote sensors") ----------
// Retained JSON at dettson/config/sensors:
//   {"sensors":[{"id":"kitchen","max_age_s":300,"offset":-0.5}, ...]}
constexpr size_t kSensorRosterMax = 16;  // SensorFusion participant-mask width

struct SensorRosterEntry {
  std::string id;
  bool hasMaxAge = false;
  uint32_t maxAgeS = 0;   // valid when hasMaxAge; SensorFusion clamps to 180-900
  float offsetC = 0.0f;   // optional "offset": default 0, clamped ±kSensorOffsetMaxC
};

// Same tolerance philosophy and scanner limits as parsePresetRosterJson:
// returns false (out empty) unless the payload is an object with a "sensors"
// array. Entries with a missing/empty/duplicate id are skipped; "max_age_s"
// is kept only when a positive number; "offset" missing/null/malformed -> 0.
bool parseSensorRosterJson(const char* json, std::vector<SensorRosterEntry>& out);

// ---------- JSON building ----------
// Escapes ", \, and control characters for embedding in a JSON string.
std::string jsonEscape(const std::string& s);

// MQTT Discovery payload for climate.dettson_hvac (docs/06 sketch, complete).
// preset_modes is built from the configured roster; default = boot roster.
std::string climateDiscoveryJson(
    const std::vector<std::string>& presetModes = {"home", "away", "sleep"});

// dettson/state/hold payload: {"type":"two_hours","remaining":7032}
std::string holdStateJson(HoldType t, uint32_t remainingS);

// Discovery payloads for the diagnostic entities (docs/06 "Entities" table).
std::string activeEquipmentDiscoveryJson();      // sensor
std::string modulationDiscoveryJson();           // sensor, %
std::string blowerDiscoveryJson();               // sensor
std::string faultDiscoveryJson();                // sensor
std::string healthDiscoveryJson();               // binary_sensor, device_class problem
std::string lastErrorDiscoveryJson();            // sensor, entity_category diagnostic
std::string compressorMinOffRemainingDiscoveryJson();  // sensor, s, diagnostic
std::string compressorLockedOutDiscoveryJson();        // binary_sensor, diagnostic
std::string holdDiscoveryJson();                       // sensor, diagnostic (type + attrs)
std::string emHeatDiscoveryJson();                     // switch component (G15)
std::string sensorAgeDiscoveryJson(const std::string& sensorId);            // diagnostic
std::string sensorParticipatingDiscoveryJson(const std::string& sensorId);  // diagnostic
// number entity, entity_category config, ±kSensorOffsetMaxC, 0.1 step (G6);
// also used for the local fallback sensor via kLocalSensorId.
std::string sensorOffsetDiscoveryJson(const std::string& sensorId);

}  // namespace hamqtt
}  // namespace dettson
