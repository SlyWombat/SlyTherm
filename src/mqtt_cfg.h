// mqtt_cfg.h — on-device MQTT broker provisioning for the wall unit (issue #22).
// The broker host/port/credentials are set on the wall UI (Settings -> Server)
// and saved in NVS, so the unit is pointed at Home Assistant's broker without a
// reflash. The MQTT task owns the connection: it reads this config at boot and
// re-applies whenever the UI saves a change. Compiled only into
// env:thermostat_s3 (DETTSON_UI).
#pragma once

#include <cstddef>
#include <cstdint>

namespace mqtt_cfg {

// Load saved broker config from NVS, falling back to the compile-time secrets
// if NVS is empty. Returns true if a host is configured. Call once (MQTT task).
bool begin(const char* fbHost, uint16_t fbPort, const char* fbUser, const char* fbPass);

// Current active config (for the UI to display + prefill, and the MQTT task to
// apply). Any out pointer may be null.
void current(char* host, size_t hostN, uint16_t* port,
             char* user, size_t userN, char* pass, size_t passN);

// UI: persist a new config (NVS) and flag it for the MQTT task to re-apply.
void save(const char* host, uint16_t port, const char* user, const char* pass);

// MQTT task: returns true once after each save() (then clears the flag).
bool takeDirty();

bool hostSet();               // is a broker host configured?
void setConnected(bool c);    // MQTT task publishes link state for the UI
bool connected();

}  // namespace mqtt_cfg
