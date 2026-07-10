// remote_vpn.cpp — see remote_vpn.h. Compiled only into env:remote_p4_vpn.

#include "remote_vpn.h"

#ifdef SLYTHERM_WG

#include <Arduino.h>
#include <WiFi.h>
#include <ctime>

#include <esp_wireguard.h>

#include "telnet_log.h"

#if __has_include("remote_secrets.h")
#include "remote_secrets.h"
#endif
// Unconfigured builds compile but no-op (same pattern as the WiFi fallback).
#ifndef SLYTHERM_WG_PRIVKEY
#define SLYTHERM_WG_PRIVKEY ""
#endif
#ifndef SLYTHERM_WG_PEER_PUBKEY
#define SLYTHERM_WG_PEER_PUBKEY ""
#endif
#ifndef SLYTHERM_WG_PSK
#define SLYTHERM_WG_PSK ""
#endif
#ifndef SLYTHERM_WG_ENDPOINT
#define SLYTHERM_WG_ENDPOINT ""
#endif
#ifndef SLYTHERM_WG_PORT
#define SLYTHERM_WG_PORT 51820
#endif
#ifndef SLYTHERM_WG_ADDRESS
#define SLYTHERM_WG_ADDRESS "10.20.30.2"  // this device inside the tunnel
#endif
#ifndef SLYTHERM_WG_MASK
#define SLYTHERM_WG_MASK "255.255.255.0"
#endif
// The home LAN. Doubles as (a) the route pushed through the tunnel and
// (b) the "am I already home?" test that gates escalation/teardown (#149).
#ifndef SLYTHERM_WG_ROUTE_IP
#define SLYTHERM_WG_ROUTE_IP "192.168.10.0"
#endif
#ifndef SLYTHERM_WG_ROUTE_MASK
#define SLYTHERM_WG_ROUTE_MASK "255.255.255.0"
#endif

namespace remote_vpn {
namespace {

// #149: sustained direct-path failure before the tunnel comes up. Long
// enough that ordinary broker blips (15 s keepalive, reconnect backoff)
// never escalate; well inside the net-guard's 5 min blocking panel.
constexpr uint32_t kEscalateMs = 60u * 1000u;

wireguard_config_t gCfg = ESP_WIREGUARD_CONFIG_DEFAULT();
wireguard_ctx_t gCtx = ESP_WIREGUARD_CONTEXT_DEFAULT();
bool gInited = false;     // esp_wireguard_init done (once ever)
bool gEscalated = false;  // connect() issued; WG stack retrying handshakes
bool gUp = false;         // peer handshake current
bool gRouted = false;     // allowed-ip added for THIS connect cycle
bool gDefaulted = false;  // set_default applied (undo on teardown)
volatile bool gRetryReq = false;  // posted by the UI task
uint32_t gDirectFailMs = 0;  // when wifi-up-but-broker-down began (0 = n/a)
uint32_t gLastPollMs = 0;
bool gClockWaitLogged = false;

bool configured() { return SLYTHERM_WG_ENDPOINT[0] && SLYTHERM_WG_PRIVKEY[0]; }

bool onHomeLan() {  // WiFi IP inside the routed home subnet -> no tunnel
  IPAddress net, mask;
  if (!net.fromString(SLYTHERM_WG_ROUTE_IP) ||
      !mask.fromString(SLYTHERM_WG_ROUTE_MASK))
    return false;
  const uint32_t ip = (uint32_t)WiFi.localIP();
  return (ip & (uint32_t)mask) == ((uint32_t)net & (uint32_t)mask);
}

bool initOnce() {
  if (gInited) return true;
  gCfg.private_key = SLYTHERM_WG_PRIVKEY;
  gCfg.public_key = SLYTHERM_WG_PEER_PUBKEY;
  gCfg.preshared_key = SLYTHERM_WG_PSK[0] ? SLYTHERM_WG_PSK : NULL;
  gCfg.address = SLYTHERM_WG_ADDRESS;
  gCfg.netmask = SLYTHERM_WG_MASK;
  gCfg.endpoint = SLYTHERM_WG_ENDPOINT;
  gCfg.port = SLYTHERM_WG_PORT;
  gCfg.persistent_keepalive = 25;  // holds any NAT on the away-network path
  const esp_err_t e = esp_wireguard_init(&gCfg, &gCtx);
  if (e != ESP_OK) { telnet_log::logf("[wg] init failed: %d", (int)e); return false; }
  gInited = true;
  return true;
}

void escalate() {
  if (gEscalated || !initOnce()) return;
  const esp_err_t e = esp_wireguard_connect(&gCtx);
  if (e != ESP_OK) { telnet_log::logf("[wg] connect failed: %d", (int)e); return; }
  gEscalated = true;
  telnet_log::logf("[wg] direct path down %us - escalating to tunnel (%s:%d as %s)",
                   (unsigned)(kEscalateMs / 1000u), SLYTHERM_WG_ENDPOINT,
                   (int)SLYTHERM_WG_PORT, SLYTHERM_WG_ADDRESS);
}

void teardown(const char* why) {
  if (!gEscalated) return;
  if (gDefaulted) { esp_wireguard_restore_default(&gCtx); gDefaulted = false; }
  esp_wireguard_disconnect(&gCtx);
  gEscalated = false; gUp = false;
  gRouted = false;  // disconnect zeroes the peer; re-add on the next cycle
  telnet_log::logf("[wg] tunnel down (%s) - direct mode", why);
}

}  // namespace

void service(uint32_t nowMs, bool wifiUp, bool mqttUp) {
  if (!configured()) return;

  if (gRetryReq) {  // Settings-page VPN button (#148 UI)
    gRetryReq = false;
    telnet_log::log("[wg] manual retry requested");
    if (gEscalated) { teardown("manual retry"); escalate(); }
    else gDirectFailMs = (nowMs > kEscalateMs) ? nowMs - kEscalateMs : 1;  // skip grace
  }

  if (!wifiUp) {  // no radio: ladder resets; tunnel can't live anyway
    gDirectFailMs = 0;
    if (gEscalated) teardown("wifi down");
    return;
  }

  if (!gEscalated) {  // ---- direct mode (#149 kStandby) ----
    if (mqttUp || onHomeLan()) {
      // Direct path healthy, or we're on the home LAN where a tunnel can't
      // help (broker itself down) and would only loop traffic through WAN.
      gDirectFailMs = 0;
      return;
    }
    if (gDirectFailMs == 0) { gDirectFailMs = nowMs ? nowMs : 1; return; }
    if (nowMs - gDirectFailMs < kEscalateMs) return;
    if (time(nullptr) < 1600000000) {  // WG handshakes need a real clock
      if (!gClockWaitLogged) { gClockWaitLogged = true;
        telnet_log::log("[wg] escalation ready but clock not synced - waiting on NTP"); }
      return;
    }
    escalate();
    return;
  }

  // ---- escalated: drive peer state (~2 s cadence) ----
  if (nowMs - gLastPollMs < 2000) return;
  gLastPollMs = nowMs;

  if (onHomeLan()) { teardown("back on home LAN"); gDirectFailMs = 0; return; }

  const bool up = (esp_wireguard_peer_is_up(&gCtx) == ESP_OK);
  if (up && !gUp) {
    gUp = true;
    telnet_log::log("[wg] tunnel UP");
    if (!gRouted) {
      // Cryptokey filter entry for home-LAN sources (input + peer lookup;
      // NOT lwIP routing — echo replies exit the input netif, which is why
      // ping tests pass without this ever being consulted for output).
      if (esp_wireguard_add_allowed_ip(&gCtx, SLYTHERM_WG_ROUTE_IP,
                                       SLYTHERM_WG_ROUTE_MASK) == ESP_OK) {
        gRouted = true;
        telnet_log::logf("[wg] allowed-ip added: %s/%s", SLYTHERM_WG_ROUTE_IP,
                         SLYTHERM_WG_ROUTE_MASK);
      } else {
        telnet_log::log("[wg] allowed-ip add FAILED - home LAN unreachable");
      }
    }
    // Arduino's prebuilt lwIP has no policy-routing hook: off-subnet
    // destinations only enter the tunnel if the wg netif is the default.
    if (esp_wireguard_set_default(&gCtx) == ESP_OK) {
      gDefaulted = true;
      telnet_log::log("[wg] default route -> tunnel");
    } else {
      telnet_log::log("[wg] set_default FAILED");
    }
  } else if (!up && gUp) {
    gUp = false;
    telnet_log::log("[wg] tunnel lost (rehandshaking)");
  }
}

bool up() { return gUp; }

State state() {
  if (!configured()) return State::kDisabled;
  if (!gEscalated) return State::kStandby;
  return gUp ? State::kUp : State::kHandshaking;
}

void requestRetry() { gRetryReq = true; }

}  // namespace remote_vpn

#endif  // SLYTHERM_WG
