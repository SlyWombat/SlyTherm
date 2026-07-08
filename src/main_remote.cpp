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

#include "UiModel.h"
#include "remote_mqtt.h"
#include "remote_net_guard.h"
#include "remote_wifi.h"
#include "slytherm_ui.h"
#include "ui/ui_port.h"

using namespace dettson;
using namespace dettson::ui;

namespace {

UiModel gUi;
SemaphoreHandle_t gUiMux = nullptr;
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
  SensorRow rows[2] = {};
  strlcpy(rows[0].name, "Wall unit", sizeof(rows[0].name));
  rows[0].tempC = 21.5f; rows[0].participating = true; rows[0].healthy = true; rows[0].dominant = true; rows[0].occupied = true;
  strlcpy(rows[1].name, "Bedroom", sizeof(rows[1].name));
  rows[1].tempC = 20.1f; rows[1].participating = true; rows[1].healthy = true; rows[1].lastOccAgeS = 5400;
  gUi.setSensorRows(rows, 2);
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
  slytherm_ui::begin(&gUi, gUiMux, /*reducedUi=*/false, /*firstRun=*/false);
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
extern "C" void uiToggleSensor(const char*) {}   // #102: forward to Controller
extern "C" void uiSniffStart() {}
extern "C" void uiSniffStop() {}
extern "C" bool uiSniffActive() { return false; }
extern "C" uint32_t uiSniffFrames() { return 0; }
extern "C" int uiSniffLines(char[10][56]) { return 0; }
extern "C" void uiClearReducedMode() { ESP.restart(); }  // no reset-loop latch here yet
extern "C" void uiNoteTouch() {}                 // #90 sleep state is Controller-side

void setup() {
  Serial.begin(115200);
  // Give the native USB-CDC link a moment to enumerate before the first print.
  delay(1500);
  Serial.println();
  Serial.println("=== SlyTherm Remote (ESP32-P4) boot ===");
  Serial.printf("Build: %s %s\n", __DATE__, __TIME__);
  Serial.printf("Chip: %s rev v%d.%d, %d MB flash, PSRAM %s\n",
                ESP.getChipModel(), ESP.getChipRevision() / 100,
                ESP.getChipRevision() % 100, ESP.getFlashChipSize() / (1024 * 1024),
                psramFound() ? "OK" : "NOT FOUND");

  gPrefs.begin("remote", false);
  gClock24 = gPrefs.getBool("clk24", false);

  gUiMux = xSemaphoreCreateMutex();
  gUi.setHasBus(false);   // #101: busless persona — no RS-485/CT-485 UI
  // Pre-link placeholder only: the #102 link overwrites setpoints/mode/hold/
  // preset/fused-temp from the Controller's retained echo within seconds of
  // MQTT connecting. Sensor rows/outdoor stay demo until their feed lands.
  fillDemoState();

  remote_wifi::begin();
  remote_mqtt::attachModel(&gUi, gUiMux);  // #102: echo -> model, intents -> broker
  remote_mqtt::begin();

  xTaskCreatePinnedToCore(uiTask, "ui", 24576, nullptr, 1, nullptr, 0);
}

void loop() {
  remote_wifi::loop();
  remote_mqtt::loop();

  // Feed link health + wall clock into the model (the UI renders these live).
  static uint32_t lastTickMs = 0;
  const uint32_t nowMs = millis();
  if (nowMs - lastTickMs >= 1000) {
    lastTickMs = nowMs;
    // #109: run the degraded-UX guard on the link signals. brokerUp needs
    // BOTH wifi and an MQTT session; controllerUp is the availability LWT.
    remote_net_guard::feed(remote_wifi::connected() && remote_mqtt::connected(),
                           remote_mqtt::controllerOnline(),
                           remote_wifi::attempts() + remote_mqtt::attempts());
    xSemaphoreTake(gUiMux, portMAX_DELAY);
    gUi.setLinkHealth(remote_wifi::connected(), remote_mqtt::connected(), false);
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
      char c[24];
      strftime(c, sizeof(c), gClock24 ? "%a  %H:%M" : "%a  %I:%M %p", &ti);
      gUi.setClock(c);
    }
    gUi.tick(nowMs / 1000);
    xSemaphoreGive(gUiMux);
  }

  static uint32_t lastBeatMs = 0;
  if (nowMs - lastBeatMs >= 5000) {
    lastBeatMs = nowMs;
    Serial.printf("alive t=%lus heap=%u touch=%s wifi=%s ip=%s mqtt=%s\n", nowMs / 1000,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  slytherm_ui::portTouchOk() ? "OK" : "FAIL",
                  remote_wifi::connected() ? "OK" : "FAIL",
                  remote_wifi::connected() ? WiFi.localIP().toString().c_str() : "-",
                  remote_mqtt::connected() ? "OK" : "FAIL");
  }
  delay(2);
}
