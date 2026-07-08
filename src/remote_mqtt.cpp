#include "remote_mqtt.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PubSubClient.h>

#include "ota_client.h"  // #111: no-op inlines unless -DSLYTHERM_OTA
#include <WiFi.h>

#include <cstring>
#include <vector>

#include "HaMqtt.h"        // parsePresetRosterJson (pure, shared with the Controller)
#include "RemoteLink.h"    // #102 codec: echo/status parsers + intent builders
#include "mqtt_cfg.h"
#include "remote_wifi.h"

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
    gMqtt.subscribe(gOtaCheckTopic);     // #111 OTA drive (per-Remote)
    gMqtt.subscribe(gOtaApplyTopic);
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
      default:
        Serial.printf("[link] intent type %d has no wire form yet, dropped\n", static_cast<int>(it.type));
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
  if (!remote_wifi::connected()) return;

  const uint32_t nowMs = millis();
  discoverBroker(nowMs);

  if (mqtt_cfg::takeDirty()) {
    gMqtt.disconnect();
  }

  if (gMqtt.connected()) {
    gMqtt.loop();
    pumpIntents();
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
uint32_t attempts() { return gAttempts; }

}  // namespace remote_mqtt
