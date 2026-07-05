// main_thermostat.cpp — the bootable thermostat application (issue #55).
//
// Wires every lib/ module into the docs/05 Phase 4 pipeline on FreeRTOS:
//
//   core 0: mqttTask    — Wi-Fi + PubSubClient glue for lib/HaMqtt (discovery,
//                         subscriptions, state publishing, LWT availability)
//   core 1: controlTask — fixed-cadence control loop:
//             SensorFusion -> ModeStateMachine (+RecoveryEstimator advisory)
//             -> DualFuelArbiter -> CompressorGuard -> DemandArbiter
//             -> shapers -> actuator; SafetySupervisor reports invariants
//             every cycle and gates the external-WDT pet
//           ct485Task   — Ct485Thermostat protocol state machine; UART glue
//                         stubbed unless -DDETTSON_CT485_UART
//
// ===================== DEMANDS-DISABLED GUARANTEE (default build) ===========
// The default build CANNOT raise a live demand:
//   - the actuator is LoggingActuator: demands are printed, never transmitted;
//   - Ct485Thermostat boots SILENT and is never resume()d without
//     -DDETTSON_CT485_TX_ENABLE, and its Config::offsetVariant is left kUnset,
//     so it refuses demand frames BY DESIGN (docs/02 §5a Phase 2 gate);
//   - no relay GPIO is configured outside -DDETTSON_ACTUATOR_RELAY, and even
//     then pins default to -1 (log only) in thermostat_config.h.
// Phase 3 TX needs all three gates deliberately opened.
// ============================================================================
//
// ----------------------------- BENCH QUICKSTART ----------------------------
// Run the full control loop on a bare DevKitC with MQTT only (no bus, no
// relays, no DS18B20 — the default build):
//   1. cp src/thermostat_secrets.h.example src/thermostat_secrets.h and fill
//      in Wi-Fi + broker credentials (gitignored).
//   2. pio run -e thermostat -t upload && pio device monitor
//   3. Publish a retained sensor roster:
//        mosquitto_pub -r -t dettson/config/sensors \
//          -m '{"sensors":[{"id":"bench","max_age_s":300}]}'
//   4. Simulate a room sensor (repeat at least every 300 s; non-retained):
//        mosquitto_pub -t dettson/sensors/bench/state \
//          -m '{"temp":19.5,"occ":true}'
//      and (optionally) outdoor temp for the dual-fuel arbiter:
//        mosquitto_pub -t dettson/cmd/outdoor_temp -m '5.0'
//   5. Drive it: mosquitto_pub -t dettson/cmd/mode -m heat
//                mosquitto_pub -t dettson/cmd/setpoint -m 21.5
//   6. Watch dettson/state/# — and the serial log, where LoggingActuator
//      prints every demand the pipeline would have emitted. Home Assistant
//      auto-discovers the climate + diagnostic entities (docs/06).
// Without sensor input the boot gate stays closed (boot-to-no-demand,
// docs/04 §3) and the boot-grace alarm raises after 120 s — that is the
// system working as specified, not a bug.
// ----------------------------------------------------------------------------

#include <Arduino.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include <cmath>
#include <cstring>

#include <esp_system.h>
#include <esp_timer.h>
// IDF >= 5 (Arduino core 3.x) moved esp_efuse_mac_get_default() out of
// esp_system.h; older cores have no esp_mac.h at all.
#if __has_include(<esp_mac.h>)
#include <esp_mac.h>
#endif

#include "CompressorGuard.h"
#include "Ct485Core.h"
#include "Ct485Frame.h"
#include "Ct485Thermostat.h"
#include "DemandArbiter.h"
#include "DemandShaper.h"
#include "DettsonConfig.h"
#include "DualFuelArbiter.h"
#include "HaMqtt.h"
#include "ModeStateMachine.h"
#include "OutdoorTempSource.h"
#include "PidShaper.h"
#include "RecoveryEstimator.h"
#include "RelaySequencer.h"
#include "SafetySupervisor.h"
#include "SensorFusion.h"
#include "UiModel.h"

#ifdef DETTSON_UI
#include <ESPmDNS.h>      // mDNS broker auto-discovery (silent home-system connect)
#include "slytherm_ui.h"  // LVGL wall-UI binding (compiled only in env:thermostat_s3)
#include "wifi_prov.h"    // on-device Wi-Fi provisioning (owned by the MQTT task)
#include "mqtt_cfg.h"     // on-device broker provisioning (NVS + mDNS)
#include "telnet_log.h"   // WiFi-accessible debug log (port 23)
#endif

#include "thermostat_config.h"
#if __has_include("thermostat_secrets.h")
#include "thermostat_secrets.h"
#endif
// Absent/partial secrets file -> empty defaults: builds everywhere, Wi-Fi
// skipped (serial-only bench). Matches the sniffer_config.h pattern.
#ifndef THERMOSTAT_WIFI_SSID
#define THERMOSTAT_WIFI_SSID ""
#endif
#ifndef THERMOSTAT_WIFI_PASS
#define THERMOSTAT_WIFI_PASS ""
#endif
#ifndef THERMOSTAT_MQTT_HOST
#define THERMOSTAT_MQTT_HOST ""
#endif
#ifndef THERMOSTAT_MQTT_PORT
#define THERMOSTAT_MQTT_PORT 1883
#endif
#ifndef THERMOSTAT_MQTT_USER
#define THERMOSTAT_MQTT_USER ""
#endif
#ifndef THERMOSTAT_MQTT_PASS
#define THERMOSTAT_MQTT_PASS ""
#endif

#ifdef DETTSON_DS18B20
#include <DallasTemperature.h>
#include <OneWire.h>
#endif

namespace {

using namespace dettson;
namespace cfg = thermostat;
namespace hm = dettson::hamqtt;

// ============================================================================
// Time: monotonic seconds that never go backwards across a reboot.
// CompressorGuard/ResetLoopAccountant blobs need a reboot-surviving timebase
// (docs/04 §2 brownout row). The base is persisted every kClockSaveS; a
// reboot resumes from the saved value, undercounting the off-time spent
// rebooting — conservative (min-off appears less served, never more).
// ============================================================================
uint32_t gClockBaseS = 0;

uint32_t nowSeconds() {
  return gClockBaseS + static_cast<uint32_t>(esp_timer_get_time() / 1000000LL);
}

// ============================================================================
// Shared state between tasks
// ============================================================================
SemaphoreHandle_t gCmdMux;   // Pending mailbox + sensor table
SemaphoreHandle_t gSnapMux;  // control -> MQTT state snapshot
SemaphoreHandle_t gCtMux;    // Ct485Thermostat instance
// Guards gUi across the control task and the wall-UI task (DETTSON_UI). Null
// (no-op) when the UI task isn't built, so all wraps stay compile-safe.
SemaphoreHandle_t gUiMux = nullptr;
inline void uiLock()   { if (gUiMux) xSemaphoreTake(gUiMux, portMAX_DELAY); }
inline void uiUnlock() { if (gUiMux) xSemaphoreGive(gUiMux); }

constexpr size_t kFusionSlots = SensorFusion::kMaxSensors;  // 8: local + 7 remotes
constexpr size_t kSensorNameLen = 24;

struct SensorEntry {
  bool used = false;
  char name[kSensorNameLen] = {};
  bool inRoster = false;
  bool hasMaxAge = false;
  uint32_t maxAgeS = kSensorMaxAgeS;
  float offsetC = 0.0f;
  bool lastOcc = false;  // for the fusion JSON "occupied" field
};
SensorEntry gSensorTable[kFusionSlots];  // index == SensorFusion id; 0 = local

// Command mailbox: MQTT task fills under gCmdMux, control task swaps it out.
struct Pending {
  bool hasMode = false;        hm::Mode mode = hm::Mode::kOff;
  bool hasSetpoint = false;    float setpointC = 0;
  bool hasLow = false;         float lowC = 0;
  bool hasHigh = false;        float highC = 0;
  bool hasFan = false;         hm::FanMode fan = hm::FanMode::kAuto;
  bool hasPreset = false;      char preset[kPresetNameMaxLen + 1] = {};
  bool hasHold = false;        hm::HoldCommand hold;
  bool hasEmHeat = false;      bool emHeat = false;
  bool lockClear = false;
  bool hasTarget = false;      hm::NextTarget target;
  bool hasPresetRoster = false;
  PresetDef presetDefs[kMaxPresets]; size_t presetCount = 0;
  bool sensorRosterDirty = false;
  bool hasOat = false;         float oatC = 0;
  struct OffsetCmd { uint8_t idx; float offsetC; };
  OffsetCmd offsets[8]; size_t offsetCount = 0;
  struct Sample { uint8_t idx; float tempC; int8_t occ; };  // occ: -1 unknown
  Sample samples[32]; size_t sampleCount = 0;
  bool anyInbound = false;  // any accepted MQTT traffic (setpoint-staleness clock)
};
Pending gPending;

// Control -> MQTT snapshot (plain copy under gSnapMux).
struct Snapshot {
  bool tempValid = false;      float tempC = 0;
  float heatSp = kFallbackHeatSetpointC, coolSp = kFallbackCoolSetpointC;
  hm::Mode mode = hm::Mode::kOff;
  bool emHeat = false;
  hm::FanMode fan = hm::FanMode::kAuto;
  char preset[kPresetNameMaxLen + 1] = {};
  HoldType holdType = HoldType::kNone; uint32_t holdRemainS = 0;
  char action[16] = "off";
  char equipment[12] = "idle";
  float modulationPct = 0;
  bool oatValid = false; float oatC = 0; OatRung oatRung = OatRung::kNone;
  char fusionJson[176] = "{}";
  uint32_t compMinOffRemainS = 0;
  bool compLockedOut = false;
  char changeReason[24] = "none";
  bool healthProblem = false;
  char lastError[safety::kAlarmTextLen] = {};
  hm::LockState lockState = hm::LockState::kUnlocked;
  hm::LockLevel lockLevel = hm::LockLevel::kSettingsOnly;
  bool pinSet = false;
  char busJson[176] = "{}";
#ifdef DETTSON_ACTUATOR_RELAY
  char relaysJson[80] = "{}";  // Case B diagnostic (docs/06 topic map)
#endif
  struct SensorPub {
    bool used = false; char name[kSensorNameLen] = {};
    uint32_t ageS = 0; bool participating = false; float offsetC = 0;
  };
  SensorPub sensors[kFusionSlots];
  bool lockTombstone = false;  // publish retained empty to lock_clear (docs/06)
};
Snapshot gSnap;

// MQTT-task-side discovery inputs.
char gPresetNames[kMaxPresets][kPresetNameMaxLen + 1] = {};
size_t gPresetNameCount = 0;
volatile bool gDiscoveryDirty = false;

volatile bool gMqttConnected = false;
volatile bool gWifiConnected = false;

// ============================================================================
// Modules (constructed in setup() once the persisted clock is known)
// ============================================================================
Preferences gPrefs;
SensorFusion        gFusion;
OutdoorTempSource   gOat;
ModeStateMachine*   gModeSm   = nullptr;
DualFuelArbiter*    gDualFuel = nullptr;
CompressorGuard     gGuard;
DemandArbiter*      gArbiter  = nullptr;
PidShaper           gPid;
GasShaper           gGasShaper;
RecoveryEstimator   gRecovery;
ui::UiModel         gUi;
safety::SafetySupervisor* gSup = nullptr;
ct485::Ct485Thermostat*   gCt = nullptr;

// --- CompressorGuard adapters -----------------------------------------------
// Glue-side stop anchor for the min-off-remaining diagnostic and the idle
// gates; starts at boot (prior state unknown -> assume just stopped).
uint32_t gCompStopAnchorS = 0;
uint32_t gBootHoldEndS = 0;

struct GuardGate : public CompressorGate {
  bool canStart(uint32_t nowS) override { return gGuard.requestStart(nowS).allowed; }
  bool canStop(uint32_t nowS) override {
    if (!gGuard.requestStop(nowS, /*safety=*/false).allowed) return false;
    gCompStopAnchorS = nowS;
    return true;
  }
};
GuardGate gGuardGate;
HpRelayShaper gHpShaper(gGuardGate);

bool compressorProvenIdle(uint32_t nowS) {
  return !gGuard.running() &&
         nowS >= gCompStopAnchorS + kCompressorMinOffS &&
         nowS >= gBootHoldEndS;
}

void compressorSafetyStop(uint32_t nowS) {
  if (gGuard.running()) {
    gGuard.requestStop(nowS, /*safety=*/true);
    gCompStopAnchorS = nowS;
  }
  gHpShaper.reset();
}

struct IdleGate : public CompressorIdleGate {
  bool compressorIdle(uint32_t nowS) override { return compressorProvenIdle(nowS); }
};
IdleGate gIdleGate;

// ============================================================================
// Actuator interface — LoggingActuator default; CT-485/relay behind flags.
// ============================================================================
class Actuator {
 public:
  virtual ~Actuator() = default;
  virtual void apply(const DemandSet& s, uint32_t nowS, uint32_t nowMs) = 0;
  virtual void goSilent() = 0;
  virtual const char* name() const = 0;
};

class LoggingActuator : public Actuator {
 public:
  void apply(const DemandSet& s, uint32_t, uint32_t) override {
    if (same(s, last_)) return;
    last_ = s;
    Serial.printf("[actuator/log] gas=%.0f%% hpHeat=%.0f%% cool=%.0f%% fan=%.0f%% temper=%.0f%%\n",
                  s.gasHeatPct, s.hpHeatPct, s.coolPct, s.fanPct, s.defrostTemperPct);
  }
  void goSilent() override {
    if (!silentLogged_) { Serial.println("[actuator/log] goSilent()"); silentLogged_ = true; }
    last_ = DemandSet{};
  }
  const char* name() const override { return "logging"; }

 private:
  static bool same(const DemandSet& a, const DemandSet& b) {
    return a.gasHeatPct == b.gasHeatPct && a.hpHeatPct == b.hpHeatPct &&
           a.coolPct == b.coolPct && a.fanPct == b.fanPct &&
           a.defrostTemperPct == b.defrostTemperPct;
  }
  DemandSet last_;
  bool silentLogged_ = false;
};

#ifdef DETTSON_ACTUATOR_CT485
// Path A: demands routed into Ct485Thermostat. With offsetVariant kUnset
// (the shipped Config) every setDemand() is refused — by design, until the
// docs/02 §5a offsets are capture-confirmed.
class Ct485Actuator : public Actuator {
 public:
  void apply(const DemandSet& s, uint32_t, uint32_t nowMs) override {
    xSemaphoreTake(gCtMux, portMAX_DELAY);
    bool ok = true;
    // Tempering rides the heat channel toward the furnace (docs/04 §2:
    // DEFROST 0x68 is the interface board's, never ours).
    ok &= gCt->setDemand(ct485::DemandChannel::kHeat,
                         fmaxf(s.gasHeatPct, s.defrostTemperPct), nowMs);
    ok &= gCt->setDemand(ct485::DemandChannel::kCool, s.coolPct, nowMs);
    ok &= gCt->setFanDemand(s.fanPct, 0, nowMs);
    xSemaphoreGive(gCtMux);
    if (s.hpHeatPct > 0.0f) {
      // Bus HP-heat command path is a Phase 2 open question (docs/05 Phase 2a).
      logOnce_("hpHeat demand has no confirmed bus mapping — dropped");
    }
    if (!ok) logOnce_("demand refused (silent or OffsetVariant unset — docs/02 §5a gate)");
  }
  void goSilent() override {
    xSemaphoreTake(gCtMux, portMAX_DELAY);
    gCt->goSilent();
    xSemaphoreGive(gCtMux);
  }
  const char* name() const override { return "ct485"; }

 private:
  void logOnce_(const char* msg) {
    if (millis() - lastLogMs_ < 10000) return;
    lastLogMs_ = millis();
    Serial.printf("[actuator/ct485] %s\n", msg);
  }
  uint32_t lastLogMs_ = 0;
};
#endif

#ifdef DETTSON_ACTUATOR_RELAY
// Case B compressor-side contacts. Gas stays on the CT-485 path (hybrid).
class RelayActuator : public Actuator {
 public:
  RelayActuator() : seq_(gIdleGate) {
    setupPin(cfg::kRelayY1Pin); setupPin(cfg::kRelayY2Pin);
    setupPin(cfg::kRelayObPin); setupPin(cfg::kRelayGPin);
    if (cfg::kSenseGPin >= 0) pinMode(cfg::kSenseGPin, INPUT);
    if (cfg::kSenseDPin >= 0) pinMode(cfg::kSenseDPin, INPUT);
  }
  void apply(const DemandSet& s, uint32_t nowS, uint32_t nowMs) override {
    RelaySequencer::Inputs in;
    in.demand = s;
    // Never close Y without blower confirmation (docs/04 §2 coil-freeze row):
    // without a wired G-sense there is no proof, so Y is never requested.
    const bool blowerProven = cfg::kSenseGPin >= 0 && digitalRead(cfg::kSenseGPin) == HIGH;
    if (!blowerProven) { in.demand.hpHeatPct = 0; in.demand.coolPct = 0; }
    in.defrostSense = cfg::kSenseDPin >= 0 && digitalRead(cfg::kSenseDPin) == HIGH;
    lastDefrostSense_ = in.defrostSense;
    const RelayOutputs& out = seq_.update(in, nowS, nowMs);
    write(cfg::kRelayY1Pin, out.y1); write(cfg::kRelayY2Pin, out.y2);
    write(cfg::kRelayObPin, out.ob); write(cfg::kRelayGPin, out.g);
    if (s.gasHeatPct > 0 || s.defrostTemperPct > 0)
      Serial.printf("[actuator/relay] gas/temper %.0f%%/%.0f%% needs the CT-485 path\n",
                    s.gasHeatPct, s.defrostTemperPct);
  }
  void goSilent() override {
    seq_.goSilent();
    write(cfg::kRelayY1Pin, false); write(cfg::kRelayY2Pin, false);
    write(cfg::kRelayObPin, false); write(cfg::kRelayGPin, false);
  }
  const char* name() const override { return "relay"; }

  // dettson/state/relays diagnostic JSON (docs/06 topic map, Case B).
  void fillRelaysJson(char* buf, size_t n) const {
    const RelayOutputs& o = seq_.outputs();
    snprintf(buf, n, "{\"y1\":%s,\"y2\":%s,\"ob\":%s,\"g\":%s,\"defrost\":%s}",
             o.y1 ? "true" : "false", o.y2 ? "true" : "false",
             o.ob ? "true" : "false", o.g ? "true" : "false",
             lastDefrostSense_ ? "true" : "false");
  }

 private:
  static void setupPin(int p) {
    if (p >= 0) { digitalWrite(p, LOW); pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  }
  static void write(int p, bool on) { if (p >= 0) digitalWrite(p, on ? HIGH : LOW); }
  RelaySequencer seq_;
  bool lastDefrostSense_ = false;
};
#endif

#if defined(DETTSON_ACTUATOR_CT485)
Ct485Actuator gActuatorImpl;
#elif defined(DETTSON_ACTUATOR_RELAY)
RelayActuator* gActuatorPtr = nullptr;  // pin setup must wait for setup()
#else
LoggingActuator gActuatorImpl;
#endif

Actuator* gActuator = nullptr;

// ============================================================================
// NVS persistence
// ============================================================================
struct ResetLoopBlob {
  uint8_t latched = 0;
  uint8_t count = 0;
  uint32_t timesS[safety::ResetLoopAccountant::kMaxBootHistory] = {};
};

float clampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Tunables {
  float balancePointC, compressorMinOatC, auxMaxOatC, minSetpointDeltaC;
};

Tunables loadTunables() {
  Tunables t;
  // Clamped on load to the docs/05 canonical ranges.
  t.balancePointC     = clampF(gPrefs.getFloat("bp",  kBalancePointC),     -30.0f, 15.0f);
  t.compressorMinOatC = clampF(gPrefs.getFloat("cmo", kCompressorMinOatC), -30.0f, 15.0f);
  t.auxMaxOatC        = clampF(gPrefs.getFloat("amo", kAuxMaxOatC),        -30.0f, 30.0f);
  t.minSetpointDeltaC = clampF(gPrefs.getFloat("spd", kMinSetpointDeltaC),
                               kMinSetpointDeltaFloorC, 8.0f);
  return t;
}

void saveGuardBlob() {
  CompressorGuard::PersistBlob blob;
  gGuard.save(&blob);
  gPrefs.putBytes("cg", &blob, sizeof(blob));
}

void saveLockBlob() {
  ui::UiModel::LockPersistBlob blob;
  uiLock(); gUi.saveLock(&blob); uiUnlock();
  gPrefs.putBytes("lock", &blob, sizeof(blob));
}

// ============================================================================
// MQTT task (core 0)
// ============================================================================
WiFiClient gWifiClient;
PubSubClient gMqtt(gWifiClient);

uint8_t findOrAddSensor(const char* name) {  // gCmdMux held; 0 = not found/full
  for (size_t i = 1; i < kFusionSlots; ++i)
    if (gSensorTable[i].used && strncmp(gSensorTable[i].name, name, kSensorNameLen) == 0)
      return static_cast<uint8_t>(i);
  for (size_t i = 1; i < kFusionSlots; ++i)
    if (!gSensorTable[i].used) {
      gSensorTable[i] = SensorEntry{};
      gSensorTable[i].used = true;
      strlcpy(gSensorTable[i].name, name, kSensorNameLen);
      return static_cast<uint8_t>(i);
    }
  return 0;
}

uint8_t findSensor(const char* name) {  // gCmdMux held
  if (strcmp(name, hm::kLocalSensorId) == 0) return 0;
  for (size_t i = 1; i < kFusionSlots; ++i)
    if (gSensorTable[i].used && strncmp(gSensorTable[i].name, name, kSensorNameLen) == 0)
      return static_cast<uint8_t>(i);
  return 0xFF;
}

void handleSensorRoster(const char* json) {
  std::vector<hm::SensorRosterEntry> entries;
  if (!hm::parseSensorRosterJson(json, entries)) return;
  xSemaphoreTake(gCmdMux, portMAX_DELAY);
  for (size_t i = 1; i < kFusionSlots; ++i) gSensorTable[i].inRoster = false;
  for (const auto& e : entries) {
    uint8_t idx = findOrAddSensor(e.id.c_str());
    if (idx == 0) continue;  // table full (fusion mask is 8 wide)
    SensorEntry& s = gSensorTable[idx];
    s.inRoster = true;
    s.hasMaxAge = e.hasMaxAge;
    if (e.hasMaxAge) s.maxAgeS = e.maxAgeS;
    s.offsetC = e.offsetC;
  }
  gPending.sensorRosterDirty = true;
  gPending.anyInbound = true;
  gDiscoveryDirty = true;
  xSemaphoreGive(gCmdMux);
}

void handlePresetRoster(const char* json) {
  std::vector<hm::PresetEntry> entries;
  if (!hm::parsePresetRosterJson(json, entries) || entries.empty()) return;
  xSemaphoreTake(gCmdMux, portMAX_DELAY);
  gPending.presetCount = 0;
  gPresetNameCount = 0;
  for (const auto& e : entries) {
    if (gPending.presetCount >= kMaxPresets) break;
    PresetDef& d = gPending.presetDefs[gPending.presetCount++];
    strlcpy(d.name, e.name.c_str(), sizeof(d.name));
    d.heatC = e.heatC;
    d.coolC = e.coolC;
    strlcpy(gPresetNames[gPresetNameCount++], e.name.c_str(), kPresetNameMaxLen + 1);
  }
  gPending.hasPresetRoster = true;
  gPending.anyInbound = true;
  gDiscoveryDirty = true;
  xSemaphoreGive(gCmdMux);
}

void onMqttMessage(char* topic, uint8_t* payload, unsigned int len) {
  static char buf[1100];
  if (len >= sizeof(buf)) return;
  memcpy(buf, payload, len);
  buf[len] = '\0';

  // Wildcard topics first.
  const char* sensPrefix = "dettson/sensors/";
  const char* offPrefix = "dettson/cmd/sensor/";
  if (strncmp(topic, sensPrefix, strlen(sensPrefix)) == 0) {
    const char* id = topic + strlen(sensPrefix);
    const char* slash = strchr(id, '/');
    if (!slash || strcmp(slash, "/state") != 0) return;
    char name[kSensorNameLen];
    size_t n = static_cast<size_t>(slash - id);
    if (n == 0 || n >= sizeof(name)) return;
    memcpy(name, id, n); name[n] = '\0';
    hm::SensorReading r;
    if (!hm::parseSensorJson(buf, r) || !r.hasTemp) return;
    xSemaphoreTake(gCmdMux, portMAX_DELAY);
    uint8_t idx = findSensor(name);
    if (idx != 0xFF && idx != 0 && gSensorTable[idx].inRoster &&
        gPending.sampleCount < 32) {
      gPending.samples[gPending.sampleCount++] =
          {idx, r.tempC, static_cast<int8_t>(r.hasOcc ? (r.occupied ? 1 : 0) : -1)};
      if (r.hasOcc) gSensorTable[idx].lastOcc = r.occupied;
      gPending.anyInbound = true;
    }
    xSemaphoreGive(gCmdMux);
    return;
  }
  if (strncmp(topic, offPrefix, strlen(offPrefix)) == 0) {
    const char* id = topic + strlen(offPrefix);
    const char* slash = strchr(id, '/');
    if (!slash || strcmp(slash, "/offset") != 0) return;
    char name[kSensorNameLen];
    size_t n = static_cast<size_t>(slash - id);
    if (n == 0 || n >= sizeof(name)) return;
    memcpy(name, id, n); name[n] = '\0';
    auto p = hm::parseSensorOffset(buf);
    if (!p.ok) return;
    xSemaphoreTake(gCmdMux, portMAX_DELAY);
    uint8_t idx = findSensor(name);
    if (idx != 0xFF && gPending.offsetCount < 8) {
      gPending.offsets[gPending.offsetCount++] = {idx, p.value};
      gPending.anyInbound = true;
    }
    xSemaphoreGive(gCmdMux);
    return;
  }

  if (strcmp(topic, hm::topic::kConfigSensors) == 0) { handleSensorRoster(buf); return; }
  if (strcmp(topic, hm::topic::kConfigPresets) == 0) { handlePresetRoster(buf); return; }

  xSemaphoreTake(gCmdMux, portMAX_DELAY);
  bool accepted = true;
  if (strcmp(topic, hm::topic::kCmdSetpoint) == 0) {
    auto p = hm::parseSetpoint(buf);
    if (p.ok) { gPending.hasSetpoint = true; gPending.setpointC = p.value; } else accepted = false;
  } else if (strcmp(topic, hm::topic::kCmdTargetTempLow) == 0) {
    auto p = hm::parseSetpoint(buf);
    if (p.ok) { gPending.hasLow = true; gPending.lowC = p.value; } else accepted = false;
  } else if (strcmp(topic, hm::topic::kCmdTargetTempHigh) == 0) {
    auto p = hm::parseSetpoint(buf);
    if (p.ok) { gPending.hasHigh = true; gPending.highC = p.value; } else accepted = false;
  } else if (strcmp(topic, hm::topic::kCmdMode) == 0) {
    auto p = hm::parseMode(buf);
    if (p.ok) { gPending.hasMode = true; gPending.mode = p.value; } else accepted = false;
  } else if (strcmp(topic, hm::topic::kCmdFanMode) == 0) {
    auto p = hm::parseFanMode(buf);
    if (p.ok) { gPending.hasFan = true; gPending.fan = p.value; } else accepted = false;
  } else if (strcmp(topic, hm::topic::kCmdPreset) == 0) {
    if (len > 0 && len <= kPresetNameMaxLen) {
      gPending.hasPreset = true;
      strlcpy(gPending.preset, buf, sizeof(gPending.preset));
    } else accepted = false;
  } else if (strcmp(topic, hm::topic::kCmdHold) == 0) {
    auto p = hm::parseHoldCommand(buf);
    if (p.ok) { gPending.hasHold = true; gPending.hold = p.value; } else accepted = false;
  } else if (strcmp(topic, hm::topic::kCmdEmHeat) == 0) {
    auto p = hm::parseEmHeatCommand(buf);
    if (p.ok) { gPending.hasEmHeat = true; gPending.emHeat = p.value; } else accepted = false;
  } else if (strcmp(topic, hm::topic::kCmdLockClear) == 0) {
    auto p = hm::parseLockClearCommand(buf);
    if (p.ok) gPending.lockClear = true; else accepted = false;
  } else if (strcmp(topic, hm::topic::kCmdNextTarget) == 0) {
    hm::NextTarget t;
    if (hm::parseNextTargetJson(buf, t)) { gPending.hasTarget = true; gPending.target = t; }
    else accepted = false;
  } else if (strcmp(topic, cfg::kOatTopic) == 0) {
    auto p = hm::parseSetpoint(buf, cfg::kOatIngestMinC, cfg::kOatIngestMaxC);
    if (p.ok) { gPending.hasOat = true; gPending.oatC = p.value; } else accepted = false;
  } else {
    accepted = false;
  }
  if (accepted) gPending.anyInbound = true;
  xSemaphoreGive(gCmdMux);
}

bool pubRetained(const std::string& topic, const std::string& payload) {
  return gMqtt.publish(topic.c_str(), payload.c_str(), true);
}

void publishDiscovery() {
  using namespace hm;
  // Climate entity: preset_modes rebuilt from the configured roster.
  std::vector<std::string> presets;
  xSemaphoreTake(gCmdMux, portMAX_DELAY);
  for (size_t i = 0; i < gPresetNameCount; ++i) presets.push_back(gPresetNames[i]);
  char sensorNames[kFusionSlots][kSensorNameLen];
  bool sensorUsed[kFusionSlots];
  for (size_t i = 1; i < kFusionSlots; ++i) {
    sensorUsed[i] = gSensorTable[i].used && gSensorTable[i].inRoster;
    strlcpy(sensorNames[i], gSensorTable[i].name, kSensorNameLen);
  }
  xSemaphoreGive(gCmdMux);
  if (presets.empty()) presets = {"home", "away", "sleep"};

  pubRetained(discoveryTopic("climate", "hvac"), climateDiscoveryJson(presets));
  pubRetained(discoveryTopic("sensor", "active_equipment"), activeEquipmentDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "modulation"), modulationDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "blower"), blowerDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "fault"), faultDiscoveryJson());
  pubRetained(discoveryTopic("binary_sensor", "health"), healthDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "last_error"), lastErrorDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "compressor_min_off_remaining"),
              compressorMinOffRemainingDiscoveryJson());
  pubRetained(discoveryTopic("binary_sensor", "compressor_locked_out"),
              compressorLockedOutDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "hold"), holdDiscoveryJson());
  pubRetained(discoveryTopic("switch", "em_heat"), emHeatDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "lock"), lockDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "outdoor_temp"), outdoorTempDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "outdoor_source"), outdoorSourceDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "fusion"), fusionDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "changeover_reason"), changeoverReasonDiscoveryJson());
  pubRetained(discoveryTopic("number", std::string("sensor_") + kLocalSensorId + "_offset"),
              sensorOffsetDiscoveryJson(kLocalSensorId));
  for (size_t i = 1; i < kFusionSlots; ++i) {
    if (!sensorUsed[i]) continue;
    std::string id(sensorNames[i]);
    pubRetained(discoveryTopic("sensor", "sensor_" + id + "_age"), sensorAgeDiscoveryJson(id));
    pubRetained(discoveryTopic("binary_sensor", "sensor_" + id + "_participating"),
                sensorParticipatingDiscoveryJson(id));
    pubRetained(discoveryTopic("number", "sensor_" + id + "_offset"),
                sensorOffsetDiscoveryJson(id));
  }
}

void subscribeAll() {
  const char* topics[] = {
      hm::topic::kCmdSetpoint, hm::topic::kCmdTargetTempLow, hm::topic::kCmdTargetTempHigh,
      hm::topic::kCmdMode, hm::topic::kCmdFanMode, hm::topic::kCmdPreset,
      hm::topic::kCmdHold, hm::topic::kCmdEmHeat, hm::topic::kCmdLockClear,
      hm::topic::kCmdNextTarget, hm::topic::kConfigSensors, hm::topic::kConfigPresets,
      cfg::kOatTopic, "dettson/sensors/+/state", "dettson/cmd/sensor/+/offset"};
  for (const char* t : topics) gMqtt.subscribe(t);
}

// State publish cache: one slot per fixed topic (diff suppression).
enum PubIdx : uint8_t {
  PUB_TEMP, PUB_SP, PUB_LOW, PUB_HIGH, PUB_MODE, PUB_FAN, PUB_PRESET, PUB_HOLD,
  PUB_ACTION, PUB_EQUIP, PUB_MOD, PUB_OAT, PUB_OATSRC, PUB_FUSION, PUB_CMOR,
  PUB_CLO, PUB_EMHEAT, PUB_CHG, PUB_LOCK, PUB_BUS, PUB_FAULT, PUB_HEALTH,
  PUB_LASTERR,
#ifdef DETTSON_ACTUATOR_RELAY
  PUB_RELAYS,
#endif
  PUB_COUNT
};
char gPubCache[PUB_COUNT][192];

void pubState(PubIdx i, const char* topic, const char* val, bool force) {
  if (!force && strncmp(gPubCache[i], val, sizeof(gPubCache[i]) - 1) == 0) return;
  strlcpy(gPubCache[i], val, sizeof(gPubCache[i]));
  gMqtt.publish(topic, val);
}

void publishSnapshot(bool force) {
  Snapshot s;
  xSemaphoreTake(gSnapMux, portMAX_DELAY);
  s = gSnap;
  gSnap.lockTombstone = false;  // consumed below
  xSemaphoreGive(gSnapMux);

  char b[192];
  using namespace hm;
  if (s.tempValid) {
    snprintf(b, sizeof(b), "%.2f", static_cast<double>(s.tempC));
    pubState(PUB_TEMP, topic::kStateCurrentTemp, b, force);
  }
  const bool heatish = s.mode == Mode::kHeat || s.emHeat;
  if (heatish || s.mode == Mode::kCool) {
    snprintf(b, sizeof(b), "%.1f", static_cast<double>(heatish ? s.heatSp : s.coolSp));
    pubState(PUB_SP, topic::kStateSetpoint, b, force);
  }
  snprintf(b, sizeof(b), "%.1f", static_cast<double>(s.heatSp));
  pubState(PUB_LOW, topic::kStateTargetTempLow, b, force);
  snprintf(b, sizeof(b), "%.1f", static_cast<double>(s.coolSp));
  pubState(PUB_HIGH, topic::kStateTargetTempHigh, b, force);
  // EMERGENCY_HEAT reports "heat"; the em_heat switch carries the truth (docs/06).
  pubState(PUB_MODE, topic::kStateMode, s.emHeat ? "heat" : toString(s.mode), force);
  pubState(PUB_FAN, topic::kStateFanMode, toString(s.fan), force);
  pubState(PUB_PRESET, topic::kStatePreset, s.preset[0] ? s.preset : "none", force);
  pubState(PUB_HOLD, topic::kStateHold, holdStateJson(s.holdType, s.holdRemainS).c_str(), force);
  pubState(PUB_ACTION, topic::kStateAction, s.action, force);
  pubState(PUB_EQUIP, topic::kStateActiveEquipment, s.equipment, force);
  snprintf(b, sizeof(b), "%.0f", static_cast<double>(s.modulationPct));
  pubState(PUB_MOD, topic::kStateModulation, b, force);
  if (s.oatValid) {
    snprintf(b, sizeof(b), "%.1f", static_cast<double>(s.oatC));
    pubState(PUB_OAT, topic::kStateOutdoorTemp, b, force);
  }
  pubState(PUB_OATSRC, topic::kStateOutdoorSource, oatRungName(s.oatRung), force);
  pubState(PUB_FUSION, topic::kStateFusion, s.fusionJson, force);
  snprintf(b, sizeof(b), "%lu", static_cast<unsigned long>(s.compMinOffRemainS));
  pubState(PUB_CMOR, topic::kStateCompressorMinOffRemaining, b, force);
  pubState(PUB_CLO, topic::kStateCompressorLockedOut,
           s.compLockedOut ? payload::kOn : payload::kOff, force);
  pubState(PUB_EMHEAT, topic::kStateEmHeat, s.emHeat ? payload::kOn : payload::kOff, force);
  pubState(PUB_CHG, topic::kStateChangeoverReason, s.changeReason, force);
  pubState(PUB_LOCK, topic::kStateLock,
           lockStateJson(s.lockState, s.lockLevel, s.pinSet).c_str(), force);
  pubState(PUB_BUS, "dettson/state/bus", s.busJson, force);
  pubState(PUB_FAULT, topic::kStateFault, "none", force);
  pubState(PUB_HEALTH, topic::kStateHealth,
           s.healthProblem ? payload::kOn : payload::kOff, force);
  pubState(PUB_LASTERR, topic::kStateLastError,
           s.lastError[0] ? s.lastError : "none", force);
#ifdef DETTSON_ACTUATOR_RELAY
  pubState(PUB_RELAYS, "dettson/state/relays", s.relaysJson, force);
#endif

  if (force) {  // per-sensor diagnostics on the heartbeat only (age ticks anyway)
    for (const auto& sp : s.sensors) {
      if (!sp.used) continue;
      std::string id(sp.name);
      snprintf(b, sizeof(b), "%lu", static_cast<unsigned long>(sp.ageS));
      gMqtt.publish(sensorAgeStateTopic(id).c_str(), b);
      gMqtt.publish(sensorParticipatingStateTopic(id).c_str(),
                    sp.participating ? payload::kOn : payload::kOff);
      snprintf(b, sizeof(b), "%.1f", static_cast<double>(sp.offsetC));
      gMqtt.publish(sensorOffsetStateTopic(id).c_str(), b);
    }
  }
  if (s.lockTombstone) {
    // Retained empty message deletes any retained lock_clear copy (docs/06).
    gMqtt.publish(hm::topic::kCmdLockClear, "", true);
  }
}

void mqttTask(void*) {
  const bool haveMqtt = strlen(THERMOSTAT_MQTT_HOST) > 0;
#ifdef DETTSON_UI
  // Wi-Fi owned by wifi_prov: NVS-saved creds (or compile-time fallback), set
  // from the wall UI (Settings -> WiFi). Survives reflash.
  const bool haveWifi = wifi_prov::begin(THERMOSTAT_WIFI_SSID, THERMOSTAT_WIFI_PASS);
  if (!haveWifi) Serial.println("[mqtt] no saved Wi-Fi — set one on the wall UI (Settings -> WiFi)");
  mqtt_cfg::begin(THERMOSTAT_MQTT_HOST, THERMOSTAT_MQTT_PORT, THERMOSTAT_MQTT_USER, THERMOSTAT_MQTT_PASS);
  telnet_log::begin();
#else
  const bool haveWifi = strlen(THERMOSTAT_WIFI_SSID) > 0;
  if (haveWifi) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(THERMOSTAT_WIFI_SSID, THERMOSTAT_WIFI_PASS);
  } else {
    Serial.println("[mqtt] no Wi-Fi credentials (src/thermostat_secrets.h) — serial-only bench mode");
  }
#endif
#ifdef DETTSON_UI
  // Broker provisioned on-device (mqtt_cfg): host kept in a task-persistent
  // buffer because PubSubClient::setServer stores the pointer, not a copy.
  static char sMqttHost[64] = {};
  static uint16_t sMqttPort = 1883;
  mqtt_cfg::current(sMqttHost, sizeof(sMqttHost), &sMqttPort, nullptr, 0, nullptr, 0);
  gMqtt.setServer(sMqttHost, sMqttPort);
#else
  gMqtt.setServer(THERMOSTAT_MQTT_HOST, THERMOSTAT_MQTT_PORT);
#endif
  gMqtt.setBufferSize(cfg::kMqttBufBytes);
  gMqtt.setCallback(onMqttMessage);

  uint32_t lastWifiTryMs = 0, lastMqttTryMs = 0, lastHeartbeatMs = 0, lastDiscoverMs = 0;
  for (;;) {
    const uint32_t nowMs = millis();
#ifdef DETTSON_UI
    wifi_prov::service(nowMs);
    telnet_log::poll();
    gWifiConnected = wifi_prov::connected();
    (void)haveWifi; (void)haveMqtt; (void)lastWifiTryMs;
    // Silent auto-discovery: Wi-Fi up but no broker saved -> browse mDNS for the
    // MQTT broker (fallback: the Home Assistant host on the default port).
    if (gWifiConnected && !mqtt_cfg::hostSet() && nowMs - lastDiscoverMs >= 15000) {
      lastDiscoverMs = nowMs;
      static bool sMdnsUp = false;
      if (!sMdnsUp) sMdnsUp = MDNS.begin("slytherm");
      IPAddress ip; uint16_t pt = 0;
      int n = MDNS.queryService("mqtt", "tcp");                       // 1: advertised broker
      if (n > 0) { ip = MDNS.IP(0); pt = MDNS.port(0); }
      else { n = MDNS.queryService("home-assistant", "tcp"); if (n > 0) { ip = MDNS.IP(0); pt = 1883; } }  // 2: HA host
      if (n <= 0) {                                                    // 3: well-known HA hostname
        IPAddress hip = MDNS.queryHost("homeassistant");
        if (hip != IPAddress(0, 0, 0, 0)) { ip = hip; pt = 1883; n = 1; }
      }
      if (n > 0 && ip != IPAddress(0, 0, 0, 0)) {
        char host[24];
        snprintf(host, sizeof(host), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        telnet_log::logf("[mqtt] mDNS discovered broker %s:%u", host, pt ? pt : 1883);
        mqtt_cfg::save(host, pt ? pt : 1883, "", "");  // anonymous; add auth in Installer
      }
    }
    if (mqtt_cfg::takeDirty()) {  // broker (re)provisioned: discovery or UI save
      mqtt_cfg::current(sMqttHost, sizeof(sMqttHost), &sMqttPort, nullptr, 0, nullptr, 0);
      gMqtt.setServer(sMqttHost, sMqttPort);
      gMqtt.disconnect();
    }
    const bool haveMqttNow = mqtt_cfg::hostSet();
#else
    gWifiConnected = haveWifi && WiFi.status() == WL_CONNECTED;
    if (haveWifi && !gWifiConnected && nowMs - lastWifiTryMs >= cfg::kWifiRetryMs) {
      lastWifiTryMs = nowMs;
      WiFi.disconnect();
      WiFi.begin(THERMOSTAT_WIFI_SSID, THERMOSTAT_WIFI_PASS);
    }
    const bool haveMqttNow = haveMqtt;
#endif
    if (gWifiConnected && haveMqttNow && !gMqtt.connected() &&
        nowMs - lastMqttTryMs >= cfg::kMqttReconnectMs) {
      lastMqttTryMs = nowMs;
      // LWT: availability topic -> retained "offline" (docs/06).
#ifdef DETTSON_UI
      char mqUser[48], mqPass[64];
      mqtt_cfg::current(nullptr, 0, nullptr, mqUser, sizeof(mqUser), mqPass, sizeof(mqPass));
      const char* muser = strlen(mqUser) ? mqUser : nullptr;
      const char* mpass = strlen(mqPass) ? mqPass : nullptr;
#else
      const char* muser = strlen(THERMOSTAT_MQTT_USER) ? THERMOSTAT_MQTT_USER : nullptr;
      const char* mpass = strlen(THERMOSTAT_MQTT_PASS) ? THERMOSTAT_MQTT_PASS : nullptr;
#endif
      if (gMqtt.connect(cfg::kMqttClientId, muser, mpass,
                        hm::topic::kAvailability, 0, true, hm::payload::kOffline)) {
        gMqtt.publish(hm::topic::kAvailability, hm::payload::kOnline, true);
        publishDiscovery();
        gDiscoveryDirty = false;
        subscribeAll();
        memset(gPubCache, 0, sizeof(gPubCache));  // full republish after reconnect
#ifdef DETTSON_UI
        telnet_log::logf("[mqtt] connected to %s:%u", sMqttHost, sMqttPort);
#else
        Serial.println("[mqtt] connected");
#endif
      }
#ifdef DETTSON_UI
      else telnet_log::logf("[mqtt] connect failed (state=%d)", gMqtt.state());
#endif
    }
    gMqttConnected = gMqtt.connected();
#ifdef DETTSON_UI
    mqtt_cfg::setConnected(gMqttConnected);
#endif
    if (gMqttConnected) {
      gMqtt.loop();
      if (gDiscoveryDirty) { gDiscoveryDirty = false; publishDiscovery(); }
      static uint32_t lastSnapMs = 0;
      const bool heartbeat = nowMs - lastHeartbeatMs >= cfg::kStateHeartbeatS * 1000u;
      if (heartbeat) lastHeartbeatMs = nowMs;
      if (heartbeat || nowMs - lastSnapMs >= 500) {  // diff-publish at <= 2 Hz
        lastSnapMs = nowMs;
        publishSnapshot(heartbeat);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(cfg::kMqttLoopMs));
  }
}

// ============================================================================
// CT-485 task (core 1, above control priority). Protocol state machine is
// always running; the UART glue is compile-flag stubbed (see file header).
// ============================================================================
volatile uint32_t gLastBusRxS = 0;   // 0 = never
volatile uint32_t gCtTxSuppressed = 0;

#ifdef DETTSON_CT485_UART
ct485::FrameAccumulator gCtAcc;
uint32_t gCtLastByteUs = 0;
bool gCtInProgress = false;
#endif

void ct485Task(void*) {
#ifdef DETTSON_CT485_UART
  pinMode(cfg::kCt485DePin, OUTPUT);
  digitalWrite(cfg::kCt485DePin, LOW);  // receive; hardware pulldown agrees
  Serial2.setRxBufferSize(2048);
  Serial2.begin(ct485::kBaudDefault, SERIAL_8N1, cfg::kCt485RxPin, cfg::kCt485TxPin);
  gCtLastByteUs = micros();
#endif
  for (;;) {
    const uint32_t nowMs = millis();
    xSemaphoreTake(gCtMux, portMAX_DELAY);
#ifdef DETTSON_CT485_UART
    int avail = Serial2.available();
    while (avail-- > 0) {
      const int c = Serial2.read();
      if (c < 0) break;
      const uint32_t nowUs = micros();
      const bool gapBefore =
          !gCtInProgress || (nowUs - gCtLastByteUs) >= ct485::kInterFrameGapUs;
      gCtLastByteUs = nowUs;
      gCtInProgress = true;
      if (gCtAcc.feed(static_cast<uint8_t>(c), gapBefore)) {
        ct485::Frame f;
        if (ct485::decode(gCtAcc.frame(), gCtAcc.frameLen(), f)) {
          gCt->onFrame(f, nowMs);
          gLastBusRxS = nowSeconds();
        }
      }
    }
    if (gCtInProgress &&
        static_cast<uint32_t>(micros() - gCtLastByteUs) >= ct485::kInterFrameGapUs) {
      gCtInProgress = false;
      if (gCtAcc.flush()) {
        ct485::Frame f;
        if (ct485::decode(gCtAcc.frame(), gCtAcc.frameLen(), f)) {
          gCt->onFrame(f, nowMs);
          gLastBusRxS = nowSeconds();
        }
      }
    }
#endif
    gCt->tick(nowMs);
    ct485::Frame txf;
    while (gCt->popTx(txf)) {
#if defined(DETTSON_CT485_UART) && defined(DETTSON_CT485_TX_ENABLE)
      uint8_t raw[ct485::kMaxFrame];
      const size_t n = ct485::encode(txf, raw);
      if (n > 0) {
        digitalWrite(cfg::kCt485DePin, HIGH);
        delayMicroseconds(ct485::kDePrePostUs);
        Serial2.write(raw, n);
        Serial2.flush();
        delayMicroseconds(ct485::kDePrePostUs);
        digitalWrite(cfg::kCt485DePin, LOW);
      }
#else
      // Stub: authorized frames are counted and dropped — nothing reaches a
      // wire. (With the boot-silent default nothing is ever authorized.)
      gCtTxSuppressed = gCtTxSuppressed + 1;
#endif
    }
    xSemaphoreGive(gCtMux);
    vTaskDelay(pdMS_TO_TICKS(cfg::kCt485TickMs));
  }
}

// ============================================================================
// Control task (core 1) — the docs/05 Phase 4 pipeline at a fixed cadence.
// ============================================================================
hm::FanMode gFanMode = hm::FanMode::kAuto;
bool gSetpointsValidated = false;
bool gConfigOk = true;
uint32_t gLastInboundS = 0;       // last accepted MQTT traffic (stale clock)
bool gFallbackApplied = false;
bool gWdtPetLevel = false;
float gLastHpEmittedPct = 0.0f;
bool gHpRouteHeat = false;        // which family the shared compressor serves

#ifdef DETTSON_DS18B20
void pollLocalSensors(uint32_t nowS);
#endif

// Recovery (advisory) state.
bool gHaveTarget = false;
hm::NextTarget gTarget;
uint32_t gTargetRxS = 0;
bool gTargetApplied = false;

// Run-segment tracking for RecoveryEstimator learning.
enum class Serving : uint8_t { kNone, kGas, kHpHeat, kCool };
Serving gServing = Serving::kNone;

// Persist-on-change shadows.
struct PersistShadow {
  uint8_t mode = 0xFF, priorMode = 0xFF, hold = 0xFF, fan = 0xFF;
  float heatSp = NAN, coolSp = NAN;
  bool guardRunning = false;
} gShadow;
uint32_t gLastClockSaveS = 0, gLastGuardSaveS = 0;

UserMode toUserMode(hm::Mode m) {
  switch (m) {
    case hm::Mode::kHeat: return UserMode::kHeat;
    case hm::Mode::kCool: return UserMode::kCool;
    case hm::Mode::kHeatCool: return UserMode::kAuto;
    default: return UserMode::kOff;
  }
}
hm::Mode toWireMode(UserMode m) {
  switch (m) {
    case UserMode::kHeat: case UserMode::kEmergencyHeat: return hm::Mode::kHeat;
    case UserMode::kCool: return hm::Mode::kCool;
    case UserMode::kAuto: return hm::Mode::kHeatCool;
    default: return hm::Mode::kOff;
  }
}

const char* joinStateName(ct485::JoinState j) {
  switch (j) {
    case ct485::JoinState::kUnaddressed: return "unaddressed";
    case ct485::JoinState::kSlotWait: return "slot_wait";
    case ct485::JoinState::kDiscoveryResponded: return "discovery_responded";
    case ct485::JoinState::kAddressed: return "addressed";
  }
  return "?";
}

const char* tierName(SourceTier t) {
  switch (t) {
    case SourceTier::kFusedRemotes: return "fused_remotes";
    case SourceTier::kSingleRemote: return "single_remote";
    case SourceTier::kLocalDegraded: return "local_degraded";
    default: return "none";
  }
}

void glueAlarm(bool present, uint16_t code, safety::Severity sev, const char* text,
               uint32_t nowS) {
  if (present) gSup->alarms().raise(code, sev, text, nowS);
  else gSup->alarms().clearCondition(code);
}

// Staged-HP request map (see thermostat_config.h: glue knob, not canonical).
float stagedRequestPct(float errC) {
  if (errC <= 0.0f) return cfg::kHpDutyMinPct;
  const float frac = errC >= cfg::kHpFullScaleErrC ? 1.0f : errC / cfg::kHpFullScaleErrC;
  return cfg::kHpDutyMinPct + (100.0f - cfg::kHpDutyMinPct) * frac;
}

void consumeCommands(uint32_t nowS) {
  Pending p;
  xSemaphoreTake(gCmdMux, portMAX_DELAY);
  p = gPending;
  gPending = Pending{};
  // Sensor reconcile data is read inline below while still holding the mutex.
  SensorEntry table[kFusionSlots];
  memcpy(table, gSensorTable, sizeof(table));
  xSemaphoreGive(gCmdMux);

  if (p.anyInbound) gLastInboundS = nowS;

  if (p.sensorRosterDirty) {
    for (size_t i = 1; i < kFusionSlots; ++i) {
      if (!table[i].used) continue;
      gFusion.registerSensor(static_cast<uint8_t>(i));  // duplicate -> false, harmless
      gFusion.setParticipating(static_cast<uint8_t>(i), table[i].inRoster);
      if (table[i].hasMaxAge) gFusion.setSensorMaxAgeS(static_cast<uint8_t>(i), table[i].maxAgeS);
      gFusion.setSensorOffsetC(static_cast<uint8_t>(i), table[i].offsetC);
    }
  }
  for (size_t i = 0; i < p.sampleCount; ++i) {
    const auto& s = p.samples[i];
    gFusion.update(s.idx, s.tempC,
                   s.occ < 0 ? Occupancy::kUnknown
                             : (s.occ ? Occupancy::kOccupied : Occupancy::kVacant),
                   nowS);
  }
  for (size_t i = 0; i < p.offsetCount; ++i) {
    gFusion.setSensorOffsetC(p.offsets[i].idx, p.offsets[i].offsetC);
    xSemaphoreTake(gCmdMux, portMAX_DELAY);
    gSensorTable[p.offsets[i].idx].offsetC = p.offsets[i].offsetC;
    xSemaphoreGive(gCmdMux);
  }
  if (p.hasOat) gOat.submit(OatRung::kHaWeather, p.oatC, nowS);

  if (p.hasPresetRoster) gModeSm->setPresetRoster(p.presetDefs, p.presetCount);
  if (p.hasMode) gModeSm->setMode(toUserMode(p.mode), nowS);
  if (p.hasEmHeat) gModeSm->setEmergencyHeat(p.emHeat, nowS);
  if (p.hasSetpoint) {
    // Single setpoint serves the active simple mode (docs/06 topic map).
    if (gModeSm->mode() == UserMode::kHeat || gModeSm->mode() == UserMode::kEmergencyHeat)
      gModeSm->setHeatSetpoint(p.setpointC, nowS);
    else if (gModeSm->mode() == UserMode::kCool)
      gModeSm->setCoolSetpoint(p.setpointC, nowS);
  }
  if (p.hasLow) gModeSm->setHeatSetpoint(p.lowC, nowS);
  if (p.hasHigh) gModeSm->setCoolSetpoint(p.highC, nowS);
  if (p.hasPreset) gModeSm->applyPreset(p.preset, nowS);
  if (p.hasHold) {
    if (p.hold.clear) gModeSm->clearHold();
    else gModeSm->startHold(p.hold.type, nowS);
  }
  if (p.hasFan) gFanMode = p.fan;
  if (p.lockClear) {
    uiLock(); gUi.clearUserPin(); uiUnlock();  // saveLockBlob() self-locks below
    saveLockBlob();
    xSemaphoreTake(gSnapMux, portMAX_DELAY);
    gSnap.lockTombstone = true;
    xSemaphoreGive(gSnapMux);
  }
  if (p.hasTarget) {
    gHaveTarget = true;
    gTarget = p.target;
    gTargetRxS = nowS;
    gTargetApplied = false;
  }

  // Wall-UI intents (no LVGL binding yet; queue stays wired so the binding
  // is drop-in). Control task re-validates through ModeStateMachine.
  uiLock();
  ui::UiIntent intent;
  while (gUi.popIntent(intent)) {
    switch (intent.type) {
      case ui::IntentType::kSetSetpoints:
        gModeSm->setHeatSetpoint(intent.heatC, nowS);
        gModeSm->setCoolSetpoint(intent.coolC, nowS);
        break;
      case ui::IntentType::kSetMode:
        gModeSm->setMode(static_cast<UserMode>(intent.mode), nowS);
        break;
      case ui::IntentType::kSetPreset: {
        const char* names[] = {"home", "away", "sleep"};
        gModeSm->applyPreset(names[static_cast<uint8_t>(intent.preset) % 3], nowS);
        break;
      }
      case ui::IntentType::kAckAlarms:
        gSup->alarms().ackAll();
        break;
    }
  }
  uiUnlock();
}

void persistOnChange(uint32_t nowS) {
  const uint8_t mode = static_cast<uint8_t>(gModeSm->mode());
  if (mode != gShadow.mode) {
    // Entering EMERGENCY_HEAT: persist the mode it should restore to (G15).
    if (mode == static_cast<uint8_t>(UserMode::kEmergencyHeat) && gShadow.mode != 0xFF)
      gPrefs.putUChar("pmode", gShadow.mode);
    gPrefs.putUChar("mode", mode);
    gShadow.mode = mode;
  }
  const float h = gModeSm->heatSetpoint(), c = gModeSm->coolSetpoint();
  if (h != gShadow.heatSp) { gPrefs.putFloat("hsp", h); gShadow.heatSp = h; }
  if (c != gShadow.coolSp) { gPrefs.putFloat("csp", c); gShadow.coolSp = c; }
  const uint8_t hold = static_cast<uint8_t>(gModeSm->activeHoldType());
  if (hold != gShadow.hold) { gPrefs.putUChar("hold", hold); gShadow.hold = hold; }
  const uint8_t fan = static_cast<uint8_t>(gFanMode);
  if (fan != gShadow.fan) { gPrefs.putUChar("fan", fan); gShadow.fan = fan; }

  if (gGuard.running() != gShadow.guardRunning ||
      nowS - gLastGuardSaveS >= cfg::kGuardSaveS) {
    gShadow.guardRunning = gGuard.running();
    gLastGuardSaveS = nowS;
    saveGuardBlob();
  }
  if (nowS - gLastClockSaveS >= cfg::kClockSaveS) {
    gLastClockSaveS = nowS;
    gPrefs.putUInt("clk", nowS);
  }
}

void updateRunSegments(const DemandSet& out, const FusedTemp& fused, uint32_t nowS) {
  Serving now = Serving::kNone;
  if (out.gasHeatPct > 0) now = Serving::kGas;
  else if (out.hpHeatPct > 0) now = Serving::kHpHeat;
  else if (out.coolPct > 0) now = Serving::kCool;
  if (now == gServing) return;
  if (gServing != Serving::kNone && fused.valid) gRecovery.endSegment(fused.value, nowS);
  if (now != Serving::kNone && fused.valid) {
    gRecovery.startSegment(now == Serving::kCool ? RecoveryMode::kCool : RecoveryMode::kHeat,
                           now == Serving::kGas ? RecoveryEquipment::kGas
                                                : RecoveryEquipment::kHp,
                           fused.value, nowS);
  }
  gServing = now;
}

void adviseRecovery(const FusedTemp& fused, const OatReading& oat, uint32_t nowS) {
  if (!gHaveTarget || !fused.valid) return;
  const uint32_t elapsed = nowS - gTargetRxS;
  if (elapsed >= gTarget.inS) { gHaveTarget = false; return; }
  RecoveryTarget t;
  t.setpointC = gTarget.tempC;
  t.mode = gTarget.mode == hm::Mode::kCool ? RecoveryMode::kCool : RecoveryMode::kHeat;
  t.inS = gTarget.inS - elapsed;
  const RecoveryEquipment equip =
      (t.mode == RecoveryMode::kHeat && oat.valid &&
       oat.valueC <= gDualFuel->config().balancePointC)
          ? RecoveryEquipment::kGas
          : RecoveryEquipment::kHp;
  const RecoveryAdvice a = gRecovery.advise(t, fused.value, equip);
  if (a.startNow && !gTargetApplied) {
    // ADVISORY acted on here, in glue: early-apply the scheduled setpoint via
    // the time-less setters (no hold). Every demand still passes the full
    // pipeline gates (docs/06 "Smart recovery").
    if (t.mode == RecoveryMode::kHeat) gModeSm->setHeatSetpoint(t.setpointC);
    else gModeSm->setCoolSetpoint(t.setpointC);
    gTargetApplied = true;
    Serial.printf("[recovery] early start: %.1fC in %lus\n",
                  static_cast<double>(t.setpointC), static_cast<unsigned long>(t.inS));
  }
}

void fillSnapshot(const FusedTemp& fused, const OatReading& oat, const DemandSet& out,
                  bool temperActive, const char* changeReason, uint32_t nowS) {
  Snapshot s;
  xSemaphoreTake(gSnapMux, portMAX_DELAY);
  s.lockTombstone = gSnap.lockTombstone;  // keep an unconsumed tombstone flag
  xSemaphoreGive(gSnapMux);

  s.tempValid = fused.valid; s.tempC = fused.value;
  s.heatSp = gModeSm->heatSetpoint(); s.coolSp = gModeSm->coolSetpoint();
  s.mode = toWireMode(gModeSm->mode());
  s.emHeat = gModeSm->emergencyHeat();
  s.fan = gFanMode;
  strlcpy(s.preset, gModeSm->activePreset(), sizeof(s.preset));
  s.holdType = gModeSm->activeHoldType();
  s.holdRemainS = gModeSm->holdRemainingS(nowS);

  const bool heating = out.gasHeatPct > 0 || out.hpHeatPct > 0;
  if (temperActive) strlcpy(s.action, "defrosting", sizeof(s.action));
  else if (heating) strlcpy(s.action, "heating", sizeof(s.action));
  else if (out.coolPct > 0) strlcpy(s.action, "cooling", sizeof(s.action));
  else if (out.fanPct > 0) strlcpy(s.action, "fan", sizeof(s.action));
  else strlcpy(s.action, gModeSm->mode() == UserMode::kOff ? "off" : "idle", sizeof(s.action));

  if (temperActive) strlcpy(s.equipment, "defrost", sizeof(s.equipment));
  else if (out.gasHeatPct > 0) strlcpy(s.equipment, "gas_heat", sizeof(s.equipment));
  else if (out.hpHeatPct > 0) strlcpy(s.equipment, "hp_heat", sizeof(s.equipment));
  else if (out.coolPct > 0) strlcpy(s.equipment, "cool", sizeof(s.equipment));
  else strlcpy(s.equipment, "idle", sizeof(s.equipment));

  s.modulationPct = out.gasHeatPct;
  s.oatValid = oat.valid; s.oatC = oat.valueC; s.oatRung = oat.rung;

  // Fusion JSON (docs/06 topic map).
  char parts[96] = "";
  bool occupied = false;
  size_t pos = 0;
  xSemaphoreTake(gCmdMux, portMAX_DELAY);
  for (size_t i = 1; i < kFusionSlots; ++i) {
    if (!gSensorTable[i].used) continue;
    const SensorStatus st = gFusion.status(static_cast<uint8_t>(i), nowS);
    if (!st.live) continue;
    if (gSensorTable[i].lastOcc) occupied = true;
    pos += static_cast<size_t>(snprintf(parts + pos, sizeof(parts) - pos, "%s\"%s\"",
                                        pos ? "," : "", gSensorTable[i].name));
    if (pos >= sizeof(parts) - 1) break;
  }
  size_t pubIdx = 0;
  for (size_t i = 0; i < kFusionSlots && pubIdx < kFusionSlots; ++i) {
    if (!gSensorTable[i].used && i != 0) continue;
    Snapshot::SensorPub& sp = s.sensors[pubIdx++];
    sp.used = true;
    strlcpy(sp.name, i == 0 ? hm::kLocalSensorId : gSensorTable[i].name, sizeof(sp.name));
    const SensorStatus st = gFusion.status(static_cast<uint8_t>(i), nowS);
    sp.ageS = st.ageS == UINT32_MAX ? 0 : st.ageS;
    sp.participating = st.live;
    sp.offsetC = st.offsetC;
  }
  xSemaphoreGive(gCmdMux);
  snprintf(s.fusionJson, sizeof(s.fusionJson),
           "{\"temp\":%.2f,\"tier\":\"%s\",\"participants\":[%s],\"occupied\":%s}",
           static_cast<double>(fused.value), tierName(fused.tier), parts,
           occupied ? "true" : "false");

  // Compressor diagnostics (glue estimate; the guard owns the real timers).
  if (gGuard.running()) s.compMinOffRemainS = 0;
  else {
    uint32_t end = gCompStopAnchorS + kCompressorMinOffS;
    if (gBootHoldEndS > end) end = gBootHoldEndS;
    s.compMinOffRemainS = nowS < end ? end - nowS : 0;
  }
  s.compLockedOut = gGuard.lockedOut() || !oat.valid ||
                    oat.valueC < gDualFuel->config().compressorMinOatC;

  strlcpy(s.changeReason, changeReason, sizeof(s.changeReason));
  s.healthProblem = gSup->healthProblem();
  strlcpy(s.lastError, gSup->alarms().lastErrorText(), sizeof(s.lastError));
  uiLock();
  s.lockState = static_cast<hm::LockState>(gUi.lockState());
  s.lockLevel = static_cast<hm::LockLevel>(gUi.lockLevel());
  s.pinSet = gUi.userPinSet();
  uiUnlock();

  xSemaphoreTake(gCtMux, portMAX_DELAY);
  snprintf(s.busJson, sizeof(s.busJson),
           "{\"join\":\"%s\",\"addr\":%u,\"silent\":%s,\"last_ack\":\"0x%02X\","
           "\"alarms\":{\"pairing\":%s,\"comms_loss\":%s,\"starvation\":%s}}",
           joinStateName(gCt->joinState()), gCt->nodeAddress(),
           gCt->silent() ? "true" : "false", gCt->lastResponseCode(),
           gCt->pairingAlarm() ? "true" : "false",
           gCt->commsLossAlarm() ? "true" : "false",
           gCt->starvationAlarm() ? "true" : "false");
  xSemaphoreGive(gCtMux);

#ifdef DETTSON_ACTUATOR_RELAY
  if (gActuatorPtr) gActuatorPtr->fillRelaysJson(s.relaysJson, sizeof(s.relaysJson));
#endif

  xSemaphoreTake(gSnapMux, portMAX_DELAY);
  gSnap = s;
  xSemaphoreGive(gSnapMux);
}

void controlCycle(uint32_t nowS, uint32_t nowMs) {
  consumeCommands(nowS);

  // ---- Inputs ----
#ifdef DETTSON_DS18B20
  pollLocalSensors(nowS);
#endif
  const FusedTemp fused = gFusion.fusedTemp(nowS);
  const OatReading oat = gOat.read(nowS);

  // ---- Stale-MQTT fallback profile (docs/04 §2 MQTT row; docs/06) ----
  const bool setpointFresh = nowS - gLastInboundS < kMqttStaleS;
  if (!setpointFresh && !gFallbackApplied) {
    gFallbackApplied = true;
    // Dual-bounded, mode = last user mode, never escalate OFF.
    gModeSm->setHeatSetpoint(kFallbackHeatSetpointC);
    gModeSm->setCoolSetpoint(kFallbackCoolSetpointC);
  }
  if (setpointFresh) gFallbackApplied = false;
  glueAlarm(gFallbackApplied, cfg::kAlarmMqttFallback, safety::Severity::kAdvisory,
            "MQTT stale: fallback setpoints", nowS);

  // ---- Boot gate (docs/04 §3 boot validation) ----
  safety::BootFacts bf;
  bf.sensorOk = fused.valid;
  bf.setpointPresent = gSetpointsValidated;
  bf.configCrcOk = gConfigOk;
  gSup->updateBootGate(bf, nowS);

  // ---- Mode state machine -> effective call ----
  const bool swapOk = compressorProvenIdle(nowS);
  Call call = gModeSm->update(fused.value, fused.valid, nowS, swapOk);

  // DS18B20-only degraded mode (docs/04 §4): cooling disabled, heat-to ceiling.
  if (fused.degraded) {
    if (call.type == CallType::kCool) call = Call{};
    if (call.type == CallType::kHeat && fused.value >= kDegradedHeatCeilC) call = Call{};
  }
  glueAlarm(fused.degraded, cfg::kAlarmDegradedMode, safety::Severity::kCritical,
            "DS18B20-only degraded mode", nowS);

  // ---- Dual fuel ----
  DualFuelInputs dfi;
  dfi.heatCall = call.type == CallType::kHeat;
  dfi.setpointC = gModeSm->heatSetpoint();
  dfi.roomTempC = fused.value;
  dfi.roomTempValid = fused.valid;
  dfi.oatC = oat.valueC;
  dfi.oatValid = oat.valid;
  dfi.hpDemandPct = gLastHpEmittedPct;
#ifdef DETTSON_ACTUATOR_RELAY
  dfi.defrostActive = cfg::kSenseDPin >= 0 && digitalRead(cfg::kSenseDPin) == HIGH;
#endif
  const DualFuelOutput dfo = gDualFuel->step(dfi, nowS);
  glueAlarm(dfo.oatInvalidAlarm, cfg::kAlarmOatFailCold, safety::Severity::kAdvisory,
            "OAT invalid: fail-cold (gas only)", nowS);

  // ---- Per-equipment requests ----
  float gasReq = 0, hpReq = 0, coolReq = 0, fanReq = 0;
  const bool temperActive = dfo.temperRequest;
  static const char* changeReason = "none";
  if (call.type == CallType::kHeat) {
    // EMERGENCY_HEAT is gas-only by definition; the user's explicit choice
    // overrides the high-OAT gas lockout (decision flagged in the report).
    const HeatSource src = call.gasOnly ? HeatSource::kGas : dfo.source;
    if (src == HeatSource::kGas) {
      gPid.selectMode(call.gasOnly ? 1 : 0);
      gasReq = gPid.update(gModeSm->heatSetpoint(), fused.value, fused.valid,
                           temperActive, nowS);
      if (fused.degraded) gasReq = fminf(gasReq, cfg::kDegradedGasCapPct);
      changeReason = call.gasOnly ? "manual"
                     : dfo.escalated ? "escalation"
                     : dfo.oatInvalidAlarm ? "fail_cold" : "balance_point";
    } else if (src == HeatSource::kHeatPump) {
      gPid.reset();  // no stale integrator when gas re-stages
      hpReq = stagedRequestPct(call.errorC);
      changeReason = "balance_point";
    }
  } else {
    gPid.reset();
    if (call.type == CallType::kCool) {
      // Cooling lockouts (docs/05 table): indoor floor + unknown OAT.
      const bool lockedOut = !oat.valid || fused.degraded ||
                             fused.value < kCoolingIndoorLockoutC;
      if (!lockedOut) coolReq = stagedRequestPct(call.errorC);
      changeReason = lockedOut ? "lockout" : changeReason;
    }
  }
  if (gModeSm->mode() != UserMode::kOff) {
    if (gFanMode == hm::FanMode::kOn) fanReq = 100.0f;
    else if (gFanMode == hm::FanMode::kCirculate)
      fanReq = ((nowS % 3600) / 60) < cfg::kFanCirculateMinPerHour ? 100.0f : 0.0f;
  }

  // ---- Safety facts + supervisor (before any emission) ----
  safety::HealthFacts hf;
  hf.sensorValid = fused.valid;
  hf.setpointFresh = setpointFresh;
  hf.mqttAlive = gMqttConnected;
#ifdef DETTSON_CT485_UART
  hf.busAlive = gLastBusRxS != 0 && nowS - gLastBusRxS < kBusDeadmanS;
#else
  hf.busAlive = true;  // no bus fitted on the bench — deadman idle by design
#endif
  hf.controlLoopTicking = true;  // we are the loop; a stall stops update() itself
  {  // sanity cross-check of the previous emission (arbiter enforces; verify)
    const DemandSet& prev = gArbiter->current();
    const bool heatFam = prev.gasHeatPct > 0 || prev.hpHeatPct > 0 || prev.defrostTemperPct > 0;
    hf.demandStateSane = !(heatFam && prev.coolPct > 0) &&
                         !(prev.gasHeatPct > 0 && prev.hpHeatPct > 0);
  }
  gSup->update(hf, nowS);

  // ---- Shapers -> single emission point -> actuator ----
  DemandRequest req;
  if (!gSup->demandPermitted()) {
    // Safety drop: immediate, never the comfort path (docs/04 §1 prime
    // directive). Recovery below re-enters through every timer.
    if (gGasShaper.lit()) gGasShaper.forceStop(nowS);
    compressorSafetyStop(nowS);
    gActuator->goSilent();
  } else {
    req.gasHeatPct = gGasShaper.shape(gasReq, nowS);
    // One compressor, one shaper: route its (possibly min-on-held) output to
    // the family that last requested it, so a held output can never flip
    // heat<->cool mid-run.
    if (hpReq > 0) gHpRouteHeat = true;
    else if (coolReq > 0) gHpRouteHeat = false;
    const float hpOut = gHpShaper.shape(fmaxf(hpReq, coolReq), nowS);
    if (gHpRouteHeat) req.hpHeatPct = hpOut; else req.coolPct = hpOut;
    req.fanPct = fanReq;
    req.defrostTemperPct = temperActive ? dfo.temperHeatPct : 0.0f;
#if defined(DETTSON_CT485_UART) && defined(DETTSON_CT485_TX_ENABLE)
    if (gSup->bootGateOpen() && gCt->silent()) {
      xSemaphoreTake(gCtMux, portMAX_DELAY);
      gCt->resume(nowMs);
      xSemaphoreGive(gCtMux);
    }
#endif
  }
  glueAlarm(gGasShaper.runtimeAlarm(), cfg::kAlarmGasRuntime, safety::Severity::kCritical,
            "gas max-runtime trip", nowS);

  const DemandSet out = gArbiter->set(gSup->filterRequest(req), nowS);
  glueAlarm(gArbiter->invariantAlarm(), cfg::kAlarmDemandConflict,
            safety::Severity::kCritical, "demand-conflict invariant latched", nowS);
  gLastHpEmittedPct = out.hpHeatPct > 0 ? out.hpHeatPct : out.coolPct;
  gActuator->apply(out, nowS, nowMs);

  // CT-485 TX-stack alarms surface through the registry (docs/06 bus entity).
  xSemaphoreTake(gCtMux, portMAX_DELAY);
  const bool busAlarm = gCt->pairingAlarm() || gCt->commsLossAlarm() || gCt->starvationAlarm();
  xSemaphoreGive(gCtMux);
  glueAlarm(busAlarm, cfg::kAlarmBusTxStack, safety::Severity::kCritical,
            "CT-485 TX stack alarm (see dettson/state/bus)", nowS);

  // ---- External hardware watchdog pet (docs/04 §3 pet-gating) ----
  if (gSup->petExternalWdt() && cfg::kWdtPetPin >= 0) {
    gWdtPetLevel = !gWdtPetLevel;
    digitalWrite(cfg::kWdtPetPin, gWdtPetLevel ? HIGH : LOW);
  }

  // ---- Advisory recovery + learning ----
  updateRunSegments(out, fused, nowS);
  adviseRecovery(fused, oat, nowS);

  // ---- UI model sync (rendered by the wall-UI task, DETTSON_UI) ----
  uiLock();
  gUi.tick(nowS);
  gUi.setFusedTemp(fused.value, fused.valid);
  gUi.setSetpoints(gModeSm->heatSetpoint(), gModeSm->coolSetpoint());
  gUi.setUserMode(static_cast<ui::UserMode>(gModeSm->mode()));
  uint8_t mask = ui::kEquipNone;
  if (out.gasHeatPct > 0 || out.defrostTemperPct > 0) mask |= ui::kEquipGas;
  if (out.hpHeatPct > 0) mask |= ui::kEquipHpHeat;
  if (out.coolPct > 0) mask |= ui::kEquipHpCool;
  if (out.fanPct > 0) mask |= ui::kEquipFan;
  gUi.setActiveEquipment(mask);
  gUi.setHvacAction(out.defrostTemperPct > 0 ? ui::HvacAction::kDefrosting
                    : (out.gasHeatPct > 0 || out.hpHeatPct > 0) ? ui::HvacAction::kHeating
                    : out.coolPct > 0 ? ui::HvacAction::kCooling
                    : out.fanPct > 0 ? ui::HvacAction::kFanOnly
                                     : ui::HvacAction::kIdle);
  gUi.setOutdoor(oat.valueC, oat.valid,
                 oat.rung == OatRung::kBus ? ui::OutdoorSource::kBus
                 : oat.rung == OatRung::kWired ? ui::OutdoorSource::kWiredDs18b20
                 : oat.rung == OatRung::kHaWeather ? ui::OutdoorSource::kHaWeather
                                                   : ui::OutdoorSource::kNone);
  gUi.setGasModulationPct(out.gasHeatPct);
  gUi.setLinkHealth(gWifiConnected, gMqttConnected, hf.busAlive);
  gUi.setDegradedMode(fused.degraded);
  // Per-sensor rows for the Sensors screen + ambient "driving sensor".
  {
    ui::SensorRow rows[ui::kMaxSensorRows];
    uint8_t rn = 0;
    const uint8_t dom = gFusion.dominantParticipant();
    for (size_t i = 0; i < kFusionSlots && rn < ui::kMaxSensorRows; ++i) {
      if (!gSensorTable[i].used) continue;
      const SensorStatus stt = gFusion.status(static_cast<uint8_t>(i), nowS);
      ui::SensorRow& r = rows[rn++];
      strlcpy(r.name, gSensorTable[i].name, sizeof(r.name));
      r.tempC = stt.tempC;
      r.occupied = stt.occupied;
      r.ageS = (stt.ageS == 0xFFFFFFFFu) ? 0 : stt.ageS;
      r.participating = stt.participating || gSensorTable[i].inRoster;
      r.healthy = stt.faults == 0 && stt.hasTemp;
      r.dominant = (static_cast<uint8_t>(i) == dom);
      r.lastOccAgeS = stt.lastOccAgeS;
    }
    gUi.setSensorRows(rows, rn);
  }
  safety::syncAlarmsToUi(gSup->alarms(), gUi);
  uiUnlock();

  // ---- Persistence + outbound snapshot ----
  persistOnChange(nowS);
  fillSnapshot(fused, oat, out, temperActive, changeReason, nowS);
}

void controlTask(void*) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    controlCycle(nowSeconds(), millis());
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(cfg::kControlPeriodMs));
  }
}

#ifdef DETTSON_UI
// Wall-UI task (core 0). Renders gUi (filled by the control task) and routes
// touch into it — display-only, demand authority stays in the control task.
void uiTask(void*) {
  slytherm_ui::begin(&gUi, gUiMux);
  for (;;) {
    slytherm_ui::service();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
#endif

#ifdef DETTSON_DS18B20
OneWire* gOneWire = nullptr;
DallasTemperature* gDallas = nullptr;
uint32_t gLastDsPollS = 0;

void pollLocalSensors(uint32_t nowS) {
  if (!gDallas || nowS - gLastDsPollS < 10) return;
  gLastDsPollS = nowS;
  gDallas->requestTemperatures();
  const float t = gDallas->getTempCByIndex(0);
  // SensorFusion's range gate rejects the -127/+85 sentinels (docs/04 §4).
  gFusion.update(0, t, Occupancy::kUnknown, nowS);
  if (gDallas->getDeviceCount() > 1)
    gOat.submit(OatRung::kWired, gDallas->getTempCByIndex(1), nowS);
}
#endif

uint32_t ctRandomMs(uint32_t lo, uint32_t hi) {
  return lo + (esp_random() % (hi - lo + 1));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("Dettson thermostat (issue #55) — demands "
#if defined(DETTSON_CT485_TX_ENABLE)
                 "TX-ENABLED (Phase 3 build)"
#else
                 "DISABLED (logging/stub build)"
#endif
  );

  if (cfg::kWdtPetPin >= 0) { pinMode(cfg::kWdtPetPin, OUTPUT); digitalWrite(cfg::kWdtPetPin, LOW); }

  gPrefs.begin(cfg::kNvsNamespace, false);

  // Monotonic clock: resume from the persisted base (never backwards).
  gClockBaseS = gPrefs.getUInt("clk", 0) + 1;
  const uint32_t nowS = nowSeconds();
  gCompStopAnchorS = nowS;
  gLastInboundS = nowS;  // stale clock starts at boot, not at epoch

  const esp_reset_reason_t rr = esp_reset_reason();
  const bool abnormalReset = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
                             rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT ||
                             rr == ESP_RST_BROWNOUT;

  // ---- SafetySupervisor + reset-loop accounting (incremented every boot) ----
  gSup = new safety::SafetySupervisor(nowS);
  {
    ResetLoopBlob rl;
    if (gPrefs.getBytes("rl", &rl, sizeof(rl)) == sizeof(rl) &&
        rl.count <= safety::ResetLoopAccountant::kMaxBootHistory) {
      gSup->resetLoop().restore(rl.timesS, rl.count, rl.latched != 0);
    }
    gSup->resetLoop().recordBoot(nowS, abnormalReset);
    ResetLoopBlob outBlob;
    outBlob.latched = gSup->resetLoop().latched() ? 1 : 0;
    outBlob.count = static_cast<uint8_t>(
        gSup->resetLoop().save(outBlob.timesS, safety::ResetLoopAccountant::kMaxBootHistory));
    gPrefs.putBytes("rl", &outBlob, sizeof(outBlob));
    if (outBlob.latched)
      Serial.println("[boot] RESET-LOOP LATCHED: no-demand until manual clear (docs/04 §2)");
  }

  // ---- CompressorGuard restore (full hold-off if blob missing/corrupt) ----
  {
    CompressorGuard::PersistBlob blob;
    const bool haveBlob = gPrefs.getBytes("cg", &blob, sizeof(blob)) == sizeof(blob);
    const uint32_t jitterS = esp_random() % 61;  // 0-60 s (docs/05 table)
    gGuard.bootRestore(haveBlob ? &blob : nullptr, nowS, abnormalReset, jitterS);
    gBootHoldEndS = nowS + kBootCompressorHoldoffS + jitterS;
  }
  gGasShaper.bootRestore(nowS, /*minOffServed=*/false);  // no proof persisted yet

  // ---- Tunables (clamped on load) + control modules ----
  const Tunables tun = loadTunables();
  {
    ModeStateMachine::Config mc;
    mc.minSetpointDeltaC = tun.minSetpointDeltaC;
    gModeSm = new ModeStateMachine(mc);
    DualFuelConfig dc;
    dc.balancePointC = tun.balancePointC;
    dc.compressorMinOatC = tun.compressorMinOatC;
    dc.auxMaxOatC = tun.auxMaxOatC;
    gConfigOk = DualFuelArbiter::configValid(dc);  // hard rule, docs/04 §4
    gDualFuel = new DualFuelArbiter(dc);           // invalid -> validated defaults
    gArbiter = new DemandArbiter(nowS);
  }

  // ---- Restore user state (boot stays no-demand until the gate validates) ----
  {
    PresetDef defaults[kMaxPresets];
    size_t n = 0;
    for (const auto& p : cfg::kDefaultPresets) {
      strlcpy(defaults[n].name, p.name, sizeof(defaults[n].name));
      defaults[n].heatC = p.heatC;
      defaults[n].coolC = p.coolC;
      ++n;
    }
    gModeSm->setPresetRoster(defaults, n);

    gModeSm->setHeatSetpoint(clampF(gPrefs.getFloat("hsp", kFallbackHeatSetpointC),
                                    hm::kClimateMinTempC, hm::kClimateMaxTempC));
    gModeSm->setCoolSetpoint(clampF(gPrefs.getFloat("csp", kFallbackCoolSetpointC),
                                    hm::kClimateMinTempC, hm::kClimateMaxTempC));
    // Restored mode is structurally cross-checked against OAT lockouts
    // (docs/04 §3): all OAT rungs are stale at boot -> fail-cold, so a
    // restored COOL/HP mode cannot demand until a live, permitting OAT.
    const UserMode prior = static_cast<UserMode>(gPrefs.getUChar("pmode", 0));
    const UserMode mode = static_cast<UserMode>(gPrefs.getUChar("mode", 0));
    if (mode == UserMode::kEmergencyHeat && prior != UserMode::kEmergencyHeat)
      gModeSm->setMode(prior, nowS);
    gModeSm->setMode(mode <= UserMode::kEmergencyHeat ? mode : UserMode::kOff, nowS);
    gModeSm->clearHold();  // the boot setMode() must not fabricate a hold
    const HoldType hold = static_cast<HoldType>(gPrefs.getUChar("hold", 0));
    // Timed holds restart their full window — conservative for comfort, and
    // remaining time is not blob-persisted (decision flagged in the report).
    if (hold != HoldType::kNone && hold <= HoldType::kIndefinite)
      gModeSm->startHold(hold, nowS);
    gFanMode = static_cast<hm::FanMode>(gPrefs.getUChar("fan", 0));
    if (gFanMode > hm::FanMode::kCirculate) gFanMode = hm::FanMode::kAuto;
    gSetpointsValidated = true;
  }

  // ---- UiModel screen-lock blob (fails open by design) ----
  {
    ui::UiModel::LockPersistBlob blob;
    if (gPrefs.getBytes("lock", &blob, sizeof(blob)) == sizeof(blob))
      gUi.restoreLock(&blob, nowS);
  }

  // ---- Sensors ----
  gFusion.registerSensor(0, /*isLocal=*/true);  // id 0 = "local" DS18B20 slot
  gSensorTable[0].used = true;
  strlcpy(gSensorTable[0].name, hm::kLocalSensorId, kSensorNameLen);
  gSensorTable[0].inRoster = true;
#ifdef DETTSON_DS18B20
  if (cfg::kOneWirePin >= 0) {
    gOneWire = new OneWire(static_cast<uint8_t>(cfg::kOneWirePin));
    gDallas = new DallasTemperature(gOneWire);
    gDallas->begin();
  }
#endif

  // ---- CT-485 stack (boot-silent; demand frames refused: OffsetVariant
  //      kUnset until the docs/02 §5a capture confirmation) ----
  {
    ct485::Ct485Thermostat::Config cc;
    esp_efuse_mac_get_default(cc.mac);
    for (uint8_t& b : cc.sessionId) b = static_cast<uint8_t>(esp_random());
    cc.randomMs = ctRandomMs;
    gCt = new ct485::Ct485Thermostat(cc);
  }

#if defined(DETTSON_ACTUATOR_RELAY)
  gActuatorPtr = new RelayActuator();
  gActuator = gActuatorPtr;
#else
  gActuator = &gActuatorImpl;
#endif
  Serial.printf("[boot] actuator=%s reset=%d abnormal=%d clk=%lu\n",
                gActuator->name(), static_cast<int>(rr), abnormalReset ? 1 : 0,
                static_cast<unsigned long>(nowS));

  gCmdMux = xSemaphoreCreateMutex();
  gSnapMux = xSemaphoreCreateMutex();
  gCtMux = xSemaphoreCreateMutex();
#ifdef DETTSON_UI
  gUiMux = xSemaphoreCreateMutex();
#endif

  // Task layout (docs/01 §4): Wi-Fi/MQTT core 0; control + CT-485 core 1,
  // CT-485 above control so a slow control cycle can't starve bus timing.
  xTaskCreatePinnedToCore(mqttTask, "mqtt", cfg::kMqttStack, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(controlTask, "control", cfg::kControlStack, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(ct485Task, "ct485", cfg::kCt485Stack, nullptr, 4, nullptr, 1);
#ifdef DETTSON_UI
  // Wall UI on core 0 with Wi-Fi/MQTT; control + CT-485 keep core 1 to protect
  // the control cadence and future TX turnaround (docs/03 §8, issue #28).
  xTaskCreatePinnedToCore(uiTask, "ui", 24576, nullptr, 1, nullptr, 0);
#endif
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));  // all work lives in the pinned tasks
}
