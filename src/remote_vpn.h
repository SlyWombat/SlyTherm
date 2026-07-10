// remote_vpn.h — #148/#149: WireGuard uplink for an off-LAN Remote
// (env:remote_p4_vpn only). #149 policy: the tunnel is a FALLBACK, not a
// default — the Remote always tries the direct broker path first, escalates
// to the tunnel only after a sustained direct failure with WiFi healthy
// (60 s — comfortably inside the net-guard's 5 min blocking threshold), and
// tears the tunnel down when it finds itself back on the home LAN. Keys and
// endpoint are baked via remote_secrets.h (owner decision 2026-07-10; the
// on-glass provisioning idea stays parked in #149).
#pragma once

#include <cstdint>

namespace remote_vpn {

// Drive the #149 ladder. Call every loop() with current link facts:
//   wifiUp - association state (wifi_prov::connected())
//   mqttUp - broker session state (remote_mqtt::connected())
// WireGuard init/connect happen lazily inside (handshakes need NTP; the
// escalation waits for a valid wall clock).
void service(uint32_t nowMs, bool wifiUp, bool mqttUp);

bool up();  // peer handshake current (tunnel carrying traffic)

// Settings-page status word. kDisabled = not configured / init failed;
// kStandby = direct mode, tunnel intentionally down (#149 normal-at-home);
// kHandshaking = escalated, trying; kUp = tunnel carrying traffic.
enum class State : uint8_t { kDisabled, kStandby, kHandshaking, kUp };
State state();

// UI-task safe: posts a request; service() executes it. In kStandby this
// forces an immediate escalation (skip the 60 s grace); in kHandshaking/kUp
// it forces a fresh disconnect+reconnect.
void requestRetry();

}  // namespace remote_vpn
