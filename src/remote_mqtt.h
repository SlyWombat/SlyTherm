// P4 MQTT link (issues #98 bring-up, #102/#103 Remote<->Controller protocol).
// Owns the broker connection (reusing mqtt_cfg's NVS config + the Controller's
// mDNS auto-discovery), the Remote's LWT, and the private link:
//   - consumes slytherm/remote/state (retained authoritative echo) ->
//     reconciles the attached UiModel (Controller wins; a ~2s suppression
//     window after a local intent keeps an in-flight echo from yanking a
//     setpoint mid-adjust, #103),
//   - consumes slytherm/config/presets -> live preset roster,
//   - consumes slytherm/controller/status (cid bind) + slytherm/availability
//     (Controller liveness),
//   - publishes popped UiIntents to slytherm/remote/<id>/intent with an
//     NVS-persisted monotonic id (the Controller dedupes by id ACROSS our
//     reboots — a counter restarting at 1 would be muted).
#pragma once

#include "UiModel.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace remote_mqtt {

// Wire the shared UiModel in BEFORE begin(). `model`/`mux` outlive this
// module; all model access is serialized by `mux` (the UI task renders it).
void attachModel(dettson::ui::UiModel* model, SemaphoreHandle_t mux);

// Call once from setup() (after remote_wifi::begin()).
void begin();

// Call every loop(). Handles discovery, (re)connect, inbound state, and
// outbound intents.
void loop();

bool connected();          // broker link up
bool controllerOnline();   // slytherm/availability == online (LWT-backed)

}  // namespace remote_mqtt
