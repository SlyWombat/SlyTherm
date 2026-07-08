#include "remote_mqtt.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PubSubClient.h>

#include "ota_client.h"  // #111: no-op inlines unless -DSLYTHERM_OTA
#include <WiFi.h>

#include <cstring>
#include <ctime>
#include <vector>

#include "HaMqtt.h"        // parsePresetRosterJson (pure, shared with the Controller)
#include "RemoteLink.h"    // #102 codec: echo/status parsers + intent builders
#include "boot_guard.h"   // #123: boot/crash telemetry payload
#include "mqtt_cfg.h"
#include "wifi_prov.h"   // #121: wifi_prov owns the radio

namespace remote_mqtt {
namespace {

// #113: injected by tools/version_flag.py; fallback keeps ad-hoc builds compiling.
#ifndef SLYTHERM_FW_VERSION
#define SLYTHERM_FW_VERSION "0.0.0-dev"
#endif

namespace rl = remote_link;
namespace hm = dettson::hamqtt;
using namespace dettson;
using namespace dettson::ui;

WiFiClient gNet;
PubSubClient gMqtt(gNet);
Preferences gPrefs;

UiModel* gModel = nullptr;
SemaphoreHandle_t gModelMux = nullptr;
inline void L() { if (gModelMux) xSemaphoreTake(gModelMux, portMAX_DELAY); }
inline void U() { if (gModelMux) xSemaphoreGive(gModelMux); }

char gId[16] = {};        // last 3 MAC bytes, hex -- e.g. "d2d30c"
char gClientId[32] = {};  // "slytherm-remote-d2d30c"
char gAvailTopic[48] = {};
char gIntentTopic[48] = {};
// #111 OTA surface, per-Remote (a shared cmd topic would update every
// Remote on the broker at once): slytherm/remote/<id>/cmd/ota_check|
// ota_apply -> requests; .../state/ota -> live client status.
char gOtaCheckTopic[56] = {};
char gOtaApplyTopic[56] = {};
char gOtaStateTopic[56] = {};
char gOtaStateCache[192] = {};  // diff suppression ("" -> republish)

// Topic literals (the Remote build doesn't compile the Controller's src/;
// these mirror hm::topic — one product namespace, docs/11).
constexpr const char* kRemoteState = "slytherm/remote/state";
constexpr const char* kControllerStatus = "slytherm/controller/status";
constexpr const char* kAvailability = "slytherm/availability";
constexpr const char* kPresetRoster = "slytherm/config/presets";
constexpr const char* kOutdoorTemp = "slytherm/state/outdoor_temp";

uint32_t gLastDiscoverMs = 0;
uint32_t gLastConnectTryMs = 0;
// #109: broker retry backoff — 5s -> 60s while failing, reset on success.
constexpr uint32_t kConnMinMs = 5000;
constexpr uint32_t kConnMaxMs = 60000;
uint32_t gConnRetryMs = kConnMinMs;
uint32_t gAttempts = 0;

// #102: NVS-persisted monotonic intent id. The Controller's dedupe table
// keeps our last id across OUR reboots (it only resets when the Controller
// reboots) — an id restarting at 1 would mute every intent until it caught
// up, so the counter must survive reflash/reboot.
uint32_t gIntentId = 0;

// #103: suppression window — don't reconcile to an echo within this window
// of a locally-published intent (a retained echo in flight still carries
// the PRE-edit state and would yank the value mid-adjust).
constexpr uint32_t kEchoSuppressMs = 2000;
uint32_t gLastIntentMs = 0;

bool gControllerOnline = false;
char gControllerCid[16] = {};

// ---- #117: live sensor rows, assembled from the Controller's retained
// topics (roster gives order+names; per-sensor topics fill the cells). ----
constexpr const char* kFusionTopic = "slytherm/state/fusion";
struct RowSrc {
  bool used = false;
  char id[24] = {};
  char name[24] = {};
  uint32_t maxAgeS = 900;
  bool hasTemp = false;
  float tempC = 0.0f;
  uint32_t ageS = 0xFFFFFFFFu;
  bool participating = false;
  bool occupied = false;
  uint32_t lastSeenEpoch = 0;
  bool dominant = false;
};
RowSrc gRows[kMaxSensorRows] = {};
uint8_t gRowCount = 0;
bool gRowsDirty = false;      // rebuild + push into the model on the next loop
bool gFusionOccupied = false;

RowSrc* rowById(const char* id) {
  for (uint8_t i = 0; i < gRowCount; ++i)
    if (strncmp(gRows[i].id, id, sizeof(gRows[i].id)) == 0) return &gRows[i];
  return nullptr;
}

// Push the assembled rows + presence into the UiModel (1 Hz max via gRowsDirty).
void publishRowsToModel() {
  SensorRow rows[kMaxSensorRows] = {};
  uint8_t n = 0;
  const uint32_t nowEpoch = static_cast<uint32_t>(time(nullptr));
  const bool clockOk = nowEpoch > 1600000000u;  // pre-NTP: ages unknown
  // Presence line: freshest last_seen across rows (mirrors the Controller).
  bool anyReporting = false, present = false;
  uint32_t bestAge = 0xFFFFFFFFu;
  const char* bestRoom = "";
  for (uint8_t i = 0; i < gRowCount && n < kMaxSensorRows; ++i) {
    const RowSrc& r = gRows[i];
    if (!r.used) continue;
    SensorRow& o = rows[n++];
    strlcpy(o.name, r.name, sizeof(o.name));
    o.tempC = r.tempC;
    o.occupied = r.occupied;
    o.ageS = r.ageS == 0xFFFFFFFFu ? 0 : r.ageS;
    o.participating = r.participating;
    o.healthy = r.hasTemp && r.ageS != 0xFFFFFFFFu && r.ageS < r.maxAgeS;
    o.dominant = r.dominant;
    uint32_t occAge = 0xFFFFFFFFu;
    if (r.occupied) occAge = 0;
    else if (clockOk && r.lastSeenEpoch > 0 && nowEpoch >= r.lastSeenEpoch)
      occAge = nowEpoch - r.lastSeenEpoch;
    o.lastOccAgeS = occAge;
    if (r.lastSeenEpoch > 0 || r.occupied) anyReporting = true;
    if (r.occupied) present = true;
    if (occAge < bestAge) { bestAge = occAge; bestRoom = r.name; }
  }
  L();
  gModel->setSensorRows(rows, n);
  gModel->setPresence(anyReporting, present || (bestAge < 3u * 3600u), anyReporting,
                      bestRoom, bestAge);
  U();
}

void applyEcho(const rl::ControllerEcho& e) {
  L();
  gModel->setSetpoints(e.heatC, e.coolC);
  UserMode m = UserMode::kOff;
  if (e.emHeat) m = UserMode::kEmergencyHeat;
  else if (e.mode == rl::Mode::kHeat) m = UserMode::kHeat;
  else if (e.mode == rl::Mode::kCool) m = UserMode::kCool;
  else if (e.mode == rl::Mode::kHeatCool) m = UserMode::kAuto;
  gModel->setUserMode(m);
  HoldType h = HoldType::kNone;
  switch (e.hold) {
    case rl::Hold::kUntilNextPreset: h = HoldType::kUntilNextPreset; break;
    case rl::Hold::kTwoHours: h = HoldType::kTwoHours; break;
    case rl::Hold::kFourHours: h = HoldType::kFourHours; break;
    case rl::Hold::kIndefinite: h = HoldType::kIndefinite; break;
    default: break;
  }
  gModel->setHoldStatus(h, e.holdRemainS);
  gModel->setActivePreset(e.activePreset.c_str());
  gModel->setFusedTemp(e.fusedTempC, e.fusedTempValid);
  // #116: action + equipment (wire strings from state/action|active_equipment).
  HvacAction a = HvacAction::kIdle;
  if (e.action == "heating") a = HvacAction::kHeating;
  else if (e.action == "cooling") a = HvacAction::kCooling;
  else if (e.action == "fan") a = HvacAction::kFanOnly;
  else if (e.action == "defrosting") a = HvacAction::kDefrosting;
  gModel->setHvacAction(a);
  uint8_t eq = kEquipNone;
  if (e.equipment == "gas_heat") eq = kEquipGas;
  else if (e.equipment == "hp_heat") eq = kEquipHpHeat;
  else if (e.equipment == "cool") eq = kEquipHpCool;
  else if (e.equipment == "defrost") eq = kEquipGas | kEquipHpHeat;  // tempering
  gModel->setActiveEquipment(eq);
  // #116: alarm summary — only touch the (dirtying) alarm list on change.
  static std::string sAlarmCache;
  std::string alarmSig;
  alarmSig.reserve(96);
  alarmSig += static_cast<char>('0' + (e.alarmN % 10));
  alarmSig += '|'; alarmSig += e.alarm1; alarmSig += '|'; alarmSig += e.alarm2;
  if (alarmSig != sAlarmCache) {
    sAlarmCache = alarmSig;
    gModel->clearAlarms();
    if (e.alarmN > 0 && !e.alarm1.empty()) gModel->pushAlarm(e.alarm1.c_str(), 0);
    if (e.alarmN > 1 && !e.alarm2.empty()) gModel->pushAlarm(e.alarm2.c_str(), 0);
    // More active than we carry texts for: a truthful overflow marker.
    if (e.alarmN > 2) gModel->pushAlarm("(more alarms on the Controller)", 0);
  }
  // #118: vacation banner round-trip.
  gModel->setVacation(e.vacationActive, e.vacBanner.c_str());
  U();
}

void applyRoster(const char* json) {
  std::vector<hm::PresetEntry> roster;
  if (!hm::parsePresetRosterJson(json, roster)) return;
  DisplayState::PresetView pv[kMaxPresets] = {};
  uint8_t n = 0;
  for (const auto& p : roster) {
    if (n >= kMaxPresets) break;
    strlcpy(pv[n].name, p.name.c_str(), sizeof(pv[n].name));
    pv[n].heatC = p.heatC;
    pv[n].coolC = p.coolC;
    ++n;
  }
  L();
  gModel->setPresets(pv, n);
  U();
  Serial.printf("[link] preset roster: %u presets\n", n);
}

void onMessage(char* topic, uint8_t* payload, unsigned int len) {
  // NUL-terminate a bounded copy (PubSubClient payloads aren't terminated).
  static char buf[1024];
  if (len >= sizeof(buf) || gModel == nullptr) return;
  memcpy(buf, payload, len);
  buf[len] = '\0';

  if (strcmp(topic, kRemoteState) == 0) {
    if (gLastIntentMs != 0 && millis() - gLastIntentMs < kEchoSuppressMs) return;  // #103
    rl::ControllerEcho e;
    if (rl::parseRemoteStateJson(buf, e)) applyEcho(e);
    else Serial.println("[link] BAD remote/state payload (rejected whole)");
  } else if (strcmp(topic, kAvailability) == 0) {
    gControllerOnline = (strcmp(buf, "online") == 0);
    Serial.printf("[link] controller availability: %s\n", buf);
  } else if (strcmp(topic, kControllerStatus) == 0) {
    rl::ControllerStatus cs;
    if (rl::parseControllerStatusJson(buf, cs) && strcmp(gControllerCid, cs.cid.c_str()) != 0) {
      strlcpy(gControllerCid, cs.cid.c_str(), sizeof(gControllerCid));
      Serial.printf("[link] bound controller cid=%s version=%s\n", gControllerCid, cs.version.c_str());
    }
  } else if (strcmp(topic, kPresetRoster) == 0) {
    applyRoster(buf);
  } else if (strcmp(topic, gOtaCheckTopic) == 0) {
    ota::requestCheck();   // payload ignored; no-op mid-phase (#111)
  } else if (strcmp(topic, gOtaApplyTopic) == 0) {
    ota::requestApply();
  } else if (strcmp(topic, "slytherm/config/sensors") == 0) {  // #117 roster
    std::vector<hm::SensorRosterEntry> roster;
    if (!hm::parseSensorRosterJson(buf, roster)) return;
    RowSrc fresh[kMaxSensorRows] = {};
    uint8_t n = 0;
    for (const auto& e : roster) {
      if (n >= kMaxSensorRows) break;
      RowSrc& r = fresh[n];
      r.used = true;
      strlcpy(r.id, e.id.c_str(), sizeof(r.id));
      strlcpy(r.name, e.name.empty() ? e.id.c_str() : e.name.c_str(), sizeof(r.name));
      if (e.hasMaxAge && e.maxAgeS > 0) r.maxAgeS = e.maxAgeS;
      if (RowSrc* prev = rowById(r.id)) {  // keep live cells across roster refreshes
        r.hasTemp = prev->hasTemp; r.tempC = prev->tempC; r.ageS = prev->ageS;
        r.participating = prev->participating; r.occupied = prev->occupied;
        r.lastSeenEpoch = prev->lastSeenEpoch; r.dominant = prev->dominant;
      }
      ++n;
    }
    memcpy(gRows, fresh, sizeof(gRows));
    gRowCount = n;
    gRowsDirty = true;
    Serial.printf("[link] sensor roster: %u rooms\n", n);
  } else if (strcmp(topic, kFusionTopic) == 0) {  // #117 occupancy + dominant
    rl::FusionView f;
    if (!rl::parseFusionJson(buf, f)) return;
    gFusionOccupied = f.occupied;
    for (uint8_t i = 0; i < gRowCount; ++i)
      gRows[i].dominant = !f.dominant.empty() &&
                          strncmp(gRows[i].id, f.dominant.c_str(), sizeof(gRows[i].id)) == 0;
    gRowsDirty = true;
  } else if (strncmp(topic, "slytherm/state/sensor/", 22) == 0) {  // #117 age|participating
    char id[24];
    const char* rest = topic + 22;
    const char* slash = strchr(rest, '/');
    if (!slash) return;
    size_t n = static_cast<size_t>(slash - rest);
    if (n == 0 || n >= sizeof(id)) return;
    memcpy(id, rest, n); id[n] = '\0';
    RowSrc* r = rowById(id);
    if (r == nullptr) return;
    if (strcmp(slash, "/age") == 0) {
      char* end = nullptr;
      const unsigned long v = strtoul(buf, &end, 10);
      if (end != buf) { r->ageS = static_cast<uint32_t>(v); gRowsDirty = true; }
    } else if (strcmp(slash, "/participating") == 0) {
      r->participating = strcmp(buf, "ON") == 0;
      gRowsDirty = true;
    }
  } else if (strncmp(topic, "slytherm/sensors/", 17) == 0) {  // #117 state|presence
    char id[24];
    const char* rest = topic + 17;
    const char* slash = strchr(rest, '/');
    if (!slash) return;
    size_t n = static_cast<size_t>(slash - rest);
    if (n == 0 || n >= sizeof(id)) return;
    memcpy(id, rest, n); id[n] = '\0';
    RowSrc* r = rowById(id);
    if (r == nullptr) return;
    if (strcmp(slash, "/state") == 0) {
      hm::SensorReading sr;
      if (hm::parseSensorJson(buf, sr) && sr.hasTemp) {
        r->hasTemp = true; r->tempC = sr.tempC; r->ageS = 0;
        if (sr.hasOcc) r->occupied = sr.occupied;
        gRowsDirty = true;
      }
    } else if (strcmp(slash, "/presence") == 0) {
      rl::PresenceView pv;
      if (rl::parsePresenceJson(buf, pv)) {
        r->occupied = pv.occupied;
        if (pv.lastSeenEpoch > 0) r->lastSeenEpoch = pv.lastSeenEpoch;
        gRowsDirty = true;
      }
    }
  } else if (strcmp(topic, kOutdoorTemp) == 0) {
    // Controller-published outdoor temp (plain float): feeds the top-bar
    // "Outside" readout and lets the #92 splash gate on the live link.
    char* end = nullptr;
    const float v = strtof(buf, &end);
    if (end != buf) {
      L();
      gModel->setOutdoor(v, true, OutdoorSource::kHaWeather);
      U();
    }
  }
}

void deriveIds() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(gId, sizeof(gId), "%02x%02x%02x", mac[3], mac[4], mac[5]);
  snprintf(gClientId, sizeof(gClientId), "slytherm-remote-%s", gId);
  snprintf(gAvailTopic, sizeof(gAvailTopic), "slytherm/remote/%s/status", gId);
  snprintf(gIntentTopic, sizeof(gIntentTopic), "slytherm/remote/%s/intent", gId);
  snprintf(gOtaCheckTopic, sizeof(gOtaCheckTopic), "slytherm/remote/%s/cmd/ota_check", gId);
  snprintf(gOtaApplyTopic, sizeof(gOtaApplyTopic), "slytherm/remote/%s/cmd/ota_apply", gId);
  snprintf(gOtaStateTopic, sizeof(gOtaStateTopic), "slytherm/remote/%s/state/ota", gId);
}

void discoverBroker(uint32_t nowMs) {
  if (mqtt_cfg::hostSet() || nowMs - gLastDiscoverMs < 15000) return;
  gLastDiscoverMs = nowMs;
  static bool sMdnsUp = false;
  if (!sMdnsUp) sMdnsUp = MDNS.begin(gClientId);
  IPAddress ip;
  uint16_t pt = 0;
  int n = MDNS.queryService("mqtt", "tcp");
  if (n > 0) {
    ip = MDNS.address(0);
    pt = MDNS.port(0);
  } else {
    n = MDNS.queryService("home-assistant", "tcp");
    if (n > 0) {
      ip = MDNS.address(0);
      pt = 1883;
    }
  }
  if (n <= 0) {
    IPAddress hip = MDNS.queryHost("homeassistant");
    if (hip != IPAddress(0, 0, 0, 0)) {
      ip = hip;
      pt = 1883;
      n = 1;
    }
  }
  if (n > 0 && ip != IPAddress(0, 0, 0, 0)) {
    char host[24];
    snprintf(host, sizeof(host), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    Serial.printf("[mqtt] mDNS discovered broker %s:%u\n", host, pt ? pt : 1883);
    mqtt_cfg::save(host, pt ? pt : 1883, "", "");
  }
}

void tryConnect(uint32_t nowMs) {
  if (!mqtt_cfg::hostSet() || gMqtt.connected() || nowMs - gLastConnectTryMs < gConnRetryMs) return;
  gLastConnectTryMs = nowMs;
  ++gAttempts;

  char host[64] = {}, user[48] = {}, pass[64] = {};
  uint16_t port = 1883;
  mqtt_cfg::current(host, sizeof(host), &port, user, sizeof(user), pass, sizeof(pass));
  gMqtt.setServer(host, port);

  Serial.printf("[mqtt] connecting to %s:%u as \"%s\"...\n", host, port, gClientId);
  const char* muser = user[0] ? user : nullptr;
  const char* mpass = pass[0] ? pass : nullptr;
  if (gMqtt.connect(gClientId, muser, mpass, gAvailTopic, 0, true, "offline")) {
    gMqtt.publish(gAvailTopic, "online", true);
    gMqtt.subscribe(kRemoteState);       // retained echo -> instant restore (#102)
    gMqtt.subscribe(kControllerStatus);  // cid bind
    gMqtt.subscribe(kAvailability);      // Controller liveness (LWT-backed)
    gMqtt.subscribe(kPresetRoster);      // live preset roster
    gMqtt.subscribe(kOutdoorTemp);       // top-bar Outside readout
    gMqtt.subscribe("slytherm/config/sensors");          // #117 roster
    gMqtt.subscribe(kFusionTopic);                        // #117 occupancy/dominant
    gMqtt.subscribe("slytherm/state/sensor/+/age");       // #117 staleness
    gMqtt.subscribe("slytherm/state/sensor/+/participating");
    gMqtt.subscribe("slytherm/sensors/+/state");          // #117 per-room temp
    gMqtt.subscribe("slytherm/sensors/+/presence");       // #117 presence
    gMqtt.subscribe(gOtaCheckTopic);     // #111 OTA drive (per-Remote)
    gMqtt.subscribe(gOtaApplyTopic);
    { char t[48];  // #123: retained boot/crash telemetry (built once at boot)
      snprintf(t, sizeof(t), "slytherm/remote/%s/boot", gId);
      gMqtt.publish(t, boot_guard::statusJson(), true); }
    gOtaStateCache[0] = 0;               // republish OTA status after (re)connect
    // #111: broker connectivity = the post-OTA self-test (the Controller is
    // deliberately NOT required — OTA must work on a Remote with no
    // Controller on the network). Confirms a pending update; else no-op.
    ota::noteSelfTestPass();
    gConnRetryMs = kConnMinMs;           // backoff resets on success
    Serial.println("[mqtt] connected; subscribed to controller echo/status/roster");
  } else {
    gConnRetryMs = gConnRetryMs + gConnRetryMs / 2;
    if (gConnRetryMs > kConnMaxMs) gConnRetryMs = kConnMaxMs;
    Serial.printf("[mqtt] connect failed, state=%d\n", gMqtt.state());
  }
}

// Pop queued UiIntents (the UI applied them optimistically already) and
// publish each to slytherm/remote/<id>/intent. Vacation/ack intents have no
// #104 wire type yet — logged and dropped rather than silently banked (the
// docs/11 "NO intent queuing" rule).
void pumpIntents() {
  if (!gMqtt.connected() || gModel == nullptr) return;
  for (;;) {
    UiIntent it;
    bool have = false;
    char presetName[kUiPresetNameLen] = {};
    L();
    have = gModel->popIntent(it);
    if (have && it.type == IntentType::kSetPreset) {
      const auto& st = gModel->state();
      const int idx = static_cast<int>(it.preset);
      if (idx >= 0 && idx < st.presetCount) strlcpy(presetName, st.presets[idx].name, sizeof(presetName));
    }
    U();
    if (!have) break;

    std::string j;
    switch (it.type) {
      case IntentType::kSetSetpoints: j = rl::intentSetpointsJson(gIntentId + 1, it.heatC, it.coolC); break;
      case IntentType::kSetMode: {
        rl::Mode m = rl::Mode::kOff;
        if (it.mode == UserMode::kHeat || it.mode == UserMode::kEmergencyHeat) m = rl::Mode::kHeat;
        else if (it.mode == UserMode::kCool) m = rl::Mode::kCool;
        else if (it.mode == UserMode::kAuto) m = rl::Mode::kHeatCool;
        j = rl::intentModeJson(gIntentId + 1, m);
        break;
      }
      case IntentType::kSetPreset:
        if (presetName[0] == '\0') { Serial.println("[link] preset intent with no roster match, dropped"); continue; }
        j = rl::intentPresetJson(gIntentId + 1, presetName);
        break;
      case IntentType::kSetHold: {
        rl::Hold h = rl::Hold::kNone;
        switch (it.hold) {
          case HoldType::kUntilNextPreset: h = rl::Hold::kUntilNextPreset; break;
          case HoldType::kTwoHours: h = rl::Hold::kTwoHours; break;
          case HoldType::kFourHours: h = rl::Hold::kFourHours; break;
          case HoldType::kIndefinite: h = rl::Hold::kIndefinite; break;
          default: break;
        }
        if (h == rl::Hold::kNone) continue;
        j = rl::intentHoldJson(gIntentId + 1, h);
        break;
      }
      case IntentType::kClearHold: j = rl::intentClearHoldJson(gIntentId + 1); break;
      case IntentType::kSetVacation:  // #118
        j = rl::intentVacationJson(gIntentId + 1, it.vacStartDays, it.vacNights,
                                   it.heatC, it.coolC);
        break;
      case IntentType::kClearVacation: j = rl::intentClearVacationJson(gIntentId + 1); break;  // #118
      case IntentType::kAckAlarms: j = rl::intentAckAlarmsJson(gIntentId + 1); break;          // #118
      default:
        // Every user-visible intent must round-trip or not exist (#118) —
        // reaching this is a bug in whoever added a new IntentType.
        Serial.printf("[link] BUG: intent type %d has no wire form, dropped\n", static_cast<int>(it.type));
        continue;
    }

    ++gIntentId;
    gPrefs.putUInt("iid", gIntentId);  // survive reboot (Controller dedupes across our reboots)
    gLastIntentMs = millis();          // open the #103 echo-suppression window
    gMqtt.publish(gIntentTopic, j.c_str());
    Serial.printf("[link] intent #%lu -> %s: %s\n", static_cast<unsigned long>(gIntentId), gIntentTopic, j.c_str());
  }
}

}  // namespace

void attachModel(UiModel* model, SemaphoreHandle_t mux) {
  gModel = model;
  gModelMux = mux;
}

void begin() {
  deriveIds();
  Serial.printf("[mqtt] client id: %s\n", gClientId);
  gPrefs.begin("rlink", false);
  gIntentId = gPrefs.getUInt("iid", 0);
  gMqtt.setBufferSize(1200);  // remote/state + roster payloads exceed the 256 default
  gMqtt.setCallback(onMessage);
  mqtt_cfg::begin("", 0, "", "");
}

void loop() {
  if (!wifi_prov::connected()) return;

  const uint32_t nowMs = millis();
  discoverBroker(nowMs);

  if (mqtt_cfg::takeDirty()) {
    gMqtt.disconnect();
  }

  if (gMqtt.connected()) {
    gMqtt.loop();
    pumpIntents();
    // #117: batch row updates into the model at 1 Hz max.
    static uint32_t sLastRowsMs = 0;
    if (gRowsDirty && nowMs - sLastRowsMs >= 1000) {
      sLastRowsMs = nowMs;
      gRowsDirty = false;
      publishRowsToModel();
    }
    // #111: live OTA client status, ~1 Hz, diff-suppressed. NOT retained
    // (a stale "downloading" after reboot would mislead; the cache reset
    // on connect republishes the current state instead).
    static uint32_t sLastOtaPubMs = 0;
    if (nowMs - sLastOtaPubMs >= 1000) {
      sLastOtaPubMs = nowMs;
      const ota::Status os = ota::status();
      const std::string j = hm::otaStateJson(ota::toString(os.state),
                                             os.progressPct, SLYTHERM_FW_VERSION,
                                             os.available, os.error);
      if (strncmp(gOtaStateCache, j.c_str(), sizeof(gOtaStateCache) - 1) != 0) {
        strlcpy(gOtaStateCache, j.c_str(), sizeof(gOtaStateCache));
        gMqtt.publish(gOtaStateTopic, j.c_str());
      }
    }
  } else {
    tryConnect(nowMs);
  }
}

bool connected() { return gMqtt.connected(); }
bool controllerOnline() { return gControllerOnline; }

void toggleSensorParticipation(const char* displayName) {
  // #119: the shared UI hands us the DISPLAY name; map back to the roster id
  // and publish the inverse of the current state. No optimistic flip — the
  // button renders from the echoed per-sensor participating topic (#117), so
  // a lost publish self-corrects instead of lying.
  if (displayName == nullptr || !gMqtt.connected()) return;
  for (uint8_t i = 0; i < gRowCount; ++i) {
    if (strncmp(gRows[i].name, displayName, sizeof(gRows[i].name)) != 0) continue;
    char t[64];
    snprintf(t, sizeof(t), "slytherm/cmd/sensor/%s/participating", gRows[i].id);
    gMqtt.publish(t, gRows[i].participating ? "OFF" : "ON");
    Serial.printf("[link] participation %s -> %s\n", gRows[i].id,
                  gRows[i].participating ? "OFF" : "ON");
    return;
  }
}
uint32_t attempts() { return gAttempts; }

}  // namespace remote_mqtt
