// P4 Wi-Fi bring-up over the onboard ESP32-C6 hosted link (issue #96).
// #96 scope only: associate + get an IP, compiled-in credentials
// (remote_secrets.h). Real Remotes provision on-device via the touchscreen
// (#100/#101, reusing the Controller's wifi_prov module) -- this is a P0
// bring-up shortcut, not the final mechanism.
#pragma once

#include <cstdint>

namespace remote_wifi {

// Starts the hosted-WiFi association (non-blocking). Call once from setup().
void begin();

// Services connection state. Call every loop().
void loop();

bool connected();

// Cumulative connect attempts since boot (resets never; the #109 guard
// diffs it across an outage). Retry cadence backs off 10s -> 60s while
// disconnected and snaps back on success.
uint32_t attempts();

}  // namespace remote_wifi
