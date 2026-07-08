// SlyTherm Remote entrypoint (issue #94 epic; #100 skeleton).
// Wall-mounted ESP32-P4 node: NO CT-485/RS-485, NO furnace demand pipeline —
// suppression IS this file never constructing those tasks (same convention as
// sniffer-vs-thermostat mains). See docs/11-remote-node-plan.md.
//
// Task layout: WiFi + MQTT service from the Arduino loop task; the wall UI
// (the full shared slytherm_ui stack, #101/#114) runs in its own pinned task,
// mirroring the Controller's uiTask. The UiModel here renders a DEMO
// DisplayState with hasBus=false (proves every tab draws on the P4 panel,
// Diag shows no RS-485 UI) — the real Controller-fed replica arrives with
// #102/#103.

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

#include <esp_ota_ops.h>

#include "UiModel.h"
#include "boot_guard.h"       // #122/#123: reset-loop latch + crash telemetry
#include "coredump_server.h"  // #124: LAN coredump pull (:8082)
#include "ota_client.h"  // #111: no-op inlines unless -DSLYTHERM_OTA
#include "remote_mqtt.h"
#include "remote_net_guard.h"
#include "slytherm_ui.h"
#include "telnet_log.h"       // #126: live logs on :23 (ring-replay on connect)
#include "ui/ui_port.h"
#include "wifi_prov.h"        // #121: wifi_prov owns the radio (NVS + on-device setup)

// Dev builds may carry a compiled-in seed; production images ship secretless
// and provision on-glass (#121). Same pattern as thermostat_secrets.h.
#if __has_include("remote_secrets.h")
#include "remote_secrets.h"
#else
#define REMOTE_WIFI_SSID ""
#define REMOTE_WIFI_PASS ""
#endif

using namespace dettson;
using namespace dettson::ui;

// #113: injected by tools/version_flag.py; fallback keeps ad-hoc builds compiling.
#ifndef SLYTHERM_FW_BUILD
#define SLYTHERM_FW_BUILD "0.0.0-dev"
#endif

// #111/#62: the OTA reboot gate. The Remote has no furnace — an update
// applies the moment it is verified (docs/10 §7: remote-p4 is ungated).
extern "C" bool otaSafeToReboot() { return true; }

namespace {

UiModel gUi;
SemaphoreHandle_t gUiMux = nullptr;
bool gFirstRun = false;  // #121: no WiFi config anywhere -> Welcome onboarding
Preferences gPrefs;
bool gClock24 = false;  // top-bar clock format (12h default); persisted in NVS "clk24"

// Demo DisplayState (#101 acceptance): static-but-plausible content so every
// tab has something real to draw. Replaced by the Controller echo in #102.
void fillDemoState() {
  // #108: fused/outdoor stay INVALID at boot so the #92 splash genuinely
  // gates on the Controller link (echo -> fused temp, state/outdoor_temp ->
  // outdoor). Only the sensor rows remain placeholder until #105-107.
  gUi.setSetpoints(20.0f, 24.0f);
  gUi.setUserMode(UserMode::kOff);
  gUi.setHvacAction(HvacAction::kIdle);
  // #117: NO demo sensor rows — the Sensors tab renders "No room sensor
  // reporting" truthfully until the Controller's retained roster/sensor
  // topics arrive (seconds after MQTT connect).
  DisplayState::PresetView pv[3] = {};
  strlcpy(pv[0].name, "home", sizeof(pv[0].name));  pv[0].heatC = 20.0f; pv[0].coolC = 24.0f;
  strlcpy(pv[1].name, "away", sizeof(pv[1].name));  pv[1].heatC = 16.0f; pv[1].coolC = 28.0f;
  strlcpy(pv[2].name, "sleep", sizeof(pv[2].name)); pv[2].heatC = 18.0f; pv[2].coolC = 22.0f;
  gUi.setPresets(pv, 3);
  gUi.setActivePreset("home");
}

// Wall-UI task (core 0), mirroring the Controller's uiTask: renders gUi and
// routes touch into it. Never touches the network.
void uiTask(void*) {
  // #122: >=3 consecutive abnormal boots -> the shared reduced safe screen.
  slytherm_ui::begin(&gUi, gUiMux, /*reducedUi=*/boot_guard::reducedUi(), gFirstRun);
  for (;;) {
    slytherm_ui::service();
    remote_net_guard::service();  // #109 blocking panel (lv top layer)
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

}  // namespace

// ---- extern "C" hooks the shared UI expects (#100) ----
// Clock format is real (persisted like the Controller's). The sensor toggle
// will forward a UiIntent to the Controller in #102; the RS-485 sniffer hooks
// are dead code on a busless Remote (hasBus=false hides the LISTEN UI) but
// must link.
extern "C" void uiToggleClock24() { gClock24 = !gClock24; gPrefs.putBool("clk24", gClock24); }
extern "C" bool uiClock24() { return gClock24; }
extern "C" void uiToggleSensor(const char* name) { remote_mqtt::toggleSensorParticipation(name); }  // #119
extern "C" void uiSniffStart() {}
extern "C" void uiSniffStop() {}
extern "C" bool uiSniffActive() { return false; }
extern "C" uint32_t uiSniffFrames() { return 0; }
extern "C" int uiSniffLines(char[10][56]) { return 0; }
extern "C" void uiClearReducedMode() { boot_guard::clearLatch(); ESP.restart(); }  // #122
extern "C" void uiNoteTouch() {}                 // #90 sleep state is Controller-side

void setup() {
  Serial.begin(115200);
  // Give the native USB-CDC link a moment to enumerate before the first print.
  delay(1500);
  Serial.println();
  Serial.println("=== SlyTherm Remote (ESP32-P4) boot ===");
  Serial.println("fw " SLYTHERM_FW_BUILD);  // #113: VERSION file + git sha
  Serial.printf("Chip: %s rev v%d.%d, %d MB flash, PSRAM %s\n",
                ESP.getChipModel(), ESP.getChipRevision() / 100,
                ESP.getChipRevision() % 100, ESP.getFlashChipSize() / (1024 * 1024),
                psramFound() ? "OK" : "NOT FOUND");

  boot_guard::begin("bootg");  // #122/#123: count this boot + reason/coredump

  gPrefs.begin("remote", false);
  gClock24 = gPrefs.getBool("clk24", false);

  // #64/#111: dual-app check + pending-update validation (arms the self-test
  // that remote_mqtt confirms on broker connect; rolls back a crash-looping
  // new image). No-ops without -DSLYTHERM_OTA / no pending update.
  { const esp_partition_t* run = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    Serial.printf("[boot] OTA %s: running=%s next=%s\n",
                  next ? "capable" : "NOT capable (single-app table)",
                  run ? run->label : "?", next ? next->label : "none"); }
  ota::bootValidate();

  gUiMux = xSemaphoreCreateMutex();
  gUi.setHasBus(false);   // #101: busless persona — no RS-485/CT-485 UI
  // #120: restore the screen lock — a reboot must never be a lock bypass
  // (restored state comes back LOCKED; corrupt/missing blob fails OPEN).
  { UiModel::LockPersistBlob lb;
    if (gPrefs.getBytes("lock", &lb, sizeof(lb)) == sizeof(lb))
      gUi.restoreLock(&lb, millis() / 1000); }
  // Pre-link placeholder only: the #102 link overwrites setpoints/mode/hold/
  // preset/fused-temp from the Controller's retained echo within seconds of
  // MQTT connecting. Sensor rows/outdoor stay demo until their feed lands.
  fillDemoState();

  telnet_log::begin();  // #126: live logs on :23, ring-replays recent history

  // #121: wifi_prov owns the radio — NVS creds (already seeded on fielded
  // units) win, the compiled-in secret is a dev fallback, and NO credentials
  // at all boots the first-run Welcome -> on-glass WiFi setup flow (#82).
  const bool haveWifiCfg = wifi_prov::begin(REMOTE_WIFI_SSID, REMOTE_WIFI_PASS);
  gFirstRun = !haveWifiCfg;
  if (gFirstRun) Serial.println("[wifi] unprovisioned - booting Welcome/WiFi setup");
  remote_mqtt::attachModel(&gUi, gUiMux);  // #102: echo -> model, intents -> broker
  remote_mqtt::begin();

  xTaskCreatePinnedToCore(uiTask, "ui", 24576, nullptr, 1, nullptr, 0);

  // #111: OTA task (checks the GitHub catalog; reboot ungated on the Remote).
  ota::begin();
}

void loop() {
  wifi_prov::service(millis());  // #121: connection maintenance + UI scan/connect requests
  remote_mqtt::loop();
  telnet_log::poll();       // #126
  coredump_server::poll();  // #124: LAN coredump pull (:8082)

  // Feed link health + wall clock into the model (the UI renders these live).
  // One-shot NTP once WiFi is up (same servers/TZ as the Controller, #69):
  // feeds the top-bar clock AND the OTA client's TLS cert validation (#111 —
  // an unsynced clock fails every X.509 date check).
  static bool sNtpUp = false;
  if (!sNtpUp && wifi_prov::connected()) {
    sNtpUp = true;
    configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");
  }

  static uint32_t lastTickMs = 0;
  const uint32_t nowMs = millis();
  if (nowMs - lastTickMs >= 1000) {
    lastTickMs = nowMs;
    // #109: run the degraded-UX guard on the link signals. brokerUp needs
    // BOTH wifi and an MQTT session; controllerUp is the availability LWT.
    remote_net_guard::feed(wifi_prov::connected() && remote_mqtt::connected(),
                           remote_mqtt::controllerOnline(),
                           wifi_prov::attempts() + remote_mqtt::attempts());
    xSemaphoreTake(gUiMux, portMAX_DELAY);
    gUi.setLinkHealth(wifi_prov::connected(), remote_mqtt::connected(), false,
                      wifi_prov::connected() ? static_cast<int8_t>(WiFi.RSSI()) : 0);  // #127
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
      char c[24];
      strftime(c, sizeof(c), gClock24 ? "%a  %H:%M" : "%a  %I:%M %p", &ti);
      gUi.setClock(c);
    }
    gUi.tick(nowMs / 1000);
    // #120: persist the lock on change (same shadow-compare discipline as the
    // Controller's saveLockBlob; human-rate writes, NVS wear negligible).
    static UiModel::LockPersistBlob sShadowLock{};
    UiModel::LockPersistBlob lb;
    gUi.saveLock(&lb);
    xSemaphoreGive(gUiMux);
    if (memcmp(&lb, &sShadowLock, sizeof(lb)) != 0) {
      sShadowLock = lb;
      gPrefs.putBytes("lock", &lb, sizeof(lb));
    }
    boot_guard::healthyTick(nowMs);  // #122: sustained uptime clears the latch
  }

  static uint32_t lastBeatMs = 0;
  if (nowMs - lastBeatMs >= 5000) {
    lastBeatMs = nowMs;
    telnet_log::logf("alive t=%lus heap=%u touch=%s wifi=%s ip=%s mqtt=%s", nowMs / 1000,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  slytherm_ui::portTouchOk() ? "OK" : "FAIL",
                  wifi_prov::connected() ? "OK" : "FAIL",
                  wifi_prov::connected() ? WiFi.localIP().toString().c_str() : "-",
                  remote_mqtt::connected() ? "OK" : "FAIL");
  }
  delay(2);
}
