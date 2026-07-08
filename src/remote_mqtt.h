// P4 MQTT bring-up over the hosted WiFi link (issue #98). Proves PubSubClient
// works on this stack: connect (reusing mqtt_cfg's NVS config + the
// Controller's mDNS broker auto-discovery), an LWT, and a publish/subscribe
// round trip. NOT the real Remote<->Controller protocol yet (#102) -- that's
// DisplayState/UiIntent on `slytherm/remote/<id>/...`; this is just the P0
// "does the client work at all" gate.
#pragma once

namespace remote_mqtt {

// Call once from setup() (after remote_wifi::begin()).
void begin();

// Call every loop(). Handles discovery, (re)connect, and PubSubClient's own
// loop().
void loop();

bool connected();

}  // namespace remote_mqtt
