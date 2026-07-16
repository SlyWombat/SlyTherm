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
//                         stubbed unless -DSLYTHERM_CT485_UART
//
// ===================== DEMANDS-DISABLED GUARANTEE (default build) ===========
// The default build CANNOT raise a live demand:
//   - the actuator is LoggingActuator: demands are printed, never transmitted;
//   - Ct485Thermostat boots SILENT and is never resume()d without
//     -DSLYTHERM_CT485_TX_ENABLE, and its Config::offsetVariant is left kUnset,
//     so it refuses demand frames BY DESIGN (docs/02 §5a Phase 2 gate);
//   - no relay GPIO is configured outside -DSLYTHERM_ACTUATOR_RELAY, and even
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
//        mosquitto_pub -r -t slytherm/config/sensors \
//          -m '{"sensors":[{"id":"bench","max_age_s":300}]}'
//   4. Simulate a room sensor (repeat at least every 300 s; non-retained):
//        mosquitto_pub -t slytherm/sensors/bench/state \
//          -m '{"temp":19.5,"occ":true}'
//      and (optionally) outdoor temp for the dual-fuel arbiter:
//        mosquitto_pub -t slytherm/cmd/outdoor_temp -m '5.0'
//   5. Drive it: mosquitto_pub -t slytherm/cmd/mode -m heat
//                mosquitto_pub -t slytherm/cmd/setpoint -m 21.5
//   6. Watch slytherm/state/# — and the serial log, where LoggingActuator
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
#include <ctime>  // #88: time(nullptr) to convert HA's unix last_seen -> monotonic

#include <esp_system.h>
#include <esp_timer.h>
#include <driver/uart.h>          // raw IDF UART1 for CT-485 (core 3.x pin wars)
#include <driver/gpio.h>          // gpio_get_level: RX pad probe in ct485-stats
#include <esp32-hal-periman.h>    // pin-ownership diagnostics for the same

// #113: injected by tools/version_flag.py (VERSION file + git sha). Fallbacks
// keep ad-hoc builds (e.g. PLATFORMIO_BUILD_FLAGS-only invocations) compiling.
#ifndef SLYTHERM_FW_VERSION
#define SLYTHERM_FW_VERSION "0.0.0-dev"
#endif
#ifndef SLYTHERM_FW_BUILD
#define SLYTHERM_FW_BUILD SLYTHERM_FW_VERSION
#endif
// IDF >= 5 (Arduino core 3.x) moved esp_efuse_mac_get_default() out of
// esp_system.h; older cores have no esp_mac.h at all.
#if __has_include(<esp_mac.h>)
#include <esp_mac.h>
#endif

#include "ota_client.h"  // #61: no-op inlines unless -DSLYTHERM_OTA

#include "CompressorGuard.h"
#include "Ct485Core.h"
#include "Ct485Frame.h"
#include "CopLearner.h"
#include "Ct485Thermostat.h"
#include "DemandArbiter.h"
#include "DemandShaper.h"
#include "DettsonConfig.h"
#include "DualFuelArbiter.h"
#include "FanCirculate.h"  // #128: runtime fan-circulate duty + clamp helpers
#include "HaMqtt.h"
#include "LatencyStats.h"  // #28 TX-turnaround jitter probe
#include "ModeStateMachine.h"
#include "OutdoorTempSource.h"
#include "PidShaper.h"
#include "PreCirculator.h"
#include "RecoveryEstimator.h"
#include "RelaySequencer.h"
#include "SafetySupervisor.h"
#include "SensorFusion.h"
#include "SensorParticipation.h"  // durable per-sensor participation (NVS), decoupled from roster
#include "SensorRoster.h"  // #155: resolve slots by wire id OR friendly name
#include "SleepState.h"
#include "TrendEstimator.h"
#include "UiModel.h"

// Running-partition app hash (reset-loop latch clear on a new flash) + the
// #64/#61 OTA capability/rollback calls in setup() — needed on EVERY target,
// not just the UI build (the bench env broke silently while this sat under
// SLYTHERM_UI).
#include <esp_ota_ops.h>

#ifdef SLYTHERM_UI
#include <ESPmDNS.h>      // mDNS broker auto-discovery (silent home-system connect)
#include "slytherm_ui.h"  // LVGL wall-UI binding (compiled only in env:thermostat_s3)
#include "wifi_prov.h"    // on-device Wi-Fi provisioning (owned by the MQTT task)
#include "mqtt_cfg.h"     // on-device broker provisioning (NVS + mDNS)
#include "telnet_log.h"   // WiFi-accessible debug log (port 23)
#endif
#include "boot_guard.h"       // #122/#123: boot counter + crash telemetry (all builds)
#include "coredump_server.h"  // #124: LAN coredump pull (all builds)

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

// Local indoor sensor (SensorFusion slot 0). Default OFF (issue #73): this
// wall unit fuses ONLY the MQTT/HA room sensors — no on-board DS18B20 is
// installed, so slot 0 is not registered, not published, and drops off the
// Sensors screen. Build with -DSLYTHERM_LOCAL_SENSOR to compile the local slot
// back in for other hardware; a physical DS18B20 (-DSLYTHERM_DS18B20) implies it.
#if defined(SLYTHERM_DS18B20) && !defined(SLYTHERM_LOCAL_SENSOR)
#define SLYTHERM_LOCAL_SENSOR
#endif

#ifdef SLYTHERM_DS18B20
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
// Guards gUi across the control task and the wall-UI task (SLYTHERM_UI). Null
// (no-op) when the UI task isn't built, so all wraps stay compile-safe.
SemaphoreHandle_t gUiMux = nullptr;
inline void uiLock()   { if (gUiMux) xSemaphoreTake(gUiMux, portMAX_DELAY); }
inline void uiUnlock() { if (gUiMux) xSemaphoreGive(gUiMux); }

constexpr size_t kFusionSlots = SensorFusion::kMaxSensors;  // 8: local + 7 remotes
constexpr size_t kSensorNameLen = 24;

struct SensorEntry {
  bool used = false;
  char name[kSensorNameLen] = {};   // topic id / wire segment (unchanged)
  char disp[kSensorNameLen] = {};   // #85: friendly display label; empty -> fall back to name
  bool inRoster = false;      // is a member of the configured roster (HA-owned, retained topic)
  bool participating = true;  // user's persisted "include in fusion" choice (NVS); default ON.
                              // Fusion gates on inRoster && participating — decoupled so a roster
                              // replay/reconnect can't clobber an OFF (dettson::fusionParticipates).
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
  bool manualSetpoint = false; // hasLow/hasHigh came from a person tapping a REMOTE
                               // panel (arms the 4h hold like the wall panel), vs an
                               // HA cmd/heat|cool scheduler write (stays no-hold, #91)
  bool hasFan = false;         hm::FanMode fan = hm::FanMode::kAuto;
  bool hasFanCircMin = false;  uint32_t fanCircMin = 0;   // #128 circulate minutes-per-hour
  bool hasFanCircPct = false;  float fanCircPct = 0;      // #128 circulate speed %
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
  // #88: HA-reported presence, converted to the monotonic timebase in the MQTT
  // handler (hasSeen=false when the timestamp is unknown / clock unsynced).
  struct PresenceSample { uint8_t idx; bool occupied; uint32_t lastSeenS; bool hasSeen; };
  PresenceSample presence[16]; size_t presenceCount = 0;
  // #90: HA sleep override (retained slytherm/cmd/sleep: on/off/auto).
  bool hasSleepOverride = false; SleepOverride sleepOverride = SleepOverride::kAuto;
  // #143: retained energy prices (slytherm/cmd/energy_prices) for the
  // economic switchover; validated again by DualFuelArbiter::setConfig.
  bool hasEnergyPrices = false; hm::EnergyPrices energyPrices;
  // #118: Remote vacation/ack intents (applied via the same control-task
  // functions the local UI's popIntent path uses).
  bool hasVacation = false;
  uint16_t vacStartDays = 0, vacNights = 1;
  float vacHeatC = 0, vacCoolC = 0;
  bool hasClearVacation = false;
  bool hasAckAlarms = false;
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
  uint32_t fanCircMin = 0;   // #128 circulate minutes-per-hour (retained state)
  float fanCircPct = 0;      // #128 circulate speed %
  char preset[kPresetNameMaxLen + 1] = {};
  HoldType holdType = HoldType::kNone; uint32_t holdRemainS = 0;
  char action[16] = "off";
  char equipment[12] = "idle";
  float modulationPct = 0;
  bool oatValid = false; float oatC = 0; OatRung oatRung = OatRung::kNone;
  char fusionJson[224] = "{}";  // {temp,tier,participants[],occupied,dominant} (#117)
  char statusLine[40] = "Idle";      // composed action wording (wall-screen parity)
  char trackingLine[56] = {};        // composed presence/tracking wording
  uint32_t compMinOffRemainS = 0;
  bool compLockedOut = false;
  char changeReason[24] = "none";
  bool asleep = false;  // #90: night Sleep state (slytherm/state/sleep)
  bool healthProblem = false;
  char lastError[safety::kAlarmTextLen] = {};
  // #116/#118: Remote-echo extras — alarm summary (count + first two RAW
  // texts; the Remote renders them through friendlyAlarm()) + vacation banner.
  uint8_t alarmN = 0;
  char alarm1[safety::kAlarmTextLen] = {};
  char alarm2[safety::kAlarmTextLen] = {};
  bool vacationActive = false;
  char vacBanner[32] = {};
  hm::LockState lockState = hm::LockState::kUnlocked;
  hm::LockLevel lockLevel = hm::LockLevel::kSettingsOnly;
  bool pinSet = false;
  char busJson[176] = "{}";
#ifdef SLYTHERM_ACTUATOR_RELAY
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
volatile bool gCopProxyPubReq = false; // #143: control task staged fresh COP-proxy telemetry
std::string   gCopProxyJson;           // guarded by gCmdMux

volatile bool gMqttConnected = false;
volatile bool gWifiConnected = false;
bool gClock24 = false;  // top-bar clock format (12h default); persisted in NVS "clk24"
bool gReducedUi = false;  // #80: boot the minimal safe UI (reset-loop latch); NVS "rui"
bool gFirstRun = false;   // #82: no saved Wi-Fi -> boot the Welcome onboarding screen

// ============================================================================
// Modules (constructed in setup() once the persisted clock is known)
// ============================================================================
Preferences gPrefs;
// Durable per-sensor participation (NVS key "part"), keyed by wire id. Guarded
// by gCmdMux — mutated from the MQTT task (roster/cmd handlers) and the UI task
// (uiToggleSensor), read by handleSensorRoster. See lib/SensorParticipation.
dettson::SensorParticipation gParticipation;
using ParticipationBlob = dettson::SensorParticipation::PersistBlob;
static_assert(kSensorNameLen == dettson::SensorParticipation::kIdLen,
              "participation store key length must match the sensor wire-id length");
// gCmdMux held. Records the wire-id-keyed choice, mirrors it into the slot, and
// persists to NVS on change. The write stays INSIDE the mutex on purpose: "part"
// is written from two tasks (uiToggleSensor on the UI task, the cmd handler on
// the MQTT task), so serializing build+write under the one lock is what prevents
// a concurrent double-write from dropping a setting. The ~15ms NVS hold is rare
// (only on an actual change) and gCmdMux is otherwise held only briefly.
// Set by setParticipationLocked (UI/cmd task), consumed by publishSnapshot on the
// mqttTask. Publishing MQTT directly from setParticipationLocked would race the
// mqttTask's own PubSubClient use (not thread-safe); the flag hands the retained
// participating echo to the state-publish path so it goes out within one cycle of
// a change instead of waiting for the next heartbeat.
volatile bool gParticipationEchoDirty = false;
static void setParticipationLocked(uint8_t idx, const char* wireId, bool on) {
  const bool changed = gParticipation.set(wireId, on);
  gSensorTable[idx].participating = on;
  if (changed) {
    ParticipationBlob blob;
    gParticipation.toBlob(blob);
    gPrefs.putBytes("part", &blob, sizeof(blob));
  }
  gParticipationEchoDirty = true;  // re-echo even on an idempotent re-assert (a
                                   // prior retained echo may have been lost)
}
SensorFusion        gFusion;
// #90 night Sleep state: window/idle/override logic (lib/SleepState) + the
// reversible sleep-preset glue. Control-task-only; touch arrives via the
// gUiTouchPing flag below.
SleepState          gSleep;
SleepPresetLink     gSleepLink;
volatile bool       gUiTouchPing = false;  // UI task -> control task touch note
OutdoorTempSource   gOat;
ModeStateMachine*   gModeSm   = nullptr;
DualFuelArbiter*    gDualFuel = nullptr;
CompressorGuard     gGuard;
DemandArbiter*      gArbiter  = nullptr;
PidShaper           gPid;
GasShaper           gGasShaper;
RecoveryEstimator   gRecovery;
CopLearner          gCopLearner;  // #143: record-only COP proxy (docs/13 §5)
TrendEstimator      gTrend;  // fused-temp slope for crossing prediction (#141)
PreCirculator       gPreCirc;  // blower-first pre-circulation (#142, docs/13 §3+§8)
ui::UiModel         gUi;
ui::UiModel::LockPersistBlob gShadowLock{};  // last lock state written to NVS (change-detect)
safety::SafetySupervisor* gSup = nullptr;
ct485::Ct485Thermostat*   gCt = nullptr;

// #28 TX-turnaround probe bench-stress flag (defined early: the MQTT command
// handler sets it, ct485Task reads it, the UI reads it via uiTxTurnStress()).
// Unconditional so the extern accessor always links; runtime-only, never
// persisted (defaults OFF every boot so the passive probe rides shadow windows).
volatile bool gTxTurnStress = false;

// --- CompressorGuard adapters -----------------------------------------------
// Glue-side stop anchor for the min-off-remaining diagnostic and the idle
// gates; starts at boot (prior state unknown -> assume just stopped).
uint32_t gCompStopAnchorS = 0;
uint32_t gBootHoldEndS = 0;

// ---- On-device Vacation hold (issue #78) --------------------------------------
// A long hold whose eco setpoints are gated by a calendar window. The on-device
// clock (#69) anchors the window to local midnights; while `now` is inside the
// window the eco setpoints override schedule + presence, then auto-resume at end
// (prior setpoints restored). Persisted as one NVS blob so it survives reboots.
struct VacationState {
  bool     on         = false;   // a vacation is configured
  bool     anchored   = false;   // epochs computed (needs a valid clock)
  bool     applied    = false;   // eco setpoints currently forced (prior* captured)
  uint16_t startDays  = 0;       // start offset from set-day midnight (0 = today)
  uint16_t nights     = 1;       // length in nights (>=1)
  uint32_t startEpoch = 0;       // unix seconds, local-midnight aligned
  uint32_t endEpoch   = 0;
  float    heatC      = 16.0f;   // eco heat setpoint applied in-window
  float    coolC      = 28.0f;   // eco cool setpoint applied in-window
  float    priorHeatC = 20.0f;   // captured on entry, restored on exit
  float    priorCoolC = 24.0f;
};
VacationState gVac;
bool gVacUiActive = false;        // banner visible flag (pushed to gUi under uiLock)
char gVacBanner[32] = "";         // "Vacation until Jul 12"

static void saveVacation() { gPrefs.putBytes("vac", &gVac, sizeof(gVac)); }
static bool clockIsSynced() { return time(nullptr) > 1735689600L; }  // after 2025-01-01 => NTP set
static bool anchorVacation() {
  struct tm ti;
  if (!getLocalTime(&ti, 0)) return false;
  ti.tm_hour = 0; ti.tm_min = 0; ti.tm_sec = 0;   // local midnight of the set-day
  time_t mid = mktime(&ti);
  if (mid <= 0) return false;
  gVac.startEpoch = static_cast<uint32_t>(mid + static_cast<time_t>(gVac.startDays) * 86400L);
  gVac.endEpoch   = static_cast<uint32_t>(mid + static_cast<time_t>(gVac.startDays + gVac.nights) * 86400L);
  gVac.anchored = true;
  return true;
}
// Evaluated every control cycle BEFORE the mode state machine so the eco
// setpoints are in place when demand is computed.
void evaluateVacation(uint32_t /*nowS*/) {
  if (!gVac.on) { gVacUiActive = false; gVacBanner[0] = 0; return; }
  const bool clockOk = clockIsSynced();
  if (!gVac.anchored) {                       // set while NTP was still cold: anchor once synced
    if (clockOk && anchorVacation()) saveVacation();
    else { gVacUiActive = true; strlcpy(gVacBanner, "Vacation set", sizeof(gVacBanner)); return; }
  }
  const time_t nowE = time(nullptr);
  if (clockOk && static_cast<uint32_t>(nowE) >= gVac.endEpoch) {   // window elapsed -> auto-resume
    if (gVac.applied && gModeSm) {
      gModeSm->setHeatSetpoint(gVac.priorHeatC);
      gModeSm->setCoolSetpoint(gVac.priorCoolC);
    }
    gVac = VacationState{};        // clear all (on=false)
    saveVacation();
    gVacUiActive = false; gVacBanner[0] = 0;
    Serial.println("[vac] window ended -> resume normal operation");
    return;
  }
  const bool inWindow = clockOk && static_cast<uint32_t>(nowE) >= gVac.startEpoch;
  if (inWindow && gModeSm) {        // force eco each cycle -> overrides schedule + presence
    if (!gVac.applied) {
      gVac.priorHeatC = gModeSm->heatSetpoint();
      gVac.priorCoolC = gModeSm->coolSetpoint();
      gVac.applied = true; saveVacation();
      Serial.printf("[vac] active: eco heat %.1f cool %.1f (was %.1f/%.1f)\n",
                    (double)gVac.heatC, (double)gVac.coolC,
                    (double)gVac.priorHeatC, (double)gVac.priorCoolC);
    }
    gModeSm->setHeatSetpoint(gVac.heatC);
    gModeSm->setCoolSetpoint(gVac.coolC);
  }
  // Banner: "Vacation until <end>" while active, "Vacation <start>" while scheduled.
  char d[16];
  { time_t e = static_cast<time_t>(inWindow ? gVac.endEpoch : gVac.startEpoch);
    struct tm et; localtime_r(&e, &et); strftime(d, sizeof(d), "%b %d", &et); }
  snprintf(gVacBanner, sizeof(gVacBanner), inWindow ? "Vacation until %s" : "Vacation %s", d);
  gVacUiActive = true;
}

// A MANUAL (user-initiated) setpoint/mode change should get its compressor
// demand out IMMEDIATELY — the CompressorGuard min-OFF exists to stop the
// AUTOMATIC control loop from oscillating, not to block an explicit human
// request (the ODU's own ~3-min restart delay is the physical backstop,
// docs/04 §1a). The setpoint/mode change path (MQTT + on-panel intents,
// below) arms this one-shot; the next compressor start consumes it and passes
// manual=true to CompressorGuard::requestStart, which bypasses min-OFF ONLY.
// A short expiry keeps a stale arm from ever bypassing an unrelated later
// AUTOMATIC start; max-starts/hour + boot hold-off + lockout are NEVER
// bypassed even for a manual start.
constexpr uint32_t kManualStartArmWindowS = 120;  // arm applies to a start within ~2 min of the user action

struct GuardGate : public CompressorGate {
  bool     manualArmed_ = false;
  uint32_t manualArmS_  = 0;
  void armManual(uint32_t nowS) { manualArmed_ = true; manualArmS_ = nowS; }

  bool canStart(uint32_t nowS) override {
    const bool manual =
        manualArmed_ && (nowS - manualArmS_) <= kManualStartArmWindowS;
    const auto d = gGuard.requestStart(nowS, manual);
    if (manual && d.allowed) manualArmed_ = false;  // consume on a committed start
    return d.allowed;
  }
  bool canStop(uint32_t nowS) override {
    if (!gGuard.requestStop(nowS, /*safety=*/false).allowed) return false;
    gCompStopAnchorS = nowS;
    return true;
  }
};
GuardGate gGuardGate;
HpRelayShaper gHpShaper(gGuardGate);
// Cooling is single-stage at 30% on this furnace (#140, field-confirmed):
// its own runtime-duty shaper, sharing the one compressor's guard gate.
StagedCoolShaper gCoolShaper(gGuardGate);

bool compressorProvenIdle(uint32_t nowS) {
  return !gGuard.running() &&
         nowS >= gCompStopAnchorS + kCompressorMinOffS &&
         nowS >= gBootHoldEndS;
}

// Guard-side compressor min-OFF remaining (CompressorGuard 180 s min-OFF plus
// the post-boot hold-off): the single source of truth for both the HA
// compressor_min_off_remaining sensor snapshot AND the on-panel "Cooling/
// Heating soon" acknowledgement. 0 while the compressor runs or the rest is
// served. NB: the reset-loop LOCKOUT latch is NOT reflected here — that is
// gGuard.lockedOut(), and callers surfacing "soon" must exclude it (the anchor
// countdown would otherwise imply a short wait against a forever-latch).
uint32_t compGuardMinOffRemainS(uint32_t nowS) {
  if (gGuard.running()) return 0;
  uint32_t end = gCompStopAnchorS + kCompressorMinOffS;
  if (gBootHoldEndS > end) end = gBootHoldEndS;
  return nowS < end ? end - nowS : 0;
}

void compressorSafetyStop(uint32_t nowS) {
  if (gGuard.running()) {
    gGuard.requestStop(nowS, /*safety=*/true);
    gCompStopAnchorS = nowS;
  }
  gHpShaper.reset();
  gCoolShaper.reset();
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

#ifdef SLYTHERM_ACTUATOR_CT485
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

#ifdef SLYTHERM_ACTUATOR_RELAY
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

  // slytherm/state/relays diagnostic JSON (docs/06 topic map, Case B).
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

#if defined(SLYTHERM_ACTUATOR_CT485)
Ct485Actuator gActuatorImpl;
#elif defined(SLYTHERM_ACTUATOR_RELAY)
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
  bool economicEnabled;
  float elecPricePerKwh, gasPricePerM3;
};

Tunables loadTunables() {
  Tunables t;
  // Clamped on load to the docs/05 canonical ranges.
  t.balancePointC     = clampF(gPrefs.getFloat("bp",  kBalancePointC),     -30.0f, 15.0f);
  t.compressorMinOatC = clampF(gPrefs.getFloat("cmo", kCompressorMinOatC), -30.0f, 15.0f);
  t.auxMaxOatC        = clampF(gPrefs.getFloat("amo", kAuxMaxOatC),        -30.0f, 30.0f);
  t.minSetpointDeltaC = clampF(gPrefs.getFloat("spd", kMinSetpointDeltaC),
                               kMinSetpointDeltaFloorC, 8.0f);
  // #143 economic switchover: default OFF (winter validation); prices are
  // last-accepted slytherm/cmd/energy_prices (the retained topic re-feeds
  // them on connect anyway — NVS covers broker-less boots).
  t.economicEnabled  = gPrefs.getBool("dfec", kDualFuelEconomicEnabledDefault);
  t.elecPricePerKwh  = clampF(gPrefs.getFloat("epk", kElecPricePerKwhDefault),
                              0.001f, kEnergyPriceMax);
  t.gasPricePerM3    = clampF(gPrefs.getFloat("gpm", kGasPricePerM3Default),
                              0.001f, kEnergyPriceMax);
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
#ifdef SLYTHERM_LOCAL_SENSOR
  if (strcmp(name, hm::kLocalSensorId) == 0) return 0;
#endif
  // #155: match wire id OR #85 friendly label so the panel path (passes disp)
  // and the cmd/sensor/<id>/... path (passes id) both resolve.
  return slyroster::findSlot(gSensorTable, 1, kFusionSlots, name, kSensorNameLen);
}

#ifdef SLYTHERM_REMOTE_LINK
// #104: Controller identity for the Remote-link heartbeat — last 3 MAC bytes,
// lowercase hex, no colons (MQTT-topic-safe; docs/11 "cid derivation").
std::string controllerCid() {
  static std::string cid;
  if (!cid.empty()) return cid;
  std::string mac = WiFi.macAddress().c_str();  // "AA:BB:CC:DD:EE:FF"
  for (size_t i = 9; i < mac.size(); ++i) {
    if (mac[i] == ':') continue;
    char c = mac[i];
    if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    cid += c;
  }
  return cid;
}

// #104: dedupe Remote UiIntents by (remoteId, monotonic id) — a Remote's own
// retry-on-no-ack can redeliver the same intent; a stale/reordered redelivery
// must not reapply after the user has moved on. Small fixed table (a handful
// of Remotes per system, docs/11 "multi-Remote"); table-full fails open
// (this is a comfort dedupe, not a safety gate — see gCmdMux callers below).
struct RemoteIntentDedupe { bool used = false; char id[kSensorNameLen] = {}; uint32_t lastId = 0; };
RemoteIntentDedupe gRemoteDedupe[4];  // gCmdMux held

bool remoteIntentIsDuplicate(const char* remoteId, uint32_t id) {  // gCmdMux held
  for (auto& d : gRemoteDedupe) {
    if (d.used && strncmp(d.id, remoteId, sizeof(d.id)) == 0) {
      if (d.lastId != 0 && id <= d.lastId) return true;
      d.lastId = id;
      return false;
    }
  }
  for (auto& d : gRemoteDedupe) {
    if (!d.used) {
      d.used = true;
      strlcpy(d.id, remoteId, sizeof(d.id));
      d.lastId = id;
      return false;
    }
  }
  return false;  // table full: process anyway rather than silently drop
}
#endif  // SLYTHERM_REMOTE_LINK

void handleSensorRoster(const char* json) {
  std::vector<hm::SensorRosterEntry> entries;
  if (!hm::parseSensorRosterJson(json, entries)) return;
  xSemaphoreTake(gCmdMux, portMAX_DELAY);
  for (size_t i = 1; i < kFusionSlots; ++i) gSensorTable[i].inRoster = false;
  for (const auto& e : entries) {
    uint8_t idx = findOrAddSensor(e.id.c_str());
    if (idx == 0) continue;  // table full (fusion mask is 8 wide)
    SensorEntry& s = gSensorTable[idx];
    // Mark roster membership AND apply the user's PERSISTED participation choice
    // (default ON) — never force true. This is the fix: a RETAINED-roster replay
    // (reboot / MQTT reconnect) must leave a sensor the user turned OFF still off.
    dettson::applyRosterMember(s, e.id.c_str(), gParticipation);
    s.hasMaxAge = e.hasMaxAge;
    if (e.hasMaxAge) s.maxAgeS = e.maxAgeS;
    s.offsetC = e.offsetC;
    strlcpy(s.disp, e.name.c_str(), sizeof(s.disp));  // #85: friendly label ("" when roster omits "name")
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

extern "C" void uiSniffStart();  // defined with the LISTEN globals below
extern "C" void uiSniffStop();

void onMqttMessage(char* topic, uint8_t* payload, unsigned int len) {
  static char buf[1100];
  if (len >= sizeof(buf)) return;
  memcpy(buf, payload, len);
  buf[len] = '\0';

#ifdef SLYTHERM_REMOTE_LINK
  // #104: slytherm/remote/<id>/intent — Remote -> Controller UiIntent.
  // Ingested into the same gPending mailbox the HA cmd/* topics use, so it is
  // re-validated by the control task exactly like any other inbound command
  // (docs/11 "Authority": the Remote never invents authoritative state).
  {
    const char* riPrefix = hm::topic::kRemoteIntentTopicPrefix;
    const size_t riPrefixLen = strlen(riPrefix);
    if (strncmp(topic, riPrefix, riPrefixLen) == 0) {
      const char* id = topic + riPrefixLen;
      const char* slash = strchr(id, '/');
      if (slash != nullptr && strcmp(slash, hm::topic::kRemoteIntentTopicSuffix) == 0) {
        char remoteId[kSensorNameLen];
        size_t n = static_cast<size_t>(slash - id);
        if (n == 0 || n >= sizeof(remoteId)) return;
        memcpy(remoteId, id, n);
        remoteId[n] = '\0';
        hm::RemoteIntent ri;
        if (hm::parseRemoteIntentJson(buf, ri)) {
          xSemaphoreTake(gCmdMux, portMAX_DELAY);
          if (!remoteIntentIsDuplicate(remoteId, ri.id)) {
            switch (ri.type) {
              case hm::RemoteIntentType::kSetpoints:
                gPending.hasLow = true; gPending.lowC = ri.heatC;
                gPending.hasHigh = true; gPending.highC = ri.coolC;
                gPending.manualSetpoint = true;  // a person tapped a remote panel:
                // treat like the wall panel (arm the 4h manual hold), not an HA write
                break;
              case hm::RemoteIntentType::kMode:
                gPending.hasMode = true; gPending.mode = ri.mode;
                break;
              case hm::RemoteIntentType::kPreset:
                gPending.hasPreset = true;
                strlcpy(gPending.preset, ri.preset.c_str(), sizeof(gPending.preset));
                break;
              case hm::RemoteIntentType::kHold:
                gPending.hasHold = true;
                gPending.hold = hm::HoldCommand{};
                gPending.hold.type = ri.hold;
                break;
              case hm::RemoteIntentType::kClearHold:
                gPending.hasHold = true;
                gPending.hold = hm::HoldCommand{};
                gPending.hold.clear = true;
                break;
              case hm::RemoteIntentType::kVacation:  // #118
                gPending.hasVacation = true;
                gPending.vacStartDays = ri.vacStartDays;
                gPending.vacNights = ri.vacNights;
                gPending.vacHeatC = ri.heatC;
                gPending.vacCoolC = ri.coolC;
                break;
              case hm::RemoteIntentType::kClearVacation:  // #118
                gPending.hasClearVacation = true;
                break;
              case hm::RemoteIntentType::kAckAlarms:  // #118
                gPending.hasAckAlarms = true;
                break;
            }
            gPending.anyInbound = true;
          }
          xSemaphoreGive(gCmdMux);
        }
        return;
      }
    }
  }
#endif  // SLYTHERM_REMOTE_LINK

  // Wildcard topics first.
  const char* sensPrefix = "slytherm/sensors/";
  const char* offPrefix = "slytherm/cmd/sensor/";
  if (strncmp(topic, sensPrefix, strlen(sensPrefix)) == 0) {
    const char* id = topic + strlen(sensPrefix);
    const char* slash = strchr(id, '/');
    if (!slash) return;
    char name[kSensorNameLen];
    size_t n = static_cast<size_t>(slash - id);
    if (n == 0 || n >= sizeof(name)) return;
    memcpy(name, id, n); name[n] = '\0';
    // #88: retained presence — {"occupied":bool,"last_seen":<unix>}. Convert HA's
    // unix last_seen into our monotonic timebase using the wall clock, then queue.
    if (strcmp(slash, "/presence") == 0) {
      hm::PresenceReading pr;
      if (!hm::parsePresenceJson(buf, pr)) return;
      xSemaphoreTake(gCmdMux, portMAX_DELAY);
      uint8_t idx = findSensor(name);
      if (idx != 0xFF && idx != 0 && gPending.presenceCount < 16) {
        const bool occupied = pr.hasOccupied ? pr.occupied : false;
        const uint32_t nowMono = nowSeconds();
        uint32_t lastSeenMono = 0; bool hasSeen = false;
        if (pr.hasLastSeen) {
          const time_t epoch = time(nullptr);
          if (epoch > static_cast<time_t>(1600000000) &&
              pr.lastSeen <= static_cast<uint32_t>(epoch)) {
            const uint32_t age = static_cast<uint32_t>(epoch) - pr.lastSeen;
            lastSeenMono = nowMono > age ? nowMono - age : 0;
            hasSeen = true;
          } else if (occupied) {          // clock unsynced but occupied now -> seen now
            lastSeenMono = nowMono; hasSeen = true;
          }  // future/implausible last_seen while vacant -> leave unplaced
        } else if (occupied) {
          lastSeenMono = nowMono; hasSeen = true;
        }
        gPending.presence[gPending.presenceCount++] = {idx, occupied, lastSeenMono, hasSeen};
        gPending.anyInbound = true;
      }
      xSemaphoreGive(gCmdMux);
      return;
    }
    if (strcmp(slash, "/state") != 0) return;
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
    if (!slash) return;
    char name[kSensorNameLen];
    size_t n = static_cast<size_t>(slash - id);
    if (n == 0 || n >= sizeof(name)) return;
    memcpy(name, id, n); name[n] = '\0';
    // #119: slytherm/cmd/sensor/<id>/participating (ON/OFF) — absolute set,
    // same roster-dirty path uiToggleSensor uses (re-validated by control).
    if (strcmp(slash, "/participating") == 0) {
      const bool on = strcmp(buf, "ON") == 0;
      if (!on && strcmp(buf, "OFF") != 0) return;  // reject junk payloads
      xSemaphoreTake(gCmdMux, portMAX_DELAY);
      uint8_t idx = findSensor(name);  // topic carries the wire id
      if (idx != 0 && idx != 0xFF && idx < kFusionSlots && gSensorTable[idx].used) {
        // #119: absolute set of the PARTICIPATION choice (not roster membership),
        // persisted to NVS so it survives the next reconnect/reboot.
        setParticipationLocked(idx, gSensorTable[idx].name, on);
        gPending.sensorRosterDirty = true;
        gPending.anyInbound = true;
        gDiscoveryDirty = true;
      }
      xSemaphoreGive(gCmdMux);
      return;
    }
    if (strcmp(slash, "/offset") != 0) return;
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
#ifdef SLYTHERM_OTA
  // #61: payload deliberately ignored — any message triggers; the OTA task
  // no-ops requests that arrive mid-phase.
  if (strcmp(topic, hm::topic::kCmdOtaCheck) == 0) { ota::requestCheck(); return; }
  if (strcmp(topic, hm::topic::kCmdOtaApply) == 0) { ota::requestApply(); return; }
#endif

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
  } else if (strcmp(topic, hm::topic::kCmdFanCirculateMin) == 0) {
    // #128: circulate minutes-per-hour (HA number / Remote panel). Strict int,
    // clamped 0-60; junk rejected (never a half-applied config).
    char* end = nullptr; const long v = strtol(buf, &end, 10);
    if (end != buf && *end == '\0') { gPending.hasFanCircMin = true; gPending.fanCircMin = fan::clampCirculateMinPerHour(v); }
    else accepted = false;
  } else if (strcmp(topic, hm::topic::kCmdFanCirculatePct) == 0) {
    // #128: circulate speed %, snapped to the nearest field-confirmed Low/Med/High.
    char* end = nullptr; const long v = strtol(buf, &end, 10);
    if (end != buf && *end == '\0') { gPending.hasFanCircPct = true; gPending.fanCircPct = fan::snapCirculatePct(static_cast<float>(v)); }
    else accepted = false;
  } else if (strcmp(topic, hm::topic::kCmdPreset) == 0) {
    if (len > 0 && len <= kPresetNameMaxLen) {
      gPending.hasPreset = true;
      strlcpy(gPending.preset, buf, sizeof(gPending.preset));
    } else accepted = false;
  } else if (strcmp(topic, hm::topic::kCmdHold) == 0) {
    // HA hold-duration select (#81) sends "none" to resume the schedule; the
    // parser reserves "clear", so map the select's no-hold option to a clear.
    if (strcmp(buf, "none") == 0) { gPending.hasHold = true; gPending.hold = hm::HoldCommand{}; gPending.hold.clear = true; }
    else { auto p = hm::parseHoldCommand(buf);
      if (p.ok) { gPending.hasHold = true; gPending.hold = p.value; } else accepted = false; }
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
  } else if (strcmp(topic, hm::topic::kCmdEnergyPrices) == 0) {
    // #143: retained energy prices for the economic switchover. Strict parse;
    // a rejected payload never moves the switchover point.
    hm::EnergyPrices ep;
    if (hm::parseEnergyPricesJson(buf, ep)) { gPending.hasEnergyPrices = true; gPending.energyPrices = ep; }
    else accepted = false;
  } else if (strcmp(topic, cfg::kOatTopic) == 0) {
    auto p = hm::parseSetpoint(buf, cfg::kOatIngestMinC, cfg::kOatIngestMaxC);
    if (p.ok) { gPending.hasOat = true; gPending.oatC = p.value; } else accepted = false;
  } else if (strcmp(topic, "slytherm/cmd/ota_mirror") == 0) {
    // #129: set/clear the LAN OTA mirror ("" or "clear" -> GitHub direct).
    ota::setMirror(strcmp(buf, "clear") == 0 ? "" : buf);
  } else if (strcmp(topic, "slytherm/cmd/sniff") == 0) {
    // Remote LISTEN control (#71 follow-up): start/stop the CT-485 capture
    // mirror without hands on the glass. Same persisted hooks as the LISTEN
    // screen buttons; the Diag LISTEN screen stays a live viewer either way.
    if (strcmp(buf, "on") == 0 || strcmp(buf, "ON") == 0 || strcmp(buf, "1") == 0)
      uiSniffStart();
    else if (strcmp(buf, "off") == 0 || strcmp(buf, "OFF") == 0 || strcmp(buf, "0") == 0)
      uiSniffStop();
    else accepted = false;
#if defined(SLYTHERM_TXTURN_PROBE)
  } else if (strcmp(topic, "slytherm/cmd/txturn_stress") == 0) {
    // #28 bench-only: force worst-case LVGL load (continuous full-screen
    // redraws, core 0) while the turnaround probe (core 1) captures max jitter.
    // NOT persisted — a deliberate toggle for the post-shadow-window bench run;
    // every boot defaults OFF so the passive probe rides shadow windows clean.
    if (strcmp(buf, "on") == 0 || strcmp(buf, "ON") == 0 || strcmp(buf, "1") == 0)
      gTxTurnStress = true;
    else if (strcmp(buf, "off") == 0 || strcmp(buf, "OFF") == 0 || strcmp(buf, "0") == 0)
      gTxTurnStress = false;
    else accepted = false;
#endif
  } else if (strcmp(topic, "slytherm/cmd/sleep") == 0) {
    // #90 HA sleep override (retain-friendly): "on" forces the Sleep state
    // (e.g. a bedroom sensor / HA sleep mode), "off" forces awake, "auto"
    // returns control to the night window.
    if (strcmp(buf, "on") == 0 || strcmp(buf, "ON") == 0 || strcmp(buf, "1") == 0)
      { gPending.hasSleepOverride = true; gPending.sleepOverride = SleepOverride::kForceAsleep; }
    else if (strcmp(buf, "off") == 0 || strcmp(buf, "OFF") == 0 || strcmp(buf, "0") == 0)
      { gPending.hasSleepOverride = true; gPending.sleepOverride = SleepOverride::kForceAwake; }
    else if (strcmp(buf, "auto") == 0)
      { gPending.hasSleepOverride = true; gPending.sleepOverride = SleepOverride::kAuto; }
    else accepted = false;
  } else {
    accepted = false;
  }
  if (accepted) gPending.anyInbound = true;
  xSemaphoreGive(gCmdMux);
}

bool pubRetained(const std::string& topic, const std::string& payload) {
  return gMqtt.publish(topic.c_str(), payload.c_str(), true);
}

// ---- Wall-UI hooks (called from the LVGL UI task) --------------------------
// 12/24h clock toggle (#69): flip + persist; the control task reads gClock24.
extern "C" void uiToggleClock24() { gClock24 = !gClock24; gPrefs.putBool("clk24", gClock24); }
extern "C" bool uiClock24() { return gClock24; }

// #90: press-edge touch note from the UI task's GT911 callback. A bare flag
// (no timestamp) so the cross-task handoff is race-free; the control task
// stamps it with its own clock via gSleep.noteTouch().
extern "C" void uiNoteTouch() { gUiTouchPing = true; }

// #80 safe-UI recovery: clear the reduced-UI latch + the reset-loop history and
// reboot into the full UI. Called from the wall UI's "Restore full screen"
// button (the deliberate manual clear). Control-side no-demand latch is cleared
// too, so a fresh boot starts clean instead of immediately re-latching.
extern "C" void uiClearReducedMode() {
  // Writes gPrefs from the UI task (the control task also writes it); benign —
  // we esp_restart() 50 ms later, so the outcome is identical either way.
  gPrefs.putBool("rui", false);
  ResetLoopBlob z{};  // latched=0, count=0, times zeroed
  gPrefs.putBytes("rl", &z, sizeof(z));
  delay(50);
  esp_restart();
}

// Sensor participation toggle (#68): flip the room's PARTICIPATION choice
// (not roster membership — the roster is HA-owned and retained), persist it to
// NVS, and re-fuse. The panel hands the #85 DISPLAY name; findSensor resolves it
// to the slot, and we persist under the slot's WIRE id so a roster replay —
// which keys by wire id — applies the same choice and cannot clobber it (#155).
extern "C" void uiToggleSensor(const char* name) {
  if (!name || !name[0]) return;
  xSemaphoreTake(gCmdMux, portMAX_DELAY);
  uint8_t idx = findSensor(name);
  if (idx == 0 || idx == 0xFF || idx >= kFusionSlots || !gSensorTable[idx].used) { xSemaphoreGive(gCmdMux); return; }
  setParticipationLocked(idx, gSensorTable[idx].name, !gSensorTable[idx].participating);
  gPending.sensorRosterDirty = true;
  gPending.anyInbound = true;
  gDiscoveryDirty = true;
  xSemaphoreGive(gCmdMux);
}

// RS-485 LISTEN capture (#71). Single-writer (ct485Task) -> single-reader (UI
// task) volatiles + a tiny newest-first ring; benign races are fine for a
// diagnostic. The telnet stream (:23) is the real capture artifact — its
// preview line runs wide (up to 16 payload bytes); the on-screen ring is
// truncated to 56 chars. sniffFrame() (writer) lives with the ct485Task below.
volatile bool gSniffActive = false;
volatile uint32_t gSniffFrames = 0;
struct SniffLine { char s[56]; };
SniffLine gSniffRing[10] = {};
volatile uint8_t gSniffHead = 0;   // next write slot (0..9), wraps at 10

// Hooks: the UI task (LISTEN buttons) and the MQTT cmd handler flip
// gSniffActive; the UI reads the volatiles/ring above. Persisted so a power
// blip mid-capture-campaign resumes capturing on boot (restored in setup()).
extern "C" void uiSniffStart() { gSniffActive = true;  gPrefs.putBool("sniff", true); }
extern "C" void uiSniffStop()  { gSniffActive = false; gPrefs.putBool("sniff", false); }
extern "C" bool uiSniffActive() { return gSniffActive; }
extern "C" uint32_t uiSniffFrames() { return gSniffFrames; }
extern "C" int uiSniffLines(char out[10][56]) {  // newest-first; returns count
  uint32_t total = gSniffFrames; int cnt = total > 10 ? 10 : (int)total;
  int head = gSniffHead;  // next write slot -> newest is head-1
  for (int i = 0; i < cnt; ++i) strlcpy(out[i], gSniffRing[(head - 1 - i + 100) % 10].s, 56);
  return cnt;
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
  pubRetained(discoveryTopic("select", "hold_duration"), holdSelectDiscoveryJson());  // #81
  pubRetained(discoveryTopic("number", "fan_circulate_min"), fanCirculateMinDiscoveryJson());  // #128
  pubRetained(discoveryTopic("number", "fan_circulate_pct"), fanCirculatePctDiscoveryJson());  // #128
  pubRetained(discoveryTopic("switch", "em_heat"), emHeatDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "lock"), lockDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "outdoor_temp"), outdoorTempDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "outdoor_source"), outdoorSourceDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "fusion"), fusionDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "changeover_reason"), changeoverReasonDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "status_line"), statusLineDiscoveryJson());
  pubRetained(discoveryTopic("sensor", "tracking_line"), trackingLineDiscoveryJson());
#ifdef SLYTHERM_LOCAL_SENSOR
  pubRetained(discoveryTopic("number", std::string("sensor_") + kLocalSensorId + "_offset"),
              sensorOffsetDiscoveryJson(kLocalSensorId));
#endif
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
      hm::topic::kCmdMode, hm::topic::kCmdFanMode,
      hm::topic::kCmdFanCirculateMin, hm::topic::kCmdFanCirculatePct,  // #128
      hm::topic::kCmdPreset,
      hm::topic::kCmdHold, hm::topic::kCmdEmHeat, hm::topic::kCmdLockClear,
      hm::topic::kCmdNextTarget, hm::topic::kCmdEnergyPrices,
      hm::topic::kConfigSensors, hm::topic::kConfigPresets,
      cfg::kOatTopic, "slytherm/sensors/+/state", "slytherm/sensors/+/presence",
      "slytherm/cmd/sensor/+/offset", "slytherm/cmd/sensor/+/participating",
      "slytherm/cmd/sleep", "slytherm/cmd/sniff", "slytherm/cmd/ota_mirror"};
  for (const char* t : topics) gMqtt.subscribe(t);
#if defined(SLYTHERM_TXTURN_PROBE)
  gMqtt.subscribe("slytherm/cmd/txturn_stress");  // #28 bench stress toggle
#endif
#ifdef SLYTHERM_REMOTE_LINK
  gMqtt.subscribe(hm::topic::kRemoteIntentSubscribeWildcard);  // #104
#endif
#ifdef SLYTHERM_OTA
  gMqtt.subscribe(hm::topic::kCmdOtaCheck);  // #61
  gMqtt.subscribe(hm::topic::kCmdOtaApply);
#endif
}

// State publish cache: one slot per fixed topic (diff suppression).
enum PubIdx : uint8_t {
  PUB_TEMP, PUB_SP, PUB_LOW, PUB_HIGH, PUB_MODE, PUB_FAN,
  PUB_FANMIN, PUB_FANPCT,  // #128 retained circulate config
  PUB_PRESET, PUB_HOLD,
  PUB_ACTION, PUB_EQUIP, PUB_MOD, PUB_OAT, PUB_OATSRC, PUB_FUSION, PUB_CMOR,
  PUB_CLO, PUB_EMHEAT, PUB_CHG, PUB_LOCK, PUB_BUS, PUB_FAULT, PUB_HEALTH,
  PUB_LASTERR, PUB_SLEEP, PUB_STATUSLINE, PUB_TRACKLINE,
#ifdef SLYTHERM_ACTUATOR_RELAY
  PUB_RELAYS,
#endif
#ifdef SLYTHERM_OTA
  PUB_OTA,
#endif
  PUB_COUNT
};
char gPubCache[PUB_COUNT][192];

// retain=true for topics a reconnecting Remote/HA must read instantly (#128 fan
// state); still diff-suppressed so a steady state doesn't rewrite every tick.
void pubState(PubIdx i, const char* topic, const char* val, bool force, bool retain = false) {
  if (!force && strncmp(gPubCache[i], val, sizeof(gPubCache[i]) - 1) == 0) return;
  strlcpy(gPubCache[i], val, sizeof(gPubCache[i]));
  gMqtt.publish(topic, val, retain);
}

#ifdef SLYTHERM_REMOTE_LINK
// #104: retained authoritative echo (docs/11 "Authority"). Unlike pubState(),
// this MUST publish retained (a reconnecting Remote restores from it) — still
// diff-suppressed so a steady state doesn't hammer the broker with retained
// writes every 500 ms tick.
std::string gRemoteEchoCache;  // mqttTask-owned only; "" forces a republish

void publishRemoteEcho(const Snapshot& s, bool force) {
  const std::string j = hm::remoteStateJson(
      s.heatSp, s.coolSp, s.mode, s.emHeat, s.holdType, s.holdRemainS,
      s.preset[0] ? s.preset : "none", s.tempC, s.tempValid,
      s.action, s.equipment, s.alarmN, s.alarm1, s.alarm2,   // #116
      s.vacationActive, s.vacBanner);                        // #118
  if (!force && j == gRemoteEchoCache) return;
  gRemoteEchoCache = j;
  gMqtt.publish(hm::topic::kRemoteState, j.c_str(), true);
}
#endif  // SLYTHERM_REMOTE_LINK

// #62: furnace-busy clock for the OTA reboot gate. Updated every snapshot
// publish (<=500 ms cadence on the mqttTask); otaSafeToReboot() below demands
// a SUSTAINED idle window measured against this, so an OTA staged moments
// after a heat call ends still waits the full window.
volatile uint32_t gOtaLastBusyMs = 0;
constexpr uint32_t kOtaIdleWindowMs = 5u * 60u * 1000u;

extern "C" bool otaSafeToReboot() {
  xSemaphoreTake(gSnapMux, portMAX_DELAY);
  const bool idle = (strcmp(gSnap.action, "idle") == 0 ||
                     strcmp(gSnap.action, "off") == 0) &&
                    strcmp(gSnap.equipment, "idle") == 0;
  xSemaphoreGive(gSnapMux);
  if (!idle) { gOtaLastBusyMs = millis(); return false; }  // stamp even if MQTT is down
  return millis() - gOtaLastBusyMs >= kOtaIdleWindowMs;  // idle-since-boot counts
}

void publishSnapshot(bool force) {
  Snapshot s;
  xSemaphoreTake(gSnapMux, portMAX_DELAY);
  s = gSnap;
  gSnap.lockTombstone = false;  // consumed below
  xSemaphoreGive(gSnapMux);
  if ((strcmp(s.action, "idle") != 0 && strcmp(s.action, "off") != 0) ||
      strcmp(s.equipment, "idle") != 0)
    gOtaLastBusyMs = millis();

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
  // #128: fan mode + circulate config RETAINED — a reconnecting Remote/HA reads
  // the truth immediately (the panel Fan sheet is the standalone control path).
  pubState(PUB_FAN, topic::kStateFanMode, toString(s.fan), force, /*retain=*/true);
  snprintf(b, sizeof(b), "%lu", static_cast<unsigned long>(s.fanCircMin));
  pubState(PUB_FANMIN, topic::kStateFanCirculateMin, b, force, /*retain=*/true);
  snprintf(b, sizeof(b), "%.0f", static_cast<double>(s.fanCircPct));
  pubState(PUB_FANPCT, topic::kStateFanCirculatePct, b, force, /*retain=*/true);
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
  pubState(PUB_STATUSLINE, topic::kStateStatusLine, s.statusLine, force);
  pubState(PUB_TRACKLINE, topic::kStateTrackingLine, s.trackingLine, force);
  snprintf(b, sizeof(b), "%lu", static_cast<unsigned long>(s.compMinOffRemainS));
  pubState(PUB_CMOR, topic::kStateCompressorMinOffRemaining, b, force);
  pubState(PUB_CLO, topic::kStateCompressorLockedOut,
           s.compLockedOut ? payload::kOn : payload::kOff, force);
  pubState(PUB_EMHEAT, topic::kStateEmHeat, s.emHeat ? payload::kOn : payload::kOff, force);
  pubState(PUB_CHG, topic::kStateChangeoverReason, s.changeReason, force);
  pubState(PUB_LOCK, topic::kStateLock,
           lockStateJson(s.lockState, s.lockLevel, s.pinSet).c_str(), force);
  pubState(PUB_BUS, "slytherm/state/bus", s.busJson, force);
  pubState(PUB_FAULT, topic::kStateFault, "none", force);
  pubState(PUB_HEALTH, topic::kStateHealth,
           s.healthProblem ? payload::kOn : payload::kOff, force);
  pubState(PUB_LASTERR, topic::kStateLastError,
           s.lastError[0] ? s.lastError : "none", force);
  // #90: night Sleep state for HA visibility/automations.
  pubState(PUB_SLEEP, "slytherm/state/sleep", s.asleep ? "asleep" : "awake", force);
#ifdef SLYTHERM_ACTUATOR_RELAY
  pubState(PUB_RELAYS, "slytherm/state/relays", s.relaysJson, force);
#endif
#ifdef SLYTHERM_OTA
  { const ota::Status os = ota::status();  // #61 live client status
    pubState(PUB_OTA, topic::kStateOta,
             otaStateJson(ota::toString(os.state), os.progressPct,
                          SLYTHERM_FW_VERSION, os.available, os.error).c_str(),
             force); }
#endif

  if (force) {  // age/offset diagnostics on the heartbeat only (NON-retained:
                // age ticks constantly, retaining it would churn the broker)
    for (const auto& sp : s.sensors) {
      if (!sp.used) continue;
      std::string id(sp.name);
      snprintf(b, sizeof(b), "%lu", static_cast<unsigned long>(sp.ageS));
      gMqtt.publish(sensorAgeStateTopic(id).c_str(), b);
      snprintf(b, sizeof(b), "%.1f", static_cast<double>(sp.offsetC));
      gMqtt.publish(sensorOffsetStateTopic(id).c_str(), b);
    }
  }
  // Participating echo: RETAINED so a reconnecting Remote gets the true state
  // without waiting for a heartbeat, and emitted PROMPTLY on change (the dirty
  // flag) not just on the heartbeat. Read the AUTHORITATIVE gSensorTable choice
  // under gCmdMux — the snapshot's participating lags a control cycle behind a
  // just-applied toggle and would latch a stale retained value for up to a
  // heartbeat. Copy under the lock, publish outside it (no MQTT under gCmdMux).
  if (force || gParticipationEchoDirty) {
    gParticipationEchoDirty = false;
    struct { char id[kSensorNameLen]; bool part; } pe[kFusionSlots];
    size_t peN = 0;
    xSemaphoreTake(gCmdMux, portMAX_DELAY);
    for (size_t i = 0; i < kFusionSlots; ++i) {
      if (!gSensorTable[i].used) continue;
      strlcpy(pe[peN].id, gSensorTable[i].name, sizeof(pe[peN].id));
      pe[peN].part = gSensorTable[i].participating;
      ++peN;
    }
    xSemaphoreGive(gCmdMux);
    for (size_t k = 0; k < peN; ++k)
      pubRetained(sensorParticipatingStateTopic(std::string(pe[k].id)),
                  pe[k].part ? payload::kOn : payload::kOff);
  }
  if (s.lockTombstone) {
    // Retained empty message deletes any retained lock_clear copy (docs/06).
    gMqtt.publish(hm::topic::kCmdLockClear, "", true);
  }
#ifdef SLYTHERM_REMOTE_LINK
  publishRemoteEcho(s, force);
#endif
}

void mqttTask(void*) {
  const bool haveMqtt = strlen(THERMOSTAT_MQTT_HOST) > 0;
#ifdef SLYTHERM_UI
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
#ifdef SLYTHERM_UI
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
  // #WDT: bound PubSubClient's socket-wait loops well under the 5 s task WDT.
  // Both PubSubClient::connect() (CONNACK wait) and ::readByte() busy-spin on
  // `!_client->available()` for up to `socketTimeout` seconds — connect() with
  // NO yield at all, readByte() with yield()==vPortYield() which re-selects this
  // prio-2 task and never lets prio-0 IDLE0 run. The default 15 s therefore
  // starves IDLE0 on core 0 and trips the watchdog (reason:"wdt") on a slow/
  // partial broker exchange during the connect->discovery burst. 2 s caps the
  // spin at < 5 s (3 s margin); a healthy LAN broker answers in << 1 s, so this
  // never causes a spurious reconnect. Diagnostic bound only — control/protocol
  // behaviour is unchanged.
  gMqtt.setSocketTimeout(2);
  gMqtt.setCallback(onMqttMessage);

  uint32_t lastWifiTryMs = 0, lastMqttTryMs = 0, lastHeartbeatMs = 0, lastDiscoverMs = 0;
  for (;;) {
    const uint32_t nowMs = millis();
#ifdef SLYTHERM_UI
    wifi_prov::service(nowMs);
    telnet_log::poll();
    coredump_server::poll();          // #124: LAN coredump pull (:8082)
    boot_guard::healthyTick(millis());  // #122/#123: clears the counter after sustained uptime
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
      if (n > 0) { ip = MDNS.address(0); pt = MDNS.port(0); }  // core 3.3.x: IP() -> address()
      else { n = MDNS.queryService("home-assistant", "tcp"); if (n > 0) { ip = MDNS.address(0); pt = 1883; } }  // 2: HA host
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
    static bool sNtpUp = false;  // one-shot NTP once WiFi is up (Eastern default, #69)
    if (gWifiConnected && !sNtpUp) { sNtpUp = true; configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov"); }
    if (gWifiConnected && haveMqttNow && !gMqtt.connected() &&
        nowMs - lastMqttTryMs >= cfg::kMqttReconnectMs) {
      lastMqttTryMs = nowMs;
      // LWT: availability topic -> retained "offline" (docs/06).
#ifdef SLYTHERM_UI
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
#ifdef SLYTHERM_REMOTE_LINK
        // #104: retained identity heartbeat. Liveness for a Remote is
        // kAvailability (above) — this topic carries cid/version only (see
        // the kControllerStatus comment in HaMqtt.h for why).
        gMqtt.publish(hm::topic::kControllerStatus,
                      hm::controllerStatusJson(controllerCid(), true,
                                               SLYTHERM_FW_VERSION).c_str(),
                      true);
#endif
        publishDiscovery();
        gDiscoveryDirty = false;
        subscribeAll();
        memset(gPubCache, 0, sizeof(gPubCache));  // full republish after reconnect
#ifdef SLYTHERM_REMOTE_LINK
        gRemoteEchoCache.clear();  // force a full retained-echo republish too
#endif
        // #123/#145: retained boot/crash telemetry. republish=false marks the
        // boot announce; every later reconnect echo of this retained topic
        // carries republish=true (uptimeS also climbs across echoes).
        gMqtt.publish("slytherm/boot",
                      boot_guard::statusJson(millis() / 1000u).c_str(), true);
        // #61: broker connectivity = the post-OTA self-test (network stack +
        // app alive). Confirms a pending update; no-op otherwise.
        ota::noteSelfTestPass();
#ifdef SLYTHERM_UI
        telnet_log::logf("[mqtt] connected to %s:%u", sMqttHost, sMqttPort);
#else
        Serial.println("[mqtt] connected");
#endif
      }
#ifdef SLYTHERM_UI
      else telnet_log::logf("[mqtt] connect failed (state=%d)", gMqtt.state());
#endif
    }
    gMqttConnected = gMqtt.connected();
#ifdef SLYTHERM_UI
    mqtt_cfg::setConnected(gMqttConnected);
#endif
    if (gMqttConnected) {
      gMqtt.loop();
      if (gDiscoveryDirty) { gDiscoveryDirty = false; publishDiscovery(); }
      if (gCopProxyPubReq) {  // #143: control task staged COP-proxy telemetry (retained)
        xSemaphoreTake(gCmdMux, portMAX_DELAY); std::string j = gCopProxyJson; gCopProxyPubReq = false; xSemaphoreGive(gCmdMux);
        pubRetained(hm::topic::kStateCopProxy, j); }
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
volatile uint32_t gCtRxBytes = 0;    // raw UART RX bytes (stats: dead-pin vs framing)

// #28 passive TX-turnaround jitter probe. gTxTurnStress is defined earlier (the
// MQTT handler needs it); the UI reads it through this extern accessor. The
// histogram and probe path exist only when SLYTHERM_TXTURN_PROBE is built.
extern "C" bool uiTxTurnStress() { return gTxTurnStress; }
#if defined(SLYTHERM_TXTURN_PROBE)
slytherm::LatencyStats gTxTurn;       // compute-turnaround samples (us), ct485Task-owned
#endif

#ifdef SLYTHERM_CT485_UART
ct485::FrameAccumulator gCtAcc;
uint32_t gCtLastByteUs = 0;
bool gCtInProgress = false;
#endif

#ifdef SLYTHERM_CT485_UART
void sniffFrame(const ct485::Frame& f) {
  if (!gSniffActive) return;
  gSniffFrames = gSniffFrames + 1;
  const unsigned long ms = millis();
  char line[96];
  int n = snprintf(line, sizeof(line), "%lu %02X>%02X t%02X l%u",
                   ms, f.src, f.dst, f.msgType, f.payloadLen);
  int nb = f.payloadLen; if (nb > 16) nb = 16;
  for (int i = 0; i < nb && n < (int)sizeof(line) - 3; ++i)
    n += snprintf(line + n, sizeof(line) - n, " %02X", f.payload[i]);
  telnet_log::logf("[ct485] %s", line);
  strlcpy(gSniffRing[gSniffHead].s, line, sizeof(gSniffRing[0].s));
  gSniffHead = (gSniffHead + 1) % 10;
  // Payload bytes past the 16-byte preview go out as continuation lines
  // ("[ct485+] <same millis> <chunk#> <hex...>", 24 B/chunk under the
  // 128-char telnet line cap) so status blocks / config TLVs reach the
  // PC-side decoder complete. The UI ring keeps only the preview line.
  for (int off = 16, idx = 0; off < (int)f.payloadLen; off += 24, ++idx) {
    n = snprintf(line, sizeof(line), "%lu %d", ms, idx);
    for (int i = off; i < (int)f.payloadLen && i < off + 24; ++i)
      n += snprintf(line + n, sizeof(line) - n, " %02X", f.payload[i]);
    telnet_log::logf("[ct485+] %s", line);
  }
}

#if defined(SLYTHERM_CT485_TX_ENABLE)
// Go-live: mirror the frames WE transmit into the same capture stream, so
// slylog logs what SlyTherm sends and not only what it hears — the RX echo of
// our own TX is transceiver-dependent (auto-direction chips may mute RX while
// driving), so we cannot rely on it. Tagged [ct485-tx] / [ct485+tx] to stay
// distinguishable from received traffic; identical hex layout to sniffFrame so
// the PC-side decoder ingests it the same way. Gated on gSniffActive like RX.
void mirrorTxFrame(const ct485::Frame& f) {
  if (!gSniffActive) return;
  const unsigned long ms = millis();
  char line[128];
  // Full header (sn=subnet sm=sendMethod sp=sendParamHi nt=nodeType pk=packetNum)
  // so a rejected demand can be diffed byte-for-byte against docs/02 §5a.
  int n = snprintf(line, sizeof(line), "%lu %02X>%02X t%02X l%u sn%02X sm%02X sp%02X nt%02X pk%02X",
                   ms, f.src, f.dst, f.msgType, f.payloadLen,
                   f.subnet, f.sendMethod, f.sendParamHi, f.srcNodeType, f.packetNum);
  int nb = f.payloadLen; if (nb > 16) nb = 16;
  for (int i = 0; i < nb && n < (int)sizeof(line) - 3; ++i)
    n += snprintf(line + n, sizeof(line) - n, " %02X", f.payload[i]);
  telnet_log::logf("[ct485-tx] %s", line);
  for (int off = 16, idx = 0; off < (int)f.payloadLen; off += 24, ++idx) {
    n = snprintf(line, sizeof(line), "%lu %d", ms, idx);
    for (int i = off; i < (int)f.payloadLen && i < off + 24; ++i)
      n += snprintf(line + n, sizeof(line) - n, " %02X", f.payload[i]);
    telnet_log::logf("[ct485+tx] %s", line);
  }
}
#endif

// Mirror salvaged torn/merged bursts (FrameAccumulator::takeRejected) as
// chunked hex — the PC decoder's resync slider recovers the frames inside.
// Drained even while sniffing is off so a stale stash never leaks into a
// later session's timeline.
void sniffRejects() {
  static uint8_t rej[ct485::kMaxFrame];  // ct485Task-only
  size_t n;
  while ((n = gCtAcc.takeRejected(rej)) != 0) {
    if (!gSniffActive) continue;
    const unsigned long ms = millis();
    char line[96];
    for (size_t off = 0, idx = 0; off < n; off += 24, ++idx) {
      int p = snprintf(line, sizeof(line), "%lu %u", ms, (unsigned)idx);
      for (size_t i = off; i < n && i < off + 24; ++i)
        p += snprintf(line + p, sizeof(line) - p, " %02X", rej[i]);
      telnet_log::logf("[ct485-rej] %s", line);
    }
  }
}
#endif

#if defined(SLYTHERM_TXTURN_PROBE)
// #28 passive TX-turnaround probe. On every coordinator grant addressed to the
// slot SlyTherm occupies (OEM thermostat = node 1 = kAddrThermostat) — an R2R
// poll (dataflow bit) or a Token Offer — time the firmware's COMPUTE turnaround:
// from the just-decoded grant (t0) to the would-be node-1 reply built + encoded
// and ready for DE to assert (t1). The reply is the pure/const dry-run
// (dryRunGrantResponse) so the shadow-mode stack, which never resumes, still
// times the real TX-path work WITHOUT touching the bus or any state. Called from
// the RX loop right after decode; adds one histogram sample (~every 3.2 s).
void txTurnProbe(const ct485::Frame& f) {
  const bool r2r = f.msgType == static_cast<uint8_t>(ct485::MsgType::kR2R) &&
                   (f.packetNum & ct485::kPktNumDataflowBit);
  const bool tok = f.msgType == static_cast<uint8_t>(ct485::MsgType::kTokenOffer);
  if (!(r2r || tok) || f.dst != ct485::kAddrThermostat) return;
  static uint8_t raw[ct485::kMaxFrame];  // ct485Task-only scratch (no heap in RX path)
  ct485::Frame resp;
  const uint32_t t0 = micros();
  if (gCt->dryRunGrantResponse(f.src, tok, resp))
    (void)ct485::encode(resp, raw);     // encode() dominates: the wire bytes DE would drive
  const uint32_t t1 = micros();
  gTxTurn.add(t1 - t0);                  // uint32 wrap-safe for a small delta
}
#endif

void ct485Task(void*) {
#ifdef SLYTHERM_CT485_UART
  if (cfg::kCt485DePin >= 0) {           // auto-direction transceivers have no DE (#71)
    pinMode(cfg::kCt485DePin, OUTPUT);
    digitalWrite(cfg::kCt485DePin, LOW);  // receive; hardware pulldown agrees
  }
  // Core 3.x (pioarduino 55.03.39) regression war (v0.5.3..v0.5.5): the IDF
  // console owns UART0 whose default pins ARE this board's RS-485 pair
  // (U0TXD=43 / U0RXD=44). Serial0.end() (v0.5.4) and a forced
  // uart_set_pin on UART2 (v0.5.5) both left rx=0 even with PeriMan showing
  // the pins as UART_RX/UART_TX. So: bypass Arduino HardwareSerial entirely —
  // raw IDF driver on a fresh instance (UART1), explicit pin mux, no core
  // pin-management in the path.
  Serial0.end();
  {
    // v0.5.6 (raw UART1) still read rx=0 with lvl=1: pins ESP_OK, pad idling
    // high, zero edges seen. Suspect the ROM/console's IOMUX claim on
    // GPIO43 (=U0TXD, an OUTPUT driving idle-high) survives uart_set_pin and
    // fights the transceiver. Reset both pins to plain-GPIO function FIRST,
    // then attach UART1; afterwards dump the real IOMUX/matrix state to the
    // telnet ring so the hardware answers instead of theories.
    gpio_reset_pin(static_cast<gpio_num_t>(cfg::kCt485RxPin));
    gpio_reset_pin(static_cast<gpio_num_t>(cfg::kCt485TxPin));
    uart_config_t uc = {};
    uc.baud_rate = static_cast<int>(ct485::kBaudDefault);
    uc.data_bits = UART_DATA_8_BITS;
    uc.parity    = UART_PARITY_DISABLE;
    uc.stop_bits = UART_STOP_BITS_1;
    uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uc.source_clk = UART_SCLK_DEFAULT;
    esp_err_t e1 = uart_driver_install(UART_NUM_1, 4096, 0, 0, nullptr, 0);
    esp_err_t e2 = uart_param_config(UART_NUM_1, &uc);
    esp_err_t e3 = uart_set_pin(UART_NUM_1, cfg::kCt485TxPin, cfg::kCt485RxPin,
                                UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    telnet_log::logf("[ct485] uart1 install=%d cfg=%d pins=%d rxBus=%d txBus=%d",
                     (int)e1, (int)e2, (int)e3,
                     (int)perimanGetPinBusType(cfg::kCt485RxPin),
                     (int)perimanGetPinBusType(cfg::kCt485TxPin));
    // X-ray the pins: gpio_dump_io_configuration renders IOMUX function,
    // input-enable, output-enable and matrix signal routing per pin.
    char dump[768] = {0};
    FILE* ms = fmemopen(dump, sizeof(dump) - 1, "w");
    if (ms) {
      gpio_dump_io_configuration(ms, (1ULL << cfg::kCt485RxPin) |
                                     (1ULL << cfg::kCt485TxPin));
      fclose(ms);
      char* save = nullptr;
      for (char* ln = strtok_r(dump, "\n", &save); ln;
           ln = strtok_r(nullptr, "\n", &save))
        if (*ln) telnet_log::logf("[ct485-io] %s", ln);
    }
  }
  gCtLastByteUs = micros();
#endif
  for (;;) {
    const uint32_t nowMs = millis();
    xSemaphoreTake(gCtMux, portMAX_DELAY);
#ifdef SLYTHERM_CT485_UART
    size_t availSz = 0;
    uart_get_buffered_data_len(UART_NUM_1, &availSz);
    int avail = static_cast<int>(availSz);
    while (avail-- > 0) {
      uint8_t rb;
      if (uart_read_bytes(UART_NUM_1, &rb, 1, 0) != 1) break;
      const int c = rb;
      gCtRxBytes = gCtRxBytes + 1;
      const uint32_t nowUs = micros();
      const bool gapBefore =
          !gCtInProgress || (nowUs - gCtLastByteUs) >= ct485::kInterFrameGapUs;
      gCtLastByteUs = nowUs;
      gCtInProgress = true;
      if (gCtAcc.feed(static_cast<uint8_t>(c), gapBefore)) {
        ct485::Frame f;
        if (ct485::decode(gCtAcc.frame(), gCtAcc.frameLen(), f)) {
          gCt->onFrame(f, nowMs);
#if defined(SLYTHERM_TXTURN_PROBE)
          txTurnProbe(f);  // #28: time our would-be grant response (no TX)
#endif
          gLastBusRxS = nowSeconds();
          sniffFrame(f);
        }
      }
      sniffRejects();  // drain per byte: the salvage stash is single-slot
    }
    if (gCtInProgress &&
        static_cast<uint32_t>(micros() - gCtLastByteUs) >= ct485::kInterFrameGapUs) {
      gCtInProgress = false;
      if (gCtAcc.flush()) {
        ct485::Frame f;
        if (ct485::decode(gCtAcc.frame(), gCtAcc.frameLen(), f)) {
          gCt->onFrame(f, nowMs);
#if defined(SLYTHERM_TXTURN_PROBE)
          txTurnProbe(f);  // #28: time our would-be grant response (no TX)
#endif
          gLastBusRxS = nowSeconds();
          sniffFrame(f);
        }
      }
      sniffRejects();
    }
    // Capture health: periodic accumulator counters while LISTEN is active.
    // rx = raw UART bytes read — rx frozen means the UART/pins are dead (the
    // v0.5.3 regression), rx growing with ok frozen means framing trouble.
    { static uint32_t statsMs = 0;
      if (gSniffActive && nowMs - statsMs >= 30000) { statsMs = nowMs;
        const auto& c = gCtAcc.counters();
        // lvl = raw pad level of the RX pin: RS-485 idle (mark) should read 1.
        // Stuck 0 with rx frozen = dead input path / transceiver, not framing.
        telnet_log::logf("[ct485-stats] %lu rx=%lu lvl=%d ok=%lu badLen=%lu badCk=%lu over=%lu",
                         (unsigned long)nowMs, (unsigned long)gCtRxBytes,
                         (int)gpio_get_level(static_cast<gpio_num_t>(cfg::kCt485RxPin)),
                         (unsigned long)c.framesOk,
                         (unsigned long)c.badLength, (unsigned long)c.badChecksum,
                         (unsigned long)c.overruns); } }
#if defined(SLYTHERM_TXTURN_PROBE)
    // Per-mode isolation: clear the histogram when the bench stress toggle flips
    // so the passive baseline and the stress run never co-mingle (a diluted
    // stress p99 would mask exactly the spike stress mode exists to catch).
    // Race-free: gTxTurn.add() and this reset both run inside the gCtMux hold.
    { static bool lastStress = false;
      if (gTxTurnStress != lastStress) { lastStress = gTxTurnStress; gTxTurn.reset(); } }
#endif
#if defined(SLYTHERM_TXTURN_PROBE) && defined(SLYTHERM_UI)
    // #28 verdict feed. Compute turnaround (grant decoded -> reply built+encoded),
    // plus the WIRE-TO-WIRE max the coordinator would see: compute max + the
    // task-tick dwell (grant bytes wait <= one cadence in the UART FIFO) + the DE
    // pre-drive hold. Compare w2wmax against the OEM node-1 floor (~123000 us).
    { static uint32_t txtMs = 0;
      if (gTxTurn.count() > 0 && nowMs - txtMs >= dettson::kTxTurnReportMs) { txtMs = nowMs;
        const uint32_t w2wMax = gTxTurn.max() +
                                cfg::kCt485TickMs * 1000u + ct485::kDePrePostUs;
        telnet_log::logf("[txturn] n=%lu min=%lu p50=%lu p95=%lu p99=%lu p999=%lu max=%lu us "
                         "w2wmax=%lu us(dwell%lums+de) floor=%lu stress=%d",
                         (unsigned long)gTxTurn.count(), (unsigned long)gTxTurn.min(),
                         (unsigned long)gTxTurn.percentile(0.50f),
                         (unsigned long)gTxTurn.percentile(0.95f),
                         (unsigned long)gTxTurn.percentile(0.99f),
                         (unsigned long)gTxTurn.percentile(0.999f),
                         (unsigned long)gTxTurn.max(), (unsigned long)w2wMax,
                         (unsigned long)cfg::kCt485TickMs,
                         (unsigned long)dettson::kTxTurnOemFloorUs,
                         (int)gTxTurnStress); } }
#endif
#endif
    gCt->tick(nowMs);
    ct485::Frame txf;
    while (gCt->popTx(txf)) {
#if defined(SLYTHERM_CT485_UART) && defined(SLYTHERM_CT485_TX_ENABLE)
      uint8_t raw[ct485::kMaxFrame];
      const size_t n = ct485::encode(txf, raw);
      if (n > 0) {
        // DE only exists on a manual-direction transceiver; the Waveshare S3
        // board is auto-direction (kCt485DePin = -1) so the guard skips the
        // no-op pin writes and their settle delays entirely.
        const bool haveDe = cfg::kCt485DePin >= 0;
        if (haveDe) { digitalWrite(cfg::kCt485DePin, HIGH); delayMicroseconds(ct485::kDePrePostUs); }
        uart_write_bytes(UART_NUM_1, raw, n);
        uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(250));
        if (haveDe) { delayMicroseconds(ct485::kDePrePostUs); digitalWrite(cfg::kCt485DePin, LOW); }
        mirrorTxFrame(txf);  // log our own TX to the capture stream (slylog)
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
// #128: fan "circulate" tunables, promoted from the cfg:: compile constants to
// runtime config. The constants remain the DEFAULTS (restored from NVS in
// setup, overridden by the on-panel Fan sheet or the MQTT cmd topics). Read by
// the circulate driver each control cycle; benign scalar races with the UI-task
// getters, same discipline as gClock24.
uint32_t gFanCircMin = cfg::kFanCirculateMinPerHour;
float    gFanCircPct = cfg::kFanCirculatePct;

// Fan settings (#128). Controller role: apply LOCALLY like clock/lock — set the
// control-task globals directly (a benign scalar write; the control task reads
// them next cycle). Persistence rides persistOnChange's shadow compare and the
// retained state topics ride the snapshot publish — NOT the gPending MQTT-inbound
// mailbox (which would spuriously reset the loss-of-MQTT staleness clock). The
// getters seed the on-panel Fan sheet.
extern "C" uint8_t uiFanMode() { return static_cast<uint8_t>(gFanMode); }
extern "C" void uiSetFanMode(uint8_t m) {
  if (m > static_cast<uint8_t>(hm::FanMode::kCirculate)) return;
  gFanMode = static_cast<hm::FanMode>(m);
}
extern "C" uint32_t uiFanCircMin() { return gFanCircMin; }
extern "C" uint8_t uiFanCircPct() { return static_cast<uint8_t>(gFanCircPct + 0.5f); }
extern "C" void uiSetFanCirculate(uint32_t minPerHour, uint8_t pct) {
  gFanCircMin = fan::clampCirculateMinPerHour(static_cast<long>(minPerHour));
  gFanCircPct = fan::snapCirculatePct(static_cast<float>(pct));
}

bool gSetpointsValidated = false;
bool gConfigOk = true;
uint32_t gLastInboundS = 0;       // last accepted MQTT traffic (stale clock)
bool gFallbackApplied = false;
bool gWdtPetLevel = false;
float gLastHpEmittedPct = 0.0f;

#ifdef SLYTHERM_DS18B20
void pollLocalSensors(uint32_t nowS);
#endif

// Recovery (advisory) state.
bool gHaveTarget = false;
hm::NextTarget gTarget;
uint32_t gTargetRxS = 0;
bool gTargetApplied = false;
// #141 two-ramp fallback verdict (advisory; refreshed every adviseRecovery,
// consumed by the next cycle's heat-source pick). Dead until BOTH
// kRecoveryEnabledDefault and kRecoveryTwoRampEnabledDefault are on —
// heating validation is a WINTER task.
bool gRecoveryGasAdvised = false;

// Run-segment tracking for RecoveryEstimator learning.
enum class Serving : uint8_t { kNone, kGas, kHpHeat, kCool };
Serving gServing = Serving::kNone;

// Persist-on-change shadows.
struct PersistShadow {
  uint8_t mode = 0xFF, priorMode = 0xFF, hold = 0xFF, fan = 0xFF;
  uint32_t holdEnd = 0;  // #151: timed-hold end anchor (0 for untimed holds)
  float heatSp = NAN, coolSp = NAN;
  bool guardRunning = false;
  uint32_t fanCircMin = 0xFFFFFFFFu;  // #128 circulate minutes (sentinel forces first write)
  float fanCircPct = NAN;             // #128 circulate speed %
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
               uint32_t nowS, bool autoClear = false) {
  if (present) gSup->alarms().raise(code, sev, text, nowS, autoClear);
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
      // Fuse iff it's a known roster member AND the user has it participating.
      gFusion.setParticipating(static_cast<uint8_t>(i),
                               dettson::fusionParticipates(table[i].inRoster, table[i].participating));
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
    // #88 robustness: a live state 'occ' also feeds the presence ledger, so normal
    // sensor activity keeps a room Present even if the retained .../presence topic
    // lags or isn't published. occ:true -> "seen now" (advances last_seen);
    // occ:false -> reporting-but-vacant (ledger is monotonic, so no time bump).
    if (s.occ >= 0) gFusion.updatePresence(s.idx, s.occ != 0, nowS, s.occ != 0, nowS);
  }
  for (size_t i = 0; i < p.presenceCount; ++i) {  // #88: HA last_seen presence ledger
    const auto& pr = p.presence[i];
    gFusion.updatePresence(pr.idx, pr.occupied, pr.lastSeenS, pr.hasSeen, nowS);
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
    // #91: HA setpoint writes apply WITHOUT creating a device hold (1-arg setter).
    // A hold is an on-device override only (the UI intent path below); HA owns
    // scheduling + holds, so a scheduler/automation write must not fabricate one.
    if (gModeSm->mode() == UserMode::kHeat || gModeSm->mode() == UserMode::kEmergencyHeat)
      gModeSm->setHeatSetpoint(p.setpointC);
    else if (gModeSm->mode() == UserMode::kCool)
      gModeSm->setCoolSetpoint(p.setpointC);
  }
  // A REMOTE-panel setpoint change is a manual user action, so it arms the same
  // 4h hold as the wall panel (2-arg setter -> onManualChange -> startHold). An
  // HA cmd/heat|cool write stays no-hold (#91): HA owns the schedule + holds.
  if (p.hasLow) {
    if (p.manualSetpoint) gModeSm->setHeatSetpoint(p.lowC, nowS);
    else                  gModeSm->setHeatSetpoint(p.lowC);
  }
  if (p.hasHigh) {
    if (p.manualSetpoint) gModeSm->setCoolSetpoint(p.highC, nowS);
    else                  gModeSm->setCoolSetpoint(p.highC);
  }
  if (p.hasPreset) gModeSm->applyPreset(p.preset, nowS);
  // A remote setpoint/mode/preset change is a user-directed request: arm the
  // manual compressor-start bypass so the resulting demand isn't held by the
  // guard min-OFF (ODU restart delay is the backstop; see GuardGate). NB per
  // #91 an HA *scheduler* setpoint write is indistinguishable here from a human
  // tapping HA, so it also arms — safe: max-starts/hour still caps oscillation.
  if (p.hasMode || p.hasEmHeat || p.hasSetpoint || p.hasLow || p.hasHigh || p.hasPreset) {
    gGuardGate.armManual(nowS);
    gCoolShaper.armManual(nowS);  // #151: also relax the cool shaper's demand-level min-OFF
  }
  if (p.hasSleepOverride) gSleep.setOverride(p.sleepOverride);  // #90 HA hook
  if (p.hasHold) {
    if (p.hold.clear) gModeSm->clearHold();
    else gModeSm->startHold(p.hold.type, nowS);
  }
  if (p.hasVacation) {  // #118: mirror of ui::IntentType::kSetVacation
    gVac = VacationState{};
    gVac.on = true;
    gVac.startDays = p.vacStartDays;
    gVac.nights = p.vacNights < 1 ? 1 : p.vacNights;
    gVac.heatC = p.vacHeatC;
    gVac.coolC = p.vacCoolC;
    if (clockIsSynced()) anchorVacation();
    saveVacation();
    Serial.printf("[vac] remote request start+%ud len %un heat %.1f cool %.1f\n",
                  (unsigned)gVac.startDays, (unsigned)gVac.nights,
                  (double)gVac.heatC, (double)gVac.coolC);
  }
  if (p.hasClearVacation) {  // #118: mirror of kClearVacation
    if (gVac.applied && gModeSm) {
      gModeSm->setHeatSetpoint(gVac.priorHeatC);
      gModeSm->setCoolSetpoint(gVac.priorCoolC);
    }
    gVac = VacationState{};
    saveVacation();
    Serial.println("[vac] remote cancel -> resume normal operation");
  }
  if (p.hasAckAlarms) gSup->alarms().ackAll();  // #118
  if (p.hasFan) gFanMode = p.fan;
  if (p.hasFanCircMin) gFanCircMin = p.fanCircMin;  // #128 (already clamped on ingest)
  if (p.hasFanCircPct) gFanCircPct = p.fanCircPct;  // #128 (already snapped on ingest)
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
  if (p.hasEnergyPrices) {  // #143: parse already gated (0, kEnergyPriceMax]
    DualFuelConfig dc = gDualFuel->config();
    dc.elecPricePerKwh = p.energyPrices.elecPerKwh;
    dc.gasPricePerM3 = p.energyPrices.gasPerM3;
    if (gDualFuel->setConfig(dc)) {  // full re-validation, docs/04 §4
      gPrefs.putFloat("epk", dc.elecPricePerKwh);  // survive reboots (NVS)
      gPrefs.putFloat("gpm", dc.gasPricePerM3);
      Serial.printf("[econ] prices elec=%.3f$/kWh gas=%.3f$/m3 -> COP*=%.2f "
                    "balance=%.1fC (economic %s)\n",
                    (double)dc.elecPricePerKwh, (double)dc.gasPricePerM3,
                    (double)gDualFuel->breakEvenCop(),
                    (double)gDualFuel->effectiveBalancePointC(),
                    dc.economicEnabled ? "ON" : "OFF");
    }
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
        gGuardGate.armManual(nowS);   // on-panel setpoint change: immediate demand (see GuardGate)
        gCoolShaper.armManual(nowS);  // #151: relax the cool shaper's demand-level min-OFF too
        break;
      case ui::IntentType::kSetMode:
        gModeSm->setMode(static_cast<UserMode>(intent.mode), nowS);
        gGuardGate.armManual(nowS);   // on-panel mode change: immediate demand
        gCoolShaper.armManual(nowS);  // #151
        break;
      case ui::IntentType::kSetPreset: {
        // #74: preset is carried as a roster INDEX (UI cards map 1:1 to the live
        // roster), resolved to a name against the authoritative roster.
        const PresetDef* d = gModeSm->presetAt(static_cast<uint8_t>(intent.preset));
        // #91/preset-fix: an on-device preset TAP is an explicit user command, so it
        // overrides any active hold (incl. the auto 4h manual hold) instead of being
        // silently swallowed. applyPreset() keeps its hold-respecting semantics for
        // HA/programmatic callers; we clear the hold here first, at the UI layer only.
        if (d) { gModeSm->clearHold(); gModeSm->applyPreset(d->name, nowS); gGuardGate.armManual(nowS); gCoolShaper.armManual(nowS); }  // #151
        break;
      }
      case ui::IntentType::kAckAlarms:
        gSup->alarms().ackAll();
        break;
      case ui::IntentType::kSetHold:  // hold-duration chooser (#81)
        gModeSm->startHold(intent.hold, nowS);
        break;
      case ui::IntentType::kClearHold:  // "Resume schedule"
        gModeSm->clearHold();
        break;
      case ui::IntentType::kSetVacation: {  // #78: set the window + eco setpoints
        gVac = VacationState{};
        gVac.on = true;
        gVac.startDays = intent.vacStartDays;
        gVac.nights = intent.vacNights < 1 ? 1 : intent.vacNights;
        gVac.heatC = intent.heatC;
        gVac.coolC = intent.coolC;
        if (clockIsSynced()) anchorVacation();   // else evaluateVacation anchors when NTP arrives
        saveVacation();
        Serial.printf("[vac] request start+%ud len %un heat %.1f cool %.1f\n",
                      (unsigned)gVac.startDays, (unsigned)gVac.nights,
                      (double)gVac.heatC, (double)gVac.coolC);
        break;
      }
      case ui::IntentType::kClearVacation: {  // #78: cancel/resume
        if (gVac.applied && gModeSm) {
          gModeSm->setHeatSetpoint(gVac.priorHeatC);
          gModeSm->setCoolSetpoint(gVac.priorCoolC);
        }
        gVac = VacationState{};
        saveVacation();
        Serial.println("[vac] cancelled -> resume normal operation");
        break;
      }
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
  // #151: hold blob — type + remaining + save-time stamps so a reboot/OTA
  // restores the hold (setup) and NTP later charges it for the outage. The
  // end anchor (not remaining) is the change-detect so a ticking countdown
  // doesn't rewrite NVS every cycle.
  const uint8_t hold = static_cast<uint8_t>(gModeSm->activeHoldType());
  const uint32_t holdEnd = gModeSm->holdEndS();
  if (hold != gShadow.hold || holdEnd != gShadow.holdEnd) {
    ModeStateMachine::HoldPersistBlob hb;
    gModeSm->saveHold(&hb, nowS,
                      clockIsSynced() ? static_cast<uint32_t>(time(nullptr)) : 0);
    gPrefs.putBytes("holdb", &hb, sizeof(hb));
    gShadow.hold = hold;
    gShadow.holdEnd = holdEnd;
  }
  const uint8_t fan = static_cast<uint8_t>(gFanMode);
  if (fan != gShadow.fan) { gPrefs.putUChar("fan", fan); gShadow.fan = fan; }
  // #128: circulate minutes + speed — same change-detect discipline (whether the
  // change came from the panel Fan sheet or the MQTT cmd topics).
  if (gFanCircMin != gShadow.fanCircMin) { gPrefs.putUInt("fcmin", gFanCircMin); gShadow.fanCircMin = gFanCircMin; }
  if (gFanCircPct != gShadow.fanCircPct) { gPrefs.putFloat("fcpct", gFanCircPct); gShadow.fanCircPct = gFanCircPct; }

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
  // Screen-lock (PIN) blob: persist when it changes — a PIN set/lock/unlock from
  // the wall UI updates gUi directly, so detect the change here (survives reboot).
  {
    ui::UiModel::LockPersistBlob lb;
    uiLock(); gUi.saveLock(&lb); uiUnlock();
    if (memcmp(&lb, &gShadowLock, sizeof(lb)) != 0) {
      gPrefs.putBytes("lock", &lb, sizeof(lb));
      gShadowLock = lb;
    }
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
  const bool wasGasAdvised = gRecoveryGasAdvised;  // log on the rising edge only
  gRecoveryGasAdvised = false;  // re-derived below; clears when the target lapses
  if (!gHaveTarget || !fused.valid) return;
  const uint32_t elapsed = nowS - gTargetRxS;
  if (elapsed >= gTarget.inS) { gHaveTarget = false; return; }
  RecoveryTarget t;
  t.setpointC = gTarget.tempC;
  t.mode = gTarget.mode == hm::Mode::kCool ? RecoveryMode::kCool : RecoveryMode::kHeat;
  t.inS = gTarget.inS - elapsed;
  const RecoveryEquipment equip =
      (t.mode == RecoveryMode::kHeat && oat.valid &&
       oat.valueC <= gDualFuel->effectiveBalancePointC())  // follows #143 economics
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
  // #141 two-ramp fallback (docs/13 §2, Honeywell two-ramp scheme) — WINTER
  // validation task, doubly gated OFF by default (adviseRamps() itself
  // requires enabled AND twoRampEnabled). While an early-started heat
  // recovery is pending, gas is advised only when the measured temperature
  // falls below the fallback ramp — the line at the derated gas rate that
  // still makes the target on time. Advisory: the verdict feeds next cycle's
  // heat-source pick; GasShaper/arbiter/guard gate everything downstream.
  if (gTargetApplied && t.mode == RecoveryMode::kHeat) {
    const RecoveryRamps ramps = gRecovery.adviseRamps(t, fused.value);
    if (ramps.fallbackValid && ramps.gasAdvised) {
      gRecoveryGasAdvised = true;
      if (!wasGasAdvised) {
        Serial.printf("[recovery] below fallback ramp (%.2fC < %.2fC): gas advised\n",
                      static_cast<double>(fused.value),
                      static_cast<double>(ramps.fallbackTempNowC));
      }
    }
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
  s.fanCircMin = gFanCircMin;  // #128
  s.fanCircPct = gFanCircPct;
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
  s.asleep = gSleep.asleep();  // #90: slytherm/state/sleep

  // Shadow control (#139): mirror what OUR pipeline would command to the
  // telnet stream so it can be compared 1:1 against the OEM stat's captured
  // CT-485 demands. On-change plus a 60 s heartbeat; values are the same
  // DemandSet the actuator layer would consume if TX were authorized.
  // telnet_log is a UI-build facility (headless env omits it — v0.5.0 lesson).
#ifdef SLYTHERM_UI
  { static float lg = -1, lh = -1, lc = -1, lf = -1, ld = -1;
    static uint32_t beatS = 0;
    const bool chg = out.gasHeatPct != lg || out.hpHeatPct != lh ||
                     out.coolPct != lc || out.fanPct != lf ||
                     out.defrostTemperPct != ld;
    if (chg || nowS - beatS >= 60) {
      beatS = nowS; lg = out.gasHeatPct; lh = out.hpHeatPct;
      lc = out.coolPct; lf = out.fanPct; ld = out.defrostTemperPct;
      telnet_log::logf("[shadow] %lu gas=%.0f hp=%.0f cool=%.0f fan=%.0f dfr=%.0f "
                       "T=%.2f setH=%.1f setC=%.1f mode=%u action=%s",
                       (unsigned long)millis(), (double)out.gasHeatPct,
                       (double)out.hpHeatPct, (double)out.coolPct,
                       (double)out.fanPct, (double)out.defrostTemperPct,
                       (double)fused.value, (double)s.heatSp, (double)s.coolSp,
                       (unsigned)s.mode, s.action);
    } }
#endif  // SLYTHERM_UI

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
    // Only registered slots (issue #73: with no local sensor, slot 0 is unused
    // and no "local" row/entity is published).
    if (!gSensorTable[i].used) continue;
    Snapshot::SensorPub& sp = s.sensors[pubIdx++];
    sp.used = true;
    strlcpy(sp.name, gSensorTable[i].name, sizeof(sp.name));
    const SensorStatus st = gFusion.status(static_cast<uint8_t>(i), nowS);
    sp.ageS = st.ageS == UINT32_MAX ? 0 : st.ageS;
    // Reflect the user's effective opt-in choice (roster member AND participating),
    // which fusion carries as st.participating — not st.live, so an ON-but-stale
    // sensor still shows participating (staleness has its own age topic).
    sp.participating = st.participating;
    sp.offsetC = st.offsetC;
  }
  xSemaphoreGive(gCmdMux);
  // #117: name the dominant participant so a Remote can mark "Following".
  char domId[kSensorNameLen] = "";
  { const uint8_t dom = gFusion.dominantParticipant();
    xSemaphoreTake(gCmdMux, portMAX_DELAY);
    if (dom < kFusionSlots && gSensorTable[dom].used)
      strlcpy(domId, gSensorTable[dom].name, sizeof(domId));
    xSemaphoreGive(gCmdMux); }
  snprintf(s.fusionJson, sizeof(s.fusionJson),
           "{\"temp\":%.2f,\"tier\":\"%s\",\"participants\":[%s],\"occupied\":%s,\"dominant\":\"%s\"}",
           static_cast<double>(fused.value), tierName(fused.tier), parts,
           occupied ? "true" : "false", domId);

  // Compressor diagnostics (glue estimate; the guard owns the real timers).
  // Shared helper: the UI "cooling/heating soon" ack reuses the SAME value.
  s.compMinOffRemainS = compGuardMinOffRemainS(nowS);

  // Composed wall-screen wording for HA display parity — mirrors ui_main.cpp
  // renderMain (status) and fillPresenceLine (tracking). Published on
  // slytherm/state/status_line + tracking_line so a dashboard shows the same
  // text the screen does. Rich edge cases (last-entered ages) stay screen-only.
  if (strcmp(s.action, "heating") == 0 || strcmp(s.action, "defrosting") == 0)
    snprintf(s.statusLine, sizeof(s.statusLine), "Heating to %.1f\xC2\xB0", (double)s.heatSp);
  else if (strcmp(s.action, "cooling") == 0)
    snprintf(s.statusLine, sizeof(s.statusLine), "Cooling to %.1f\xC2\xB0", (double)s.coolSp);
  else if (s.compMinOffRemainS > 0)
    snprintf(s.statusLine, sizeof(s.statusLine), "%s soon \xE2\x80\xA2 %lu min",
             gModeSm->mode() == UserMode::kCool ? "Cooling" : "Heating",
             (unsigned long)((s.compMinOffRemainS + 59u) / 60u));
  else if (gModeSm->mode() == UserMode::kOff)
    strlcpy(s.statusLine, "System off", sizeof(s.statusLine));
  else if (gModeSm->mode() == UserMode::kAuto)
    snprintf(s.statusLine, sizeof(s.statusLine), "Idle - holding %.0f-%.0f\xC2\xB0",
             (double)s.heatSp, (double)s.coolSp);
  else
    strlcpy(s.statusLine, "Idle", sizeof(s.statusLine));

  if (s.asleep) {
    if (domId[0]) snprintf(s.trackingLine, sizeof(s.trackingLine), "Reading %s \xE2\x80\xA2 Asleep", domId);
    else          strlcpy(s.trackingLine, "Asleep", sizeof(s.trackingLine));
  } else if (occupied && domId[0]) {
    snprintf(s.trackingLine, sizeof(s.trackingLine), "Reading %s \xE2\x80\xA2 Present", domId);
  } else if (domId[0]) {
    snprintf(s.trackingLine, sizeof(s.trackingLine), "Reading %s \xE2\x80\xA2 Nobody home", domId);
  } else {
    strlcpy(s.trackingLine, "No room sensor reporting", sizeof(s.trackingLine));
  }
  s.compLockedOut = gGuard.lockedOut() || !oat.valid ||
                    oat.valueC < gDualFuel->config().compressorMinOatC;

  strlcpy(s.changeReason, changeReason, sizeof(s.changeReason));
  s.healthProblem = gSup->healthProblem();
  strlcpy(s.lastError, gSup->alarms().lastErrorText(), sizeof(s.lastError));
  // #116: alarm summary for the Remote echo — same registry the wall UI renders.
  s.alarmN = gSup->alarms().count();
  if (s.alarmN > 0) strlcpy(s.alarm1, gSup->alarms().at(0).text, sizeof(s.alarm1));
  if (s.alarmN > 1) strlcpy(s.alarm2, gSup->alarms().at(1).text, sizeof(s.alarm2));
  // #118: vacation banner (same source as gUi.setVacation below).
  s.vacationActive = gVacUiActive;
  strlcpy(s.vacBanner, gVacBanner, sizeof(s.vacBanner));
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

#ifdef SLYTHERM_ACTUATOR_RELAY
  if (gActuatorPtr) gActuatorPtr->fillRelaysJson(s.relaysJson, sizeof(s.relaysJson));
#endif

  xSemaphoreTake(gSnapMux, portMAX_DELAY);
  gSnap = s;
  xSemaphoreGive(gSnapMux);
}

void controlCycle(uint32_t nowS, uint32_t nowMs) {
  consumeCommands(nowS);

  // ---- Inputs ----
#ifdef SLYTHERM_DS18B20
  pollLocalSensors(nowS);
#endif
  const FusedTemp fused = gFusion.fusedTemp(nowS);
  const OatReading oat = gOat.read(nowS);
  // #153 coast visibility: start/end edges to the telnet stream so shadow
  // analysis can tell a bridged gap from live fusion. telnet_log is a
  // UI-build facility (headless env omits it — v0.5.0 lesson).
#ifdef SLYTHERM_UI
  { static bool wasCoasting = false;
    if (fused.coasting != wasCoasting) {
      wasCoasting = fused.coasting;
      telnet_log::logf("[fusion] coast %s T=%.2f tier=%s",
                       fused.coasting ? "start" : "end",
                       (double)fused.value, tierName(fused.tier));
    } }
#endif
  // #141: fused-temp slope for crossing prediction (dropouts skipped inside).
  gTrend.update(fused.value, fused.valid, nowS);

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
            "MQTT stale: fallback setpoints", nowS, /*autoClear=*/true);

  // ---- Boot gate (docs/04 §3 boot validation) ----
  safety::BootFacts bf;
  bf.sensorOk = fused.valid;
  bf.setpointPresent = gSetpointsValidated;
  bf.configCrcOk = gConfigOk;
  gSup->updateBootGate(bf, nowS);

  // ---- #151: one-shot wall-clock outage charge for a restored timed hold ----
  // Cheap no-op once the pending flag clears (first NTP sync after a restore).
  if (clockIsSynced() &&
      gModeSm->applyHoldEpochCorrection(static_cast<uint32_t>(time(nullptr)), nowS)) {
    Serial.printf("[hold] outage-corrected: type=%u remain=%lus\n",
                  static_cast<unsigned>(gModeSm->activeHoldType()),
                  static_cast<unsigned long>(gModeSm->holdRemainingS(nowS)));
  }

  // ---- Vacation hold (#78): eco setpoints override schedule/presence in-window ----
  evaluateVacation(nowS);
  // ---- Night Sleep state (issue #90; model in lib/SleepState/SleepState.h) ----
  // Presence with the away countdown FROZEN while asleep: raw #88 presence
  // first, then re-evaluate over a window widened by the sleep overlap so a
  // motionless night never decays to Away and the 3 h logic resumes from
  // where it paused on wake. Evaluated before the mode SM so a sleep-preset
  // edge lands in this same cycle.
  if (gUiTouchPing) { gUiTouchPing = false; gSleep.noteTouch(nowS); }
  PresenceState pres = gFusion.presence(nowS);
  { const uint32_t credit = gSleep.awayCreditS(pres.lastSeenAgeS, nowS);
    if (credit > 0) pres = gFusion.presenceWithin(kPresenceAwayS + credit, nowS); }
  { int hour = -1; struct tm ti;
    const bool clockOk = getLocalTime(&ti, 0);
    if (clockOk) hour = ti.tm_hour;
    const bool wasAsleep = gSleep.asleep();
    const bool asleep =
        gSleep.update(clockOk, hour, pres.present, pres.anyReporting, nowS);
    if (asleep != wasAsleep) {
      gSleepLink.onEdge(asleep, *gModeSm, nowS);  // apply/restore the sleep preset
      Serial.printf("[sleep] %s (hour=%d present=%d)\n",
                    asleep ? "asleep" : "awake", hour, (int)pres.present);
    }
  }

  // ---- Mode state machine -> effective call ----
  const bool swapOk = compressorProvenIdle(nowS);
  Call call = gModeSm->update(fused.value, fused.valid, nowS, swapOk);

  // DS18B20-only degraded mode (docs/04 §4): cooling disabled, heat-to ceiling.
  if (fused.degraded) {
    if (call.type == CallType::kCool) call = Call{};
    if (call.type == CallType::kHeat && fused.value >= kDegradedHeatCeilC) call = Call{};
  }
  glueAlarm(fused.degraded, cfg::kAlarmDegradedMode, safety::Severity::kCritical,
            "DS18B20-only degraded mode", nowS, /*autoClear=*/true);

  // ---- Dual fuel ----
  DualFuelInputs dfi;
  dfi.heatCall = call.type == CallType::kHeat;
  dfi.setpointC = gModeSm->heatSetpoint();
  dfi.roomTempC = fused.value;
  dfi.roomTempValid = fused.valid;
  dfi.oatC = oat.valueC;
  dfi.oatValid = oat.valid;
  dfi.hpDemandPct = gLastHpEmittedPct;
#ifdef SLYTHERM_ACTUATOR_RELAY
  dfi.defrostActive = cfg::kSenseDPin >= 0 && digitalRead(cfg::kSenseDPin) == HIGH;
#endif
  const DualFuelOutput dfo = gDualFuel->step(dfi, nowS);
  glueAlarm(dfo.oatInvalidAlarm, cfg::kAlarmOatFailCold, safety::Severity::kAdvisory,
            "OAT invalid: fail-cold (gas only)", nowS, /*autoClear=*/true);

  // ---- Per-equipment requests ----
  float gasReq = 0, hpReq = 0, coolReq = 0, fanReq = 0;
  const bool temperActive = dfo.temperRequest;
  static const char* changeReason = "none";
  if (call.type == CallType::kHeat) {
    // EMERGENCY_HEAT is gas-only by definition; the user's explicit choice
    // overrides the high-OAT gas lockout (decision flagged in the report).
    // #141: a recovery that fell below the fallback ramp also picks gas
    // (advisory verdict from adviseRecovery, default OFF — winter task);
    // GasShaper timers/lockouts still gate downstream.
    const HeatSource src = (call.gasOnly || gRecoveryGasAdvised)
                               ? HeatSource::kGas : dfo.source;
    if (src == HeatSource::kGas) {
      gPid.selectMode(call.gasOnly ? 1 : 0);
      gasReq = gPid.update(gModeSm->heatSetpoint(), fused.value, fused.valid,
                           temperActive, nowS);
      if (fused.degraded) gasReq = fminf(gasReq, cfg::kDegradedGasCapPct);
      changeReason = call.gasOnly ? "manual"
                     : gRecoveryGasAdvised ? "recovery_ramp"
                     : dfo.escalated ? "escalation"
                     : dfo.oatInvalidAlarm ? "fail_cold" : "balance_point";
    } else if (src == HeatSource::kHeatPump) {
      gPid.reset();  // no stale integrator when gas re-stages
      hpReq = stagedRequestPct(call.errorC);
      changeReason = "balance_point";
    }
  } else {
    gPid.reset();
    // Cooling lockouts (docs/05 table): indoor floor + unknown OAT.
    const bool coolLockedOut = !oat.valid || fused.degraded ||
                               fused.value < kCoolingIndoorLockoutC;
    if (call.type == CallType::kCool) {
      // #140: the request is a RUNTIME-DUTY fraction (proportional band on
      // the error), not a capacity — the stage only understands 30%.
      if (!coolLockedOut) coolReq = gCoolShaper.requestFromError(call.errorC);
      changeReason = coolLockedOut ? "lockout" : changeReason;
    } else if (kCoolPredictEnabledDefault && call.type == CallType::kNone &&
               gModeSm->mode() == UserMode::kCool && fused.valid &&
               !coolLockedOut && gTrend.valid()) {
      // #141 crossing prediction (docs/13 §2): when the fused slope projects
      // a deadband crossing within the horizon, begin the gentle duty ramp
      // through the #140 band early instead of waiting for the slam at
      // +hysteresis. COOL mode only (in AUTO a pre-cool without a call would
      // sidestep the changeover dwell bookkeeping — winter/AUTO follow-ups
      // stay with #141). Advisory shaping of the REQUEST only: min-run/
      // min-off/starts-cap and CompressorGuard still gate downstream.
      const CrossingBias cb = RecoveryEstimator::crossingBias(
          gModeSm->coolSetpoint() + kCallHysteresisC - fused.value,
          gTrend.slopeCPerH(), kCoolPredictHorizonS, kCoolPredictBiasMaxC);
      if (cb.predicted) {
        const float req = gCoolShaper.requestFromError(
            fused.value - gModeSm->coolSetpoint(), cb.biasC);
        // Pre-action floor: below it the predicted duty's first on-phase
        // could end before the call opens and fragment the cycle.
        if (req >= kCoolPredictMinReqPct) {
          coolReq = req;
          changeReason = "predict";
        }
      }
    }
  }
  // ---- #142 blower-first pre-circulation (docs/13 §3, §8-gated for cooling) ----
  // When #141's crossing prediction (horizon = the pre-run lead) says a call
  // is imminent, request the blower LOW so the space is mixed — and
  // SensorFusion reads whole-space truth — BEFORE the stage commits. A fan
  // REQUEST like any other: SafetySupervisor filter + DemandArbiter stay
  // downstream, a live call/request always overrides, prediction loss
  // cancels. Heat-only by default; the cool side is §8's latent-penalty
  // territory (kBlowerFirstCoolEnabledDefault=false) and additionally
  // respects the cooling lockouts. Same mode restriction as #141: explicit
  // HEAT/COOL only, no pre-runs around the AUTO changeover dwell.
  float preFanReq = 0.0f;
  {
    PreCirculator::Inputs pci;
    pci.callActive = call.type != CallType::kNone || gasReq > 0 || hpReq > 0 ||
                     coolReq > 0 || temperActive;
    if (fused.valid && !fused.degraded && gTrend.valid()) {
      const uint32_t leadS = gPreCirc.config().leadS;
      if (gModeSm->mode() == UserMode::kHeat) {
        // Heat call opens at heatSp - hysteresis; approach = falling temp.
        pci.heatPredicted = RecoveryEstimator::crossingBias(
            fused.value - (gModeSm->heatSetpoint() - kCallHysteresisC),
            -gTrend.slopeCPerH(), leadS, kCoolPredictBiasMaxC).predicted;
      } else if (gModeSm->mode() == UserMode::kCool && oat.valid &&
                 fused.value >= kCoolingIndoorLockoutC) {
        pci.coolPredicted = RecoveryEstimator::crossingBias(
            gModeSm->coolSetpoint() + kCallHysteresisC - fused.value,
            gTrend.slopeCPerH(), leadS, kCoolPredictBiasMaxC).predicted;
      }
      // (biasMaxC only gates crossingBias validity; the bias itself is unused
      // here — the pre-run consumes the predicted flag alone.)
    }
    preFanReq = gPreCirc.update(pci, nowS);
  }
  if (gModeSm->mode() != UserMode::kOff) {
    if (gFanMode == hm::FanMode::kOn) fanReq = 100.0f;
    else if (gFanMode == hm::FanMode::kCirculate) {
      // #142/#53: pre-run seconds COUNT TOWARD the circulate duty — the
      // minutes-per-hour window shrinks by what pre-circulation already ran
      // this hour (docs/13 §3: never runtime on top). #128: minutes + speed are
      // now runtime config (gFanCircMin/gFanCircPct), not compile constants.
      fanReq = fan::circulateRequestPct(nowS, gFanCircMin, gFanCircPct,
                                        gPreCirc.dutyCreditS(nowS));
    }
    fanReq = fmaxf(fanReq, preFanReq);  // pre-run never lowers an explicit fan-on
  }

  // ---- Safety facts + supervisor (before any emission) ----
  safety::HealthFacts hf;
  hf.sensorValid = fused.valid;
  hf.setpointFresh = setpointFresh;
  hf.mqttAlive = gMqttConnected;
#ifdef SLYTHERM_CT485_UART
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
    // One compressor, two family shapers (#140): each owns its channel, so a
    // min-on-held output can never flip heat<->cool mid-run. Simultaneous
    // nonzero requests are unreachable (one Call type at a time and the AUTO
    // changeover dwell of 30 min dwarfs every demand-level hold), and the
    // shared guard gate + arbiter invariant back that up downstream.
    req.hpHeatPct = gHpShaper.shape(hpReq, nowS);
    req.coolPct = gCoolShaper.shape(coolReq, nowS);
    req.fanPct = fanReq;
    req.defrostTemperPct = temperActive ? dfo.temperHeatPct : 0.0f;
#if defined(SLYTHERM_CT485_UART) && defined(SLYTHERM_CT485_TX_ENABLE)
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
            "CT-485 TX stack alarm (see slytherm/state/bus)", nowS);

  // ---- External hardware watchdog pet (docs/04 §3 pet-gating) ----
  if (gSup->petExternalWdt() && cfg::kWdtPetPin >= 0) {
    gWdtPetLevel = !gWdtPetLevel;
    digitalWrite(cfg::kWdtPetPin, gWdtPetLevel ? HIGH : LOW);
  }

  // ---- Advisory recovery + learning ----
  updateRunSegments(out, fused, nowS);
  adviseRecovery(fused, oat, nowS);

  // ---- #143 record-only COP proxy (docs/13 §5) ----
  // Accumulate HP-heat runtime vs indoor-outdoor delta per OAT bucket, then
  // surface it as TELEMETRY ONLY (NVS blob + [copx] telnet + retained MQTT).
  // Deliberately NOT fed back into the arbiter's COP curve — that closes
  // after a season of data (see the correction seam in CopLearner.h).
  gCopLearner.tick(out.hpHeatPct > 0, fused.value, fused.valid, oat.valueC,
                   oat.valid, nowS);
  {
    static uint32_t lastSaveS = 0, lastPubS = 0;
    static uint32_t savedRunS = 0, pubbedRunS = 0;
    const uint32_t runS = gCopLearner.totalRuntimeS();
    if (runS != savedRunS && nowS - lastSaveS >= kCopSaveMinS) {
      CopLearner::PersistBlob blob;
      gCopLearner.save(&blob);
      gPrefs.putBytes("copx", &blob, sizeof(blob));
      lastSaveS = nowS;
      savedRunS = runS;
    }
    if (runS > 0 && (runS != pubbedRunS || lastPubS == 0) &&
        nowS - lastPubS >= kCopPublishMinS) {
      xSemaphoreTake(gCmdMux, portMAX_DELAY);
      gCopProxyJson = gCopLearner.proxyJson();
      gCopProxyPubReq = true;  // mqttTask publishes retained (owns gMqtt)
      xSemaphoreGive(gCmdMux);
      lastPubS = nowS;
      pubbedRunS = runS;
    }
#ifdef SLYTHERM_UI  // telnet_log is a UI-build facility (v0.5.0 lesson)
    static uint32_t lastLogS = 0;
    if (runS > 0 && nowS - lastLogS >= kCopLogMinS) {
      telnet_log::logf("[copx] %s", gCopLearner.proxyJson().c_str());
      lastLogS = nowS;
    }
#endif
  }

  // ---- UI model sync (rendered by the wall-UI task, SLYTHERM_UI) ----
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
  // "Cooling/Heating soon" ack (owner report): a genuine compressor call is
  // pending but a min-OFF timer denies the start, so the panel must NOT read
  // plain "Idle". HvacAction above stays idle (HA hvac_action enum unchanged) —
  // this is on-panel richness only. Pending = the call wants the compressor
  // (cool duty / HP-heat request > 0) yet nothing is emitted this cycle; the
  // predicate then binds on the larger of the guard (180 s) and cool-shaper
  // (480 s) min-OFF rests, and a lockout suppresses it (see evalCompressorHold).
  { const bool coolPendingIdle =
        call.type == CallType::kCool && coolReq > 0.0f && out.coolPct <= 0.0f;
    const bool heatPendingIdle =
        call.type == CallType::kHeat && hpReq > 0.0f && out.hpHeatPct <= 0.0f;
    const ui::CompressorHold chold = ui::evalCompressorHold(
        coolPendingIdle, heatPendingIdle, gGuard.lockedOut(),
        compGuardMinOffRemainS(nowS), gCoolShaper.minOffRemainingS(nowS));
    gUi.setCompressorHold(chold.held, chold.side, chold.remainS); }
  gUi.setOutdoor(oat.valueC, oat.valid,
                 oat.rung == OatRung::kBus ? ui::OutdoorSource::kBus
                 : oat.rung == OatRung::kWired ? ui::OutdoorSource::kWiredDs18b20
                 : oat.rung == OatRung::kHaWeather ? ui::OutdoorSource::kHaWeather
                                                   : ui::OutdoorSource::kNone);
  gUi.setGasModulationPct(out.gasHeatPct);
  gUi.setLinkHealth(gWifiConnected, gMqttConnected, hf.busAlive,
                    gWifiConnected ? static_cast<int8_t>(WiFi.RSSI()) : 0);  // #127
  { struct tm ti; char cb[24] = "";  // top-bar clock (#69); blank until NTP syncs
    if (getLocalTime(&ti, 0)) {
      if (gClock24) strftime(cb, sizeof(cb), "%a %H:%M", &ti);
      else { char day[8]; strftime(day, sizeof(day), "%a", &ti); int h = ti.tm_hour % 12; if (h == 0) h = 12;
        snprintf(cb, sizeof(cb), "%s %d:%02d%s", day, h, ti.tm_min, ti.tm_hour < 12 ? "am" : "pm"); }
    }
    gUi.setClock(cb); }
  { uint32_t ctFrames = 0;
#ifdef SLYTHERM_CT485_UART
    ctFrames = gCtAcc.counters().framesOk;   // live only when the RS-485 UART is compiled in
#endif
    gUi.setBusDiag(gLastBusRxS, ctFrames); }
  gUi.setDegradedMode(fused.degraded);
  gUi.setHoldStatus(gModeSm->activeHoldType(), gModeSm->holdRemainingS(nowS));  // hold chooser (#81)
  gUi.setVacation(gVacUiActive, gVacBanner);  // #78: vacation banner ("Vacation until <date>")
  // Live preset roster -> UI (#74): real names + setpoints from the authoritative
  // ModeStateMachine roster (retained slytherm/config/presets or boot defaults).
  { ui::DisplayState::PresetView pv[kMaxPresets]; uint8_t pn = 0;
    const size_t pc = gModeSm->presetCount();
    for (size_t i = 0; i < pc && pn < kMaxPresets; ++i) {
      const PresetDef* d = gModeSm->presetAt(i);
      if (!d) continue;
      strlcpy(pv[pn].name, d->name, sizeof(pv[pn].name));
      pv[pn].heatC = d->heatC; pv[pn].coolC = d->coolC; ++pn;
    }
    gUi.setPresets(pv, pn);
    gUi.setActivePreset(gModeSm->activePreset()); }  // #90/preset-highlight: authoritative
  // Per-sensor rows for the Sensors screen + ambient "driving sensor".
  {
    ui::SensorRow rows[ui::kMaxSensorRows];
    uint8_t rn = 0;
    const uint8_t dom = gFusion.dominantParticipant();
    for (size_t i = 0; i < kFusionSlots && rn < ui::kMaxSensorRows; ++i) {
      if (!gSensorTable[i].used) continue;
      const SensorStatus stt = gFusion.status(static_cast<uint8_t>(i), nowS);
      ui::SensorRow& r = rows[rn++];
      const char* disp = gSensorTable[i].disp[0] ? gSensorTable[i].disp : gSensorTable[i].name;  // #85: friendly name, fallback to id
      strlcpy(r.name, disp, sizeof(r.name));
      r.tempC = stt.tempC;
      r.occupied = stt.occupied;
      r.ageS = (stt.ageS == 0xFFFFFFFFu) ? 0 : stt.ageS;
      r.participating = dettson::fusionParticipates(gSensorTable[i].inRoster,
                                                    gSensorTable[i].participating);
      r.healthy = stt.faults == 0 && stt.hasTemp;
      r.dominant = (static_cast<uint8_t>(i) == dom);
      r.lastOccAgeS = stt.lastOccAgeS;
    }
    gUi.setSensorRows(rows, rn);
  }
  // #88/#90: presence line — sticky home/away from HA's REPORTED last_seen
  // (3 h across all presence sensors), decoupled from the motion window + temp
  // staleness; `pres` computed above with the Sleep-state away freeze applied.
  // valid=anyReporting so with no presence sensor the UI falls back to the
  // temp-health text ("Local sensor only" / "No room sensor reporting").
  {
    char room[kSensorNameLen] = "";
    // Name the room the fusion is actually FOLLOWING for control — the dominant
    // PARTICIPANT (same source as the Sensors page's "Following") — not merely
    // the most-recently-SEEN presence room (pres.dominantId), which can be a room
    // someone just passed through, or even a non-participating one. Keeps the
    // Home "Reading <room>" line consistent with what drives the fused temp.
    const uint8_t follow = gFusion.dominantParticipant();
    const uint8_t which  = (follow != 0xFF) ? follow : pres.dominantId;
    if (pres.present && which != 0xFF && which < kFusionSlots) {
      const SensorEntry& e = gSensorTable[which];
      strlcpy(room, e.disp[0] ? e.disp : e.name, sizeof(room));
    }
    gUi.setPresence(pres.anyReporting, pres.present, pres.anyReporting, room,
                    pres.lastSeenAgeS, gSleep.asleep());
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

#ifdef SLYTHERM_UI
// Wall-UI task (core 0). Renders gUi (filled by the control task) and routes
// touch into it — display-only, demand authority stays in the control task.
void uiTask(void*) {
  slytherm_ui::begin(&gUi, gUiMux, gReducedUi, gFirstRun);
  for (;;) {
    slytherm_ui::service();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
#endif

#ifdef SLYTHERM_DS18B20
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
  Serial.println("SlyTherm thermostat (issue #55) — demands "
#if defined(SLYTHERM_CT485_TX_ENABLE)
                 "TX-ENABLED (Phase 3 build)"
#else
                 "DISABLED (logging/stub build)"
#endif
  );
  Serial.println("fw " SLYTHERM_FW_BUILD);  // #113: VERSION file + git sha
  boot_guard::begin("bootg");  // #122/#123: count this boot + capture reason/coredump

  if (cfg::kWdtPetPin >= 0) { pinMode(cfg::kWdtPetPin, OUTPUT); digitalWrite(cfg::kWdtPetPin, LOW); }

  gPrefs.begin(cfg::kNvsNamespace, false);
  // A freshly-flashed firmware clears the reset-loop + safe-UI latch (a new build is
  // presumed to fix whatever crashed). Compare the running app hash to the stored one.
  { esp_app_desc_t d{}; const esp_partition_t* run = esp_ota_get_running_partition();
    if (run && esp_ota_get_partition_description(run, &d) == ESP_OK) {
      uint8_t saved[8] = {0}; gPrefs.getBytes("fwid", saved, sizeof(saved));
      if (memcmp(d.app_elf_sha256, saved, 8) != 0) {
        gPrefs.putBytes("fwid", (const void*)d.app_elf_sha256, 8);
        gPrefs.putBool("rui", false);
        ResetLoopBlob z{}; gPrefs.putBytes("rl", &z, sizeof(z));
        // #151 SUPERSEDES #87's hold-clear-on-new-firmware: an OTA reboot must
        // NOT drop the user's active hold (observed 2026-07-11: the 0.5.9 OTA
        // silently returned an owner-pinned indefinite hold to the schedule).
        // #87's stale-pill worry is covered structurally now — the hold blob is
        // magic/version/CRC-guarded, so a firmware whose blob layout changed
        // bumps kHoldBlobVersion and the restore fails open to no hold.
        Serial.println("[boot] new firmware -> cleared reset-loop + safe-UI latch");
      } } }
  // Restore the persisted LISTEN capture state (survives power loss so a
  // capture campaign resumes without hands on the glass; set via the LISTEN
  // buttons or slytherm/cmd/sniff).
  gSniffActive = gPrefs.getBool("sniff", false);
  if (gSniffActive) Serial.println("[boot] CT-485 LISTEN capture restored: active");
  // #64: OTA-capability check. Both stock tables (default_8MB/S3,
  // default_16MB/P4) are dual-app; if next is NULL the table has no second
  // slot and the OTA client (#61) must hard-disable with a visible reason.
  { const esp_partition_t* run = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    Serial.printf("[boot] OTA %s: running=%s next=%s\n",
                  next ? "capable" : "NOT capable (single-app table)",
                  run ? run->label : "?", next ? next->label : "none"); }
  // #61: pending-update validation — arms the self-test (confirmed on MQTT
  // connect) or rolls back a crash-looping new image. No-op without
  // -DSLYTHERM_OTA or when no update is pending.
  ota::bootValidate();
  gClock24 = gPrefs.getBool("clk24", false);  // top-bar 12/24h preference (#69)

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

  // ---- Reduced safe-UI latch (issue #80) ----
  // A reset loop (often a crash in an optional UI widget) latches a persistent
  // "reduced UI" flag: the next boot builds a MINIMAL known-good screen instead
  // of re-running the code that crashed. It survives until the user taps
  // "Restore full screen" (uiClearReducedMode), so a cosmetic bug can't
  // boot-loop the panel. Control/safety already fail to no-demand (above).
  gReducedUi = gPrefs.getBool("rui", false);
  if (gSup->resetLoop().latched()) { gReducedUi = true; gPrefs.putBool("rui", true); }

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
    dc.economicEnabled = tun.economicEnabled;      // #143 (default OFF)
    dc.elecPricePerKwh = tun.elecPricePerKwh;
    dc.gasPricePerM3 = tun.gasPricePerM3;
    gConfigOk = DualFuelArbiter::configValid(dc);  // hard rule, docs/04 §4
    gDualFuel = new DualFuelArbiter(dc);           // invalid -> validated defaults
    gArbiter = new DemandArbiter(nowS);
    Serial.printf("[econ] %s: elec=%.3f$/kWh gas=%.3f$/m3 afue=%.2f COP*=%.2f "
                  "balance=%.1fC\n",
                  gDualFuel->config().economicEnabled ? "ON" : "OFF (fixed balance)",
                  (double)gDualFuel->config().elecPricePerKwh,
                  (double)gDualFuel->config().gasPricePerM3,
                  (double)gDualFuel->config().afue,
                  (double)gDualFuel->breakEvenCop(),
                  (double)gDualFuel->effectiveBalancePointC());
  }

  // ---- #143 COP-proxy record restore (record-only; a bad blob starts fresh) ----
  {
    CopLearner::PersistBlob blob;
    if (gPrefs.getBytes("copx", &blob, sizeof(blob)) == sizeof(blob) &&
        gCopLearner.restore(&blob)) {
      Serial.printf("[copx] restored %lus of HP-heat record\n",
                    (unsigned long)gCopLearner.totalRuntimeS());
    }
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
    // #151: restore the hold blob — indefinite/until-next-preset as-is, timed
    // with remaining decremented by the monotonic elapsed (the outage itself
    // is charged later by applyHoldEpochCorrection once NTP syncs; see the
    // control loop). Missing/corrupt blob = no hold (fail open, header note).
    {
      ModeStateMachine::HoldPersistBlob hb;
      bool restored = gPrefs.getBytes("holdb", &hb, sizeof(hb)) == sizeof(hb) &&
                      gModeSm->restoreHold(hb, nowS);
      if (!restored) {
        // One-shot migration from the pre-#151 type-only uchar key (so the
        // upgrade TO this firmware keeps the hold the old firmware persisted).
        // Old semantics: timed holds restart their full window — conservative.
        const HoldType legacy = static_cast<HoldType>(gPrefs.getUChar("hold", 0));
        if (legacy != HoldType::kNone && legacy <= HoldType::kIndefinite) {
          gModeSm->startHold(legacy, nowS);
          // Write the blob NOW: the legacy key is removed below, so without
          // this a second reboot before any hold change would lose the hold.
          ModeStateMachine::HoldPersistBlob mig;
          gModeSm->saveHold(&mig, nowS, 0);
          gPrefs.putBytes("holdb", &mig, sizeof(mig));
          restored = true;
        }
      }
      gPrefs.remove("hold");  // legacy key retired (migration above)
      if (restored)
        Serial.printf("[hold] restored: type=%u remain=%lus\n",
                      static_cast<unsigned>(gModeSm->activeHoldType()),
                      static_cast<unsigned long>(gModeSm->holdRemainingS(nowS)));
      // Sync the change-detect shadow so a clean boot doesn't rewrite the blob
      // (an epoch correction later changes holdEndS and re-persists).
      gShadow.hold = static_cast<uint8_t>(gModeSm->activeHoldType());
      gShadow.holdEnd = gModeSm->holdEndS();
    }
    gFanMode = static_cast<hm::FanMode>(gPrefs.getUChar("fan", 0));
    if (gFanMode > hm::FanMode::kCirculate) gFanMode = hm::FanMode::kAuto;
    // #128: restore the runtime circulate config (cfg:: constants are the
    // DEFAULTS); re-clamp/snap defensively in case an older/corrupt value lands.
    gFanCircMin = fan::clampCirculateMinPerHour(
        static_cast<long>(gPrefs.getUInt("fcmin", cfg::kFanCirculateMinPerHour)));
    gFanCircPct = fan::snapCirculatePct(gPrefs.getFloat("fcpct", cfg::kFanCirculatePct));
    gShadow.fanCircMin = gFanCircMin;  // sync change-detect so a clean boot doesn't rewrite NVS
    gShadow.fanCircPct = gFanCircPct;
    gSetpointsValidated = true;
  }

  // ---- Vacation hold restore (#78): reload the persisted window/eco setpoints ----
  {
    VacationState vb;
    if (gPrefs.getBytes("vac", &vb, sizeof(vb)) == sizeof(vb) && vb.on) {
      gVac = vb;   // keep absolute epochs so a reboot never extends the window;
                   // if it was set before NTP (anchored=false) evaluateVacation anchors later
      Serial.printf("[vac] restored: start+%ud len %un eco %.1f/%.1f\n",
                    (unsigned)gVac.startDays, (unsigned)gVac.nights,
                    (double)gVac.heatC, (double)gVac.coolC);
    }
  }

  // ---- UiModel screen-lock blob (fails open by design) ----
  {
    ui::UiModel::LockPersistBlob blob;
    if (gPrefs.getBytes("lock", &blob, sizeof(blob)) == sizeof(blob))
      gUi.restoreLock(&blob, nowS);
    gUi.saveLock(&gShadowLock);  // sync change-detect shadow to the restored state
  }

  // ---- Sensors ----
  // Restore persisted per-sensor participation BEFORE any roster replay so the
  // first fusion already reflects the user's choices. Fails open (all default
  // ON) on an absent/corrupt blob (fromBlob returns false, store stays empty).
  {
    ParticipationBlob blob;
    if (gPrefs.getBytes("part", &blob, sizeof(blob)) == sizeof(blob))
      gParticipation.fromBlob(blob);
  }
#ifdef SLYTHERM_LOCAL_SENSOR
  gFusion.registerSensor(0, /*isLocal=*/true);  // id 0 = "local" DS18B20 slot
  gSensorTable[0].used = true;
  strlcpy(gSensorTable[0].name, hm::kLocalSensorId, kSensorNameLen);
  gSensorTable[0].inRoster = true;
  gSensorTable[0].participating = true;  // local slot always participates (fusion treats it as isLocal)
#endif
#ifdef SLYTHERM_DS18B20
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
#if defined(SLYTHERM_CT485_TX_ENABLE)
    // Go-live (docs/02 §5a): open the demand-offset gate to the field-confirmed
    // variant A (payload[12]=refresh timer, [13]=demand). Only in the TX build —
    // shadow/probe builds leave kUnset so buildDemandFrame() refuses every demand.
    cc.offsetVariant = ct485::OffsetVariant::kVarA;
    // Go-live join (observed on the real bus, v1.0): this Controller REPLACES the
    // OEM thermostat on a coordinator that already knows node 1 and R2R-polls it
    // directly (255->1, docs/02 §6). AutoNet never completes — the coordinator
    // won't re-address an occupied slot, so a fresh node cycles slot_wait/
    // discovery_responded forever. Claim node 1 and answer those polls instead:
    // the "remove-and-impersonate the OEM stat" path this whole design is built on.
    cc.assumeAddressed = true;   // assumedAddress defaults to kAddrThermostat (1)
    // Address demands to the COORDINATOR (0xFF), not broadcast (0x00). Observed
    // on the real bus (v1.0.2): our broadcast COOL_DEMAND (01->00) drew no furnace
    // 0x61 subsystem-busy ack and tripped the starvation watchdog, while the OEM
    // thermostat we replaced always sent demands 01->FF and the coordinator relayed
    // them to the furnace (which then ACKed). Match the OEM's proven routing.
    cc.demandDst = ct485::kAddrCoordinator;
    // Node 1 (address 0x01) is a V1-range address (docs/02 §6: V1 = 0x01-0x0E,
    // V2 = 0x10-0x3E) so it lives on subnet V1 (0x02). Because assumeAddressed
    // skips the coordinator's Set Address — where a real node LEARNS its subnet
    // (handleSetAddress: subnet_ = payload[1]) — we must hardcode the right one.
    // Observed on the real bus (v1.0.3): with the default kSubnetV2 (0x03), R2R
    // (address-based) worked but the coordinator REJECTED every COOL_DEMAND with
    // no 0x83/0x06 ack (subnet-validated) and the furnace never engaged.
    cc.assumedSubnet = ct485::kSubnetV1;
#endif
    gCt = new ct485::Ct485Thermostat(cc);
  }

#if defined(SLYTHERM_ACTUATOR_RELAY)
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
#ifdef SLYTHERM_UI
  gUiMux = xSemaphoreCreateMutex();
  // #82: first-run onboarding gate — no NVS creds and no compile-time fallback
  // means the wall UI boots to the Welcome screen instead of the empty Home.
  // Computed here (synchronously, before uiTask) so begin() sees it race-free.
  gFirstRun = !(wifi_prov::hasSavedCredentials() || strlen(THERMOSTAT_WIFI_SSID) > 0);
#endif

  // Task layout (docs/01 §4): Wi-Fi/MQTT core 0; control + CT-485 core 1,
  // CT-485 above control so a slow control cycle can't starve bus timing.
  xTaskCreatePinnedToCore(mqttTask, "mqtt", cfg::kMqttStack, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(controlTask, "control", cfg::kControlStack, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(ct485Task, "ct485", cfg::kCt485Stack, nullptr, 4, nullptr, 1);
#ifdef SLYTHERM_UI
  // Wall UI on core 0 with Wi-Fi/MQTT; control + CT-485 keep core 1 to protect
  // the control cadence and future TX turnaround (docs/03 §8, issue #28).
  xTaskCreatePinnedToCore(uiTask, "ui", 24576, nullptr, 1, nullptr, 0);
#endif
  // #61: OTA task (core 0, below MQTT). No-op without -DSLYTHERM_OTA.
  ota::begin();
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));  // all work lives in the pinned tasks
}
