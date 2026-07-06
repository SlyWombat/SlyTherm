// HaMqtt.h — Home Assistant MQTT contract for the SlyTherm thermostat.
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

// ---------- Product topic namespace (issue #79) ----------
// The ONE place the brand/topic prefix is defined. SlyTherm is the product;
// Dettson is a device profile (C105-MV, CT-485/ClimateTalk) behind it, not the
// product name. A second furnace/HP brand is a new device profile — NOT a new
// prefix. Every topic constant + per-id topic builder derives from this.
//
// MIGRATION (breaking): retained topics move dettson/* -> slytherm/*. Publish
// nothing under the old prefix; clear the old retained topics at the broker and
// delete+re-discover the HA device (HA will not rename existing entities).
#define SLYTHERM_TOPIC_PREFIX "slytherm/"

// ---------- Topic map (docs/06 "Topic map") ----------
namespace topic {

constexpr const char* kTopicPrefix = SLYTHERM_TOPIC_PREFIX;  // single-source brand prefix

// HA -> ESP32 (commands)
constexpr const char* kCmdSetpoint       = SLYTHERM_TOPIC_PREFIX "cmd/setpoint";
constexpr const char* kCmdTargetTempLow  = SLYTHERM_TOPIC_PREFIX "cmd/target_temp_low";
constexpr const char* kCmdTargetTempHigh = SLYTHERM_TOPIC_PREFIX "cmd/target_temp_high";
constexpr const char* kCmdMode           = SLYTHERM_TOPIC_PREFIX "cmd/mode";
constexpr const char* kCmdFanMode        = SLYTHERM_TOPIC_PREFIX "cmd/fan_mode";
constexpr const char* kCmdPreset         = SLYTHERM_TOPIC_PREFIX "cmd/preset";
constexpr const char* kCmdHold           = SLYTHERM_TOPIC_PREFIX "cmd/hold";  // hold type or "clear"
constexpr const char* kCmdEmHeat         = SLYTHERM_TOPIC_PREFIX "cmd/em_heat";  // switch "ON"/"OFF" (G15)
constexpr const char* kCmdLockClear      = SLYTHERM_TOPIC_PREFIX "cmd/lock_clear";  // forgotten-PIN recovery (issue #45)
constexpr const char* kCmdNextTarget     = SLYTHERM_TOPIC_PREFIX "cmd/next_target";  // smart recovery (issue #50)

// HA -> ESP32 (remote-sensor contract; see sensorStateTopic() for per-id state)
constexpr const char* kConfigSensors     = SLYTHERM_TOPIC_PREFIX "config/sensors";  // retained roster
constexpr const char* kConfigPresets     = SLYTHERM_TOPIC_PREFIX "config/presets";  // retained roster

// ESP32 -> HA (state)
constexpr const char* kStateCurrentTemp     = SLYTHERM_TOPIC_PREFIX "state/current_temp";
constexpr const char* kStateSetpoint        = SLYTHERM_TOPIC_PREFIX "state/setpoint";
constexpr const char* kStateTargetTempLow   = SLYTHERM_TOPIC_PREFIX "state/target_temp_low";
constexpr const char* kStateTargetTempHigh  = SLYTHERM_TOPIC_PREFIX "state/target_temp_high";
constexpr const char* kStateMode            = SLYTHERM_TOPIC_PREFIX "state/mode";
constexpr const char* kStateFanMode         = SLYTHERM_TOPIC_PREFIX "state/fan_mode";
constexpr const char* kStatePreset          = SLYTHERM_TOPIC_PREFIX "state/preset";
constexpr const char* kStateHold            = SLYTHERM_TOPIC_PREFIX "state/hold";  // {type, remaining}
constexpr const char* kStateAction          = SLYTHERM_TOPIC_PREFIX "state/action";
constexpr const char* kStateActiveEquipment = SLYTHERM_TOPIC_PREFIX "state/active_equipment";
constexpr const char* kStateModulation      = SLYTHERM_TOPIC_PREFIX "state/modulation";
constexpr const char* kStateOutdoorTemp     = SLYTHERM_TOPIC_PREFIX "state/outdoor_temp";
constexpr const char* kStateOutdoorSource   = SLYTHERM_TOPIC_PREFIX "state/outdoor_source";
constexpr const char* kStateFusion          = SLYTHERM_TOPIC_PREFIX "state/fusion";
constexpr const char* kStateCompressorMinOffRemaining =
    SLYTHERM_TOPIC_PREFIX "state/compressor_min_off_remaining";
constexpr const char* kStateCompressorLockedOut = SLYTHERM_TOPIC_PREFIX "state/compressor_locked_out";
constexpr const char* kStateEmHeat              = SLYTHERM_TOPIC_PREFIX "state/em_heat";  // "ON"/"OFF"
constexpr const char* kStateChangeoverReason    = SLYTHERM_TOPIC_PREFIX "state/changeover_reason";
constexpr const char* kStateLock                = SLYTHERM_TOPIC_PREFIX "state/lock";  // JSON, see lockStateJson()
constexpr const char* kStateFault               = SLYTHERM_TOPIC_PREFIX "state/fault";
constexpr const char* kAvailability             = SLYTHERM_TOPIC_PREFIX "availability";  // LWT = offline

// Diagnostic entities listed in docs/06 "Entities" without an explicit topic-map
// row; they follow the same slytherm/state/ pattern.
constexpr const char* kStateBlower    = SLYTHERM_TOPIC_PREFIX "state/blower";
constexpr const char* kStateHealth    = SLYTHERM_TOPIC_PREFIX "state/health";
constexpr const char* kStateLastError = SLYTHERM_TOPIC_PREFIX "state/last_error";

constexpr const char* kDiscoveryPrefix = "homeassistant";

}  // namespace topic

// Per-id topic helpers.
std::string sensorStateTopic(const std::string& sensorId);  // slytherm/sensors/<id>/state
std::string sensorAgeStateTopic(const std::string& sensorId);  // slytherm/state/sensor/<id>/age
std::string sensorParticipatingStateTopic(const std::string& sensorId);
std::string sensorOffsetCommandTopic(const std::string& sensorId);  // slytherm/cmd/sensor/<id>/offset
std::string sensorOffsetStateTopic(const std::string& sensorId);    // slytherm/state/sensor/<id>/offset

// Sensor id of the local DS18B20 fallback for the per-id helpers above (its
// calibration offset is exposed like any remote's — docs/07 G6).
constexpr const char* kLocalSensorId = "local";
// homeassistant/<component>/slytherm/<objectId>/config
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
// NOTE: there is deliberately no fixed Preset enum here. Presets are
// roster-defined strings (slytherm/config/presets); slytherm/cmd/preset payloads
// are passed through to ModeStateMachine::applyPreset(), which validates the
// name against the configured roster.

const char* toString(Mode m);
const char* toString(FanMode f);
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
// slytherm/cmd/sensor/<id>/offset: same strict-float rules, bounded to
// ±kSensorOffsetMaxC. Out-of-range is rejected (never clamped here);
// SensorFusion::setSensorOffsetC clamps again defensively.
Parsed<float> parseSensorOffset(const char* payloadStr);
Parsed<Mode> parseMode(const char* payloadStr);
Parsed<FanMode> parseFanMode(const char* payloadStr);

// slytherm/cmd/hold: a hold-type wire string starts a hold, "clear" ends one.
struct HoldCommand {
  bool clear = false;
  HoldType type = HoldType::kNone;  // valid when !clear
};
Parsed<HoldCommand> parseHoldCommand(const char* payloadStr);

// slytherm/cmd/em_heat (docs/07 G15): exactly "ON"/"OFF" (HA switch defaults).
// EM HEAT is a dedicated switch, NOT an hvac mode (HA's climate schema only
// accepts the standard modes — "emergency_heat" would be rejected) and NOT a
// preset (the next scheduled comfort-preset write must never silently
// disengage it). true = engage -> ModeStateMachine::setEmergencyHeat(true);
// while engaged the mode state topic keeps reporting "heat".
Parsed<bool> parseEmHeatCommand(const char* payloadStr);

// ---------- Screen lock (issue #45; docs/06 "Screen lock") ----------
// Wire mirror of ui::LockState / ui::LockLevel — HaMqtt must not depend on
// the UiModel lib (same isolation rule as UiModel's deadband duplicate);
// the src/ glue maps between the two enums.
enum class LockState : uint8_t { kUnlocked, kUserLocked, kInstallerLocked };
enum class LockLevel : uint8_t { kSettingsOnly, kSettingsAndSetpoints };
const char* toString(LockState s);  // unlocked / user_locked / installer_locked
const char* toString(LockLevel l);  // settings / settings_setpoints

// slytherm/cmd/lock_clear: clears the user screen PIN — the documented
// forgotten-PIN recovery. No PIN is required: anyone who can publish here
// already has full climate control over MQTT, so HA/broker access = admin
// (rationale in docs/06). Retained-safe by construction:
//   - the payload must be EXACTLY kLockClearPayload (never "ON"/"1"), so no
//     generic retained switch payload can clear the PIN;
//   - an empty payload is rejected — after handling, the glue publishes a
//     retained empty message to the topic, which both deletes any retained
//     copy from the broker and is itself a no-op if replayed;
//   - the installer code is deliberately NOT clearable over MQTT.
constexpr const char* kLockClearPayload = "clear_user_pin";
Parsed<bool> parseLockClearCommand(const char* payloadStr);  // ok => clear

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

// ---------- Smart recovery next-target (docs/06 "Smart recovery") ----------
// slytherm/cmd/next_target: HA publishes the next scheduled preset/setpoint
// change as JSON {"temp": C, "mode": "heat"|"cool", "in_s": seconds}.
// Advisory input to RecoveryEstimator only (same isolation rule as UiModel:
// HaMqtt does not depend on the RecoveryEstimator lib — the src/ glue maps
// this struct across). A rejected payload, or none at all, simply means no
// pre-start recommendation; it can never raise demand on its own.
struct NextTarget {
  float tempC = 0.0f;
  Mode mode = Mode::kHeat;  // only kHeat / kCool are valid on this topic
  uint32_t inS = 0;
};

// Sanity gate on "in_s": a scheduled change more than a week out is junk.
constexpr uint32_t kNextTargetMaxInS = 7 * 86400;

// Same tolerance philosophy and scanner limits as parseSensorJson, but all
// three keys are REQUIRED: temp must be a finite number within the climate
// limits, mode exactly "heat" or "cool" (heat_cool schedules publish the
// side they expect to serve), in_s a number in [0, kNextTargetMaxInS]
// (fraction truncated). Unknown extra keys ignored. Rejection leaves out
// zeroed — never a half-applied target.
bool parseNextTargetJson(const char* json, NextTarget& out);

// ---------- Preset roster config (docs/06 "Schedules and presets") ----------
// Retained JSON at slytherm/config/presets:
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
// Retained JSON at slytherm/config/sensors:
//   {"sensors":[{"id":"kitchen","max_age_s":300,"offset":-0.5}, ...]}
constexpr size_t kSensorRosterMax = 16;  // SensorFusion participant-mask width

struct SensorRosterEntry {
  std::string id;
  std::string name;       // #85: optional free-form display label; empty -> caller falls back to id
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

// MQTT Discovery payload for climate.slytherm_hvac (docs/06 sketch, complete).
// preset_modes is built from the configured roster; default = boot roster.
std::string climateDiscoveryJson(
    const std::vector<std::string>& presetModes = {"home", "away", "sleep"});

// slytherm/state/hold payload: {"type":"two_hours","remaining":7032}
std::string holdStateJson(HoldType t, uint32_t remainingS);

// slytherm/state/lock payload:
//   {"state":"user_locked","level":"settings","pin_set":true}
// pin_set = a user PIN exists (tells HA whether lock_clear has anything to do).
std::string lockStateJson(LockState s, LockLevel l, bool userPinSet);

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
std::string holdSelectDiscoveryJson();                 // select: HA sets/reads the hold duration (#81)
std::string emHeatDiscoveryJson();                     // switch component (G15)
std::string lockDiscoveryJson();                       // sensor, diagnostic (state + attrs)
std::string outdoorTempDiscoveryJson();          // sensor, °C, device_class temperature
std::string outdoorSourceDiscoveryJson();        // sensor, diagnostic: bus/wired/ha/none
// slytherm/state/fusion JSON (docs/06 topic map): state = value_json.temp,
// full payload exposed as attributes (tier, participants, occupied).
std::string fusionDiscoveryJson();               // sensor, °C, diagnostic, JSON attrs
std::string changeoverReasonDiscoveryJson();     // sensor, diagnostic
std::string sensorAgeDiscoveryJson(const std::string& sensorId);            // diagnostic
std::string sensorParticipatingDiscoveryJson(const std::string& sensorId);  // diagnostic
// number entity, entity_category config, ±kSensorOffsetMaxC, 0.1 step (G6);
// also used for the local fallback sensor via kLocalSensorId.
std::string sensorOffsetDiscoveryJson(const std::string& sensorId);

}  // namespace hamqtt
}  // namespace dettson
