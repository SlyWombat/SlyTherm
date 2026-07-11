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
// #143: retained energy prices for the economic switchover — HA publishes
// from its energy config (or a TOU automation) so prices survive our reboots
// at the broker AND in NVS; see parseEnergyPricesJson().
constexpr const char* kCmdEnergyPrices   = SLYTHERM_TOPIC_PREFIX "cmd/energy_prices";

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
// #143: retained record-only COP-proxy telemetry (CopLearner::proxyJson()).
constexpr const char* kStateCopProxy            = SLYTHERM_TOPIC_PREFIX "state/cop_proxy";
constexpr const char* kAvailability             = SLYTHERM_TOPIC_PREFIX "availability";  // LWT = offline

// Diagnostic entities listed in docs/06 "Entities" without an explicit topic-map
// row; they follow the same slytherm/state/ pattern.
constexpr const char* kStateBlower    = SLYTHERM_TOPIC_PREFIX "state/blower";
constexpr const char* kStateHealth    = SLYTHERM_TOPIC_PREFIX "state/health";
constexpr const char* kStateLastError = SLYTHERM_TOPIC_PREFIX "state/last_error";

// ---------- Remote link (issue #104; docs/11-remote-node-plan.md) ----------
// Controller-internal protocol for one or more Remote (ESP32-P4 wall unit)
// nodes — never the HA contract. Used only when the Controller firmware is
// built with -DSLYTHERM_REMOTE_LINK (src/); the topic/JSON contract itself
// lives here unconditionally since it is pure Tier-1 logic.
//
// PubSubClient supports exactly one Will per connection, and it is already
// bound to kAvailability (see main_thermostat.cpp). So kControllerStatus
// carries IDENTITY ONLY (cid + version), retained, republished on every
// connect — it has NO Will/offline half. A Remote's LIVENESS signal is
// kAvailability (existing online/offline + LWT), not this topic; a Remote
// binds its cid here on first sight and then tracks liveness via
// kAvailability. (This is a deliberate, documented deviation from the
// per-cid-topic-with-its-own-LWT sketch in docs/11 — PubSubClient cannot
// support a second Will without a second broker connection.)
constexpr const char* kControllerStatus = SLYTHERM_TOPIC_PREFIX "controller/status";
// Retained authoritative echo (DisplayState subset). ONE shared topic, not
// per-Remote: every Remote mirrors the same Controller-owned setpoint/mode/
// hold/preset/fused-temp truth (hub-and-spoke, docs/11 "Authority"), so there
// is nothing to distinguish per Remote. A reconnecting Remote reads this
// retained message and restores instantly.
constexpr const char* kRemoteState = SLYTHERM_TOPIC_PREFIX "remote/state";
// Remote -> Controller UiIntent, per Remote: slytherm/remote/<id>/intent
// (NOT retained — intents are live-only, docs/11 "NO intent queuing").
// Subscribe with the wildcard "slytherm/remote/+/intent".
constexpr const char* kRemoteIntentTopicPrefix = SLYTHERM_TOPIC_PREFIX "remote/";
constexpr const char* kRemoteIntentTopicSuffix = "/intent";
constexpr const char* kRemoteIntentSubscribeWildcard = SLYTHERM_TOPIC_PREFIX "remote/+/intent";

// ---------- OTA (issues #61/#65; docs/10) ----------
// Live client status (NOT retained — stale "downloading" after a reboot would
// mislead; the client republishes on every MQTT reconnect). Payload:
// otaStateJson() below.
constexpr const char* kStateOta = SLYTHERM_TOPIC_PREFIX "state/ota";
// Commands: check the catalog now / apply the staged-or-available update.
// Payload is ignored (any message triggers); both are no-ops while an OTA
// phase is already in flight.
constexpr const char* kCmdOtaCheck = SLYTHERM_TOPIC_PREFIX "cmd/ota_check";
constexpr const char* kCmdOtaApply = SLYTHERM_TOPIC_PREFIX "cmd/ota_apply";

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

// ---------- Retained presence JSON (issue #88) ----------
// {"occupied": <bool>, "last_seen": <unix seconds>}
// Published RETAINED per room on slytherm/sensors/<id>/presence, so a thermostat
// that reboots/reconnects seeds the last-known home/away state immediately.
// last_seen is parsed as a full-precision integer (a float would round ~1.7e9 to
// the nearest ~128 s). Missing/malformed fields are marked absent; returns true
// if the object carries either field.
struct PresenceReading {
  bool     hasOccupied = false;
  bool     occupied    = false;
  bool     hasLastSeen = false;
  uint32_t lastSeen    = 0;   // unix seconds; caller converts to its own timebase
};
bool parsePresenceJson(const char* json, PresenceReading& out);

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

// ---------- Energy prices (issue #143; docs/13 §1) ----------
// slytherm/cmd/energy_prices (RETAINED): {"elecKwh":0.15,"gasM3":0.45} —
// ALL-IN marginal $/kWh electricity and $/m3 gas (Ontario bills in m3; a
// $/therm price converts via ÷kGasM3PerTherm, see DettsonConfig.h). Both keys
// REQUIRED, each a finite number in (0, kEnergyPriceMax]. Same scanner limits
// as parseSensorJson; unknown extra keys ignored; rejection leaves out zeroed
// — a rejected payload must never move the switchover point.
struct EnergyPrices {
  float elecPerKwh = 0.0f;
  float gasPerM3 = 0.0f;
};
bool parseEnergyPricesJson(const char* json, EnergyPrices& out);

// ---------- Remote UiIntent (issue #104; docs/11 "Override-with-hold flow") ----------
// Wire mirror of ui::IntentType / ui::UiIntent (lib/UiModel) — HaMqtt must not
// depend on the UiModel lib (same isolation rule as the LockState/LockLevel
// mirrors above); the src/ glue maps this struct onto ui::UiIntent /
// ModeStateMachine calls. Vacation intents are out of scope for #104 (no
// Remote consumer yet); extend here when that lands.
enum class RemoteIntentType : uint8_t {
  kSetpoints, kMode, kPreset, kHold, kClearHold,
  // #118: vacation + alarm-ack round-trip from a Remote (same re-validated
  // control-task path as the local UI's intents).
  kVacation, kClearVacation, kAckAlarms
};

struct RemoteIntent {
  uint32_t id = 0;   // Remote-local monotonic sequence number, for dedupe;
                     // MUST start at 1 (0 is rejected — reserved as the
                     // Controller-side dedupe "unset" sentinel)
  RemoteIntentType type = RemoteIntentType::kSetpoints;
  float heatC = 0.0f;  // kSetpoints
  float coolC = 0.0f;  // kSetpoints
  Mode mode = Mode::kOff;   // kMode
  std::string preset;       // kPreset
  HoldType hold = HoldType::kNone;  // kHold
  uint16_t vacStartDays = 0;  // kVacation: start offset in days (0-30)
  uint16_t vacNights = 1;     // kVacation: length in nights (1-60); eco setpoints ride heatC/coolC
};

// {"id":<uint32>,"type":"setpoints"|"mode"|"preset"|"hold"|"clear_hold", ...}
// extra fields required per type: setpoints->heatC/coolC, mode->mode,
// preset->preset, hold->hold, clear_hold-> none. "id" and "type" are always
// required; "id" must be > 0 (see RemoteIntent::id); an invalid/missing
// type-specific field rejects the whole intent (never a half-applied one) —
// same tolerance philosophy as the other flat-object parsers in this file.
bool parseRemoteIntentJson(const char* json, RemoteIntent& out);

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

// ---------- Remote link JSON (issue #104) ----------
// slytherm/controller/status payload (retained, republished on every
// connect): {"cid":"8d82f4","status":"online","version":"Jul  7 2026"}.
// version is informational only (currently the firmware build timestamp).
std::string controllerStatusJson(const std::string& cid, bool online,
                                  const std::string& version);

// slytherm/state/ota payload (#61):
//   {"state":"downloading","progress":42,"running":"0.3.0",
//    "available":"0.4.0","error":""}
// state ∈ idle|checking|up_to_date|update_available|downloading|verifying|
// staged|rebooting|failed|rolled_back; "available"/"error" are "" when not
// applicable; progress is 0-100 (download phase only, else 0).
std::string otaStateJson(const char* state, uint8_t progressPct,
                          const std::string& runningVersion,
                          const std::string& availableVersion,
                          const std::string& error);

// slytherm/remote/state payload (retained authoritative echo):
//   {"heatC":21.0,"coolC":25.0,"mode":"heat","emHeat":false,
//    "hold":"two_hours","holdRemainS":7032,"activePreset":"home",
//    "fusedTempC":21.3,"fusedTempValid":true}
// Mirrors docs/11 "Authority": setpoints/mode/hold/active preset/fused-temp
// are exactly what the Controller owns and a Remote must reconcile to.
// #116 additions: action/equipment reuse the exact wire strings of
// slytherm/state/action + state/active_equipment; up to two raw alarm texts
// (the Remote's UI applies friendlyAlarm() at render, same as the Controller
// panel); #118 adds the vacation banner. A Remote parses ALL of these as
// OPTIONAL (defaults idle/idle/0/none) so a new Remote accepts an old
// Controller's echo during a mixed-version OTA window.
std::string remoteStateJson(float heatC, float coolC, Mode mode, bool emHeat,
                             HoldType holdType, uint32_t holdRemainS,
                             const std::string& activePreset,
                             float fusedTempC, bool fusedTempValid,
                             const char* action = "idle",
                             const char* equipment = "idle",
                             uint8_t alarmN = 0,
                             const char* alarm1 = "", const char* alarm2 = "",
                             bool vacationActive = false,
                             const char* vacBanner = "");

// #123/#145: retained boot/crash telemetry (slytherm/boot, slytherm/remote/<id>/boot):
//   {"reason":"panic","coredump":true,"prevUptimeS":8130,"version":"0.4.3","bootCount":17,
//    "rawReason":4,"rtcReason0":1,"rtcReason1":14,
//    "lastAliveUptimeS":28458,"lastAliveEpoch":1783148392,"uptimeS":3}
// reason is the fixed esp_reset_reason() mapping in src/boot_guard.cpp;
// prevUptimeS==0 means unknown (RTC lost, e.g. true power cycle). The #145
// fields survive that case: lastAlive* is the NVS heartbeat from the previous
// run (0 = unknown/no heartbeat yet), rawReason/rtcReason* are the numeric
// esp_reset_reason() and per-CPU ROM reset codes, and uptimeS is stamped at
// PUBLISH time — the topic is republished retained on every MQTT reconnect,
// so uptimeS well above 0 marks a reconnect echo, not a fresh boot.
struct BootStatus {
  const char* reason = "unknown";
  bool coredump = false;
  uint32_t prevUptimeS = 0;
  const char* version = "";
  uint32_t bootCount = 0;
  uint32_t rawReason = 0;         // esp_reset_reason() numeric
  uint32_t rtcReason0 = 0;        // ROM reset reason, CPU0
  uint32_t rtcReason1 = 0;        // ROM reset reason, CPU1
  uint32_t lastAliveUptimeS = 0;  // previous run's last NVS heartbeat
  uint32_t lastAliveEpoch = 0;    // wall clock of that heartbeat (0 = no NTP)
  uint32_t uptimeS = 0;           // THIS run's uptime at publish time
};
std::string bootStatusJson(const BootStatus& s);

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
