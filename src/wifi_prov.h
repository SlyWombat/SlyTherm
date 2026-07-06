// wifi_prov.h — on-device Wi-Fi provisioning for the wall unit (issue #37).
// One owner of the Wi-Fi radio (the MQTT task): it maintains the connection
// from NVS-saved credentials and processes scan/connect requests posted by the
// UI task. The UI never touches the radio directly. Compiled only into
// env:thermostat_s3 (DETTSON_UI).
#pragma once

#include <cstddef>
#include <cstdint>

namespace wifi_prov {

struct Net { char ssid[33] = {}; int8_t rssi = 0; bool locked = false; };

enum class ScanState : uint8_t { kIdle, kScanning, kDone };
enum class ConnState : uint8_t { kIdle, kConnecting, kConnected, kFailed };

constexpr int kMaxNets = 16;

// ---- radio-owner side (call from the MQTT task) ----
// Load saved creds (falling back to the compile-time secrets if NVS is empty)
// and bring Wi-Fi up. Returns true if any SSID is configured.
bool begin(const char* fallbackSsid, const char* fallbackPass);
// True if a Wi-Fi SSID has ever been saved to NVS. Safe to call before begin()
// (reads NVS directly) — used for the first-run onboarding gate (issue #82).
bool hasSavedCredentials();
// Drive connection maintenance + process pending scan/connect requests.
void service(uint32_t nowMs);
bool connected();

// ---- UI side (thread-safe; posts requests, reads results) ----
void      requestScan();
ScanState scanState();
int       scanResults(Net* out, int maxN);   // number copied
void      requestConnect(const char* ssid, const char* pass);  // persists on success
ConnState connState();
void      forget();                            // clear saved creds + disconnect
// Current connection for the status line.
void status(char* ssidOut, size_t ssidN, char* ipOut, size_t ipN,
            int8_t* rssiOut, bool* isConnected);

}  // namespace wifi_prov
