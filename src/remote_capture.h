// remote_capture.h — #181: who-changed-it audit capture on the camera Remote.
// Compiled only under SLYTHERM_CAM (env:remote_p4_vpn).
//
// Every user change intent on this panel (setpoint/mode/preset/hold/vacation —
// the UiModel intent observer, post screen-lock) queues an event here. A
// low-priority task photographs the person at the panel via the #150 camera
// pipeline and POSTs JPEG + metadata to a LAN receiver on kdocker2 (the SlyLog
// capture-receiver, slylog/capture-receiver/). Owner decision on #181: this
// is a security feature standing in for a per-user PIN system, so it
// deliberately ignores the MQTT camera privacy switch (which gates the live
// :8080 view only).
//
// Failure posture: never block or reject a change. Camera down / paused for
// OTA / encoder busy -> the event still POSTs, metadata-only. Receiver
// unreachable -> log and drop (the Controller's history remains the
// authoritative record of every change).
#pragma once

#include "UiModel.h"

namespace remote_capture {

// Allocate the PSRAM JPEG buffer and start the capture/POST task. Call after
// remote_camera::begin(). Safe to call once; no-op on allocation failure
// (events are then dropped with a log line).
void begin();

// Queue an audit event. Called from the UiModel intent observer — UI task,
// model mutex held — so it only copies + xQueueSend(0 ticks). presetName is
// the roster name for preset intents ("" otherwise). Non-capture intent
// types (alarm ack, OTA) are filtered here.
void noteIntent(const dettson::ui::UiIntent& intent, const char* presetName);

// Receiver base URL (e.g. "http://192.168.10.12:8093/capture"), MQTT-driven
// via retained slytherm/cmd/capture_url (same pattern as ota_mirror),
// persisted in NVS. "" -> back to the compiled default; "off" -> disable.
void setUrl(const char* url);

}  // namespace remote_capture
