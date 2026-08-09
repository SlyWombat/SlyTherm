// matter_glue.cpp — see matter_glue.h. Epic #179 phase 2.
#ifdef SLYTHERM_MATTER
#include "matter_glue.h"

#include <Arduino.h>
#include <Matter.h>
#include <cmath>
#include <cstring>

#include <platform/ESP32/ESP32Utils.h>              // chip WiFi provisioning store
#include <platform/ESP32/NetworkCommissioningDriver.h>  // ESPWiFiDriver network list
#include <platform/internal/DeviceNetworkInfo.h>

#include "HaMqtt.h"  // hm::Mode numeric values (single source, docs/06)

namespace hm = dettson::hamqtt;  // same alias main_thermostat.cpp uses

namespace matter_glue {
namespace {

MatterThermostat gThermostat;
CommandFn gOnCommand = nullptr;
bool gBegun = false;
char gManualCode[16] = "";
char gQrPayload[48] = "";

// Outbound change-compare state (control-task only).
float gLastTempC = NAN, gLastHeatC = NAN, gLastCoolC = NAN;
int gLastMode = -1;

// The library's single-sided setpoint writes use 0xffff as "leave alone";
// echoes of it must never reach the control pipeline.
constexpr double kSetpointSentinel = 0xffff;

bool toHmMode(MatterThermostat::ThermostatMode_t m, uint8_t& out) {
  switch (m) {
    case MatterThermostat::THERMOSTAT_MODE_OFF:  out = static_cast<uint8_t>(hm::Mode::kOff);      return true;
    case MatterThermostat::THERMOSTAT_MODE_HEAT: out = static_cast<uint8_t>(hm::Mode::kHeat);     return true;
    case MatterThermostat::THERMOSTAT_MODE_COOL: out = static_cast<uint8_t>(hm::Mode::kCool);     return true;
    case MatterThermostat::THERMOSTAT_MODE_AUTO: out = static_cast<uint8_t>(hm::Mode::kHeatCool); return true;
    // EMERGENCY_HEAT: hm::Mode has no em-heat member (it is a separate switch,
    // #163); the honest nearest write is HEAT. The rest have no SlyTherm
    // meaning — reject so the ecosystem sees the attribute revert.
    case MatterThermostat::THERMOSTAT_MODE_EMERGENCY_HEAT:
      out = static_cast<uint8_t>(hm::Mode::kHeat);
      return true;
    default:
      return false;
  }
  return false;  // unreachable; satisfies -Werror=return-type on the enum switch
}

MatterThermostat::ThermostatMode_t fromHmMode(uint8_t hmMode, bool emHeat) {
  if (emHeat) return MatterThermostat::THERMOSTAT_MODE_EMERGENCY_HEAT;
  switch (static_cast<hm::Mode>(hmMode)) {
    case hm::Mode::kHeat:     return MatterThermostat::THERMOSTAT_MODE_HEAT;
    case hm::Mode::kCool:     return MatterThermostat::THERMOSTAT_MODE_COOL;
    case hm::Mode::kHeatCool: return MatterThermostat::THERMOSTAT_MODE_AUTO;
    default:                  return MatterThermostat::THERMOSTAT_MODE_OFF;
  }
}

void* gBleReserve = nullptr;

}  // namespace

void reserveBleRam() {
  if (gBleReserve) return;
  gBleReserve = heap_caps_malloc(70 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  Serial.printf("[matter] BLE reserve %s\r\n", gBleReserve ? "held (70K)" : "FAILED");
}

void begin(CommandFn onCommand, const char* wifiSsid, const char* wifiPass) {
  if (gBegun) return;
  gOnCommand = onCommand;

  Serial.printf("[matter] begin: heap=%u largest=%u\r\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  gThermostat.begin(MatterThermostat::THERMOSTAT_SEQ_OP_COOLING_HEATING,
                    MatterThermostat::THERMOSTAT_AUTO_MODE_ENABLED);
  Serial.println("[matter] endpoint up, starting stack");

  // Echo suppression in every callback: publishState()'s own attribute writes
  // fire these too (observed at first boot: the outbound 18/27 mirror came
  // straight back as "commands"). A value that matches what WE last pushed is
  // our own echo, not an ecosystem write — swallow it, or every local change
  // would spuriously stamp the inbound-command clock via gPending.
  gThermostat.onChangeMode([](MatterThermostat::ThermostatMode_t m) {
    if (!gBegun) return true;  // attribute restoration during stack start, not a command
    Command c;
    if (!toHmMode(m, c.hmMode)) {
      Serial.printf("[matter] mode %d unsupported — rejected\r\n", (int)m);
      return false;
    }
    if (static_cast<int>(m) == gLastMode) return true;  // our own mirror echo
    c.hasMode = true;
    if (gOnCommand) gOnCommand(c);
    Serial.printf("[matter] cmd mode -> %s\r\n",
                  MatterThermostat::getThermostatModeString(m));
    return true;
  });
  gThermostat.onChangeHeatingSetpoint([](double c16) {
    if (!gBegun) return true;  // attribute restoration during stack start, not a command
    if (c16 >= kSetpointSentinel) return true;  // "leave alone" sentinel
    if (!std::isnan(gLastHeatC) && fabsf(static_cast<float>(c16) - gLastHeatC) < 0.05f)
      return true;  // our own mirror echo
    Command c;
    c.hasHeatSp = true;
    c.heatC = static_cast<float>(c16);
    if (gOnCommand) gOnCommand(c);
    Serial.printf("[matter] cmd heat sp -> %.1fC\r\n", c16);
    return true;
  });
  gThermostat.onChangeCoolingSetpoint([](double c16) {
    if (!gBegun) return true;  // attribute restoration during stack start, not a command
    if (c16 >= kSetpointSentinel) return true;
    if (!std::isnan(gLastCoolC) && fabsf(static_cast<float>(c16) - gLastCoolC) < 0.05f)
      return true;  // our own mirror echo
    Command c;
    c.hasCoolSp = true;
    c.coolC = static_cast<float>(c16);
    if (gOnCommand) gOnCommand(c);
    Serial.printf("[matter] cmd cool sp -> %.1fC\r\n", c16);
    return true;
  });

  if (gBleReserve) {  // hand the boot-time contiguous block to NimBLE
    free(gBleReserve);
    gBleReserve = nullptr;
    Serial.printf("[matter] BLE reserve released: largest=%u\r\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  }
  // Seed the chip stack's WiFi stores with the station's credentials BEFORE
  // start — see the header note on why an "unconfigured" ConnectivityManager
  // kills the Arduino-managed connection otherwise. BOTH stores matter: the
  // legacy station provision (ESP32Utils) and the NetworkCommissioning
  // driver's network list — the "Failed to get configured network" saboteur
  // reads the LATTER (bench 2026-08-09: legacy-only seeding didn't stop it).
  if (wifiSsid && wifiSsid[0]) {
    chip::DeviceLayer::Internal::DeviceNetworkInfo ni = {};
    strlcpy(ni.WiFiSSID, wifiSsid, sizeof(ni.WiFiSSID));
    const size_t keyLen = wifiPass ? strnlen(wifiPass, sizeof(ni.WiFiKey)) : 0;
    if (keyLen) memcpy(ni.WiFiKey, wifiPass, keyLen);
    ni.WiFiKeyLen = static_cast<uint8_t>(keyLen);
    const CHIP_ERROR e = chip::DeviceLayer::Internal::ESP32Utils::SetWiFiStationProvision(ni);

    auto& drv = chip::DeviceLayer::NetworkCommissioning::ESPWiFiDriver::GetInstance();
    char dbg[64];
    chip::MutableCharSpan dbgSpan(dbg);
    uint8_t netIdx = 0;
    const auto st = drv.AddOrUpdateNetwork(
        chip::ByteSpan(reinterpret_cast<const uint8_t*>(wifiSsid), strlen(wifiSsid)),
        chip::ByteSpan(reinterpret_cast<const uint8_t*>(wifiPass ? wifiPass : ""), keyLen),
        dbgSpan, netIdx);
    const CHIP_ERROR e2 = drv.CommitConfiguration();
    Serial.printf("[matter] chip wifi provision: legacy=%s netcomm=%d commit=%s\r\n",
                  e == CHIP_NO_ERROR ? "ok" : e.AsString(),
                  static_cast<int>(st), e2 == CHIP_NO_ERROR ? "ok" : e2.AsString());
  }
  Matter.begin();
  Serial.printf("[matter] stack up: heap=%u\r\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  gBegun = true;

  strlcpy(gManualCode, Matter.getManualPairingCode().c_str(), sizeof(gManualCode));
  // The QR *payload* ("MT:...") is what phone apps scan; the library exposes it
  // embedded in a render-me URL — extract the data= tail.
  String url = Matter.getOnboardingQRCodeUrl();
  int at = url.indexOf("data=");
  strlcpy(gQrPayload, at >= 0 ? url.c_str() + at + 5 : url.c_str(), sizeof(gQrPayload));

  if (!Matter.isDeviceCommissioned()) {
    Serial.printf("[matter] NOT commissioned — code %s / %s\r\n", gManualCode, gQrPayload);
  } else {
    Serial.println("[matter] already commissioned");
  }
}

void publishState(float fusedC, bool fusedValid, uint8_t hmMode, bool emHeat,
                  float heatSpC, float coolSpC) {
  if (!gBegun) return;
  if (fusedValid && (std::isnan(gLastTempC) || fabsf(fusedC - gLastTempC) >= 0.05f)) {
    gThermostat.setLocalTemperature(fusedC);
    gLastTempC = fusedC;
  }
  // Cache-before-set, every setter: the onChange callbacks fire SYNCHRONOUSLY
  // inside these calls, and their echo check compares against the cache — set
  // it after and the first mirror loops straight back in as a command
  // (observed: boot-time 18/27 "commands" survived the NaN check).
  const MatterThermostat::ThermostatMode_t m = fromHmMode(hmMode, emHeat);
  if (static_cast<int>(m) != gLastMode) {
    gLastMode = static_cast<int>(m);
    gThermostat.setMode(m);
  }
  // One dual write when either side moved: setCoolingHeatingSetpoints keeps the
  // cluster's heat<cool deadband invariant in a single transaction instead of
  // two single-sided writes racing it.
  if (std::isnan(gLastHeatC) || fabsf(heatSpC - gLastHeatC) >= 0.05f ||
      std::isnan(gLastCoolC) || fabsf(coolSpC - gLastCoolC) >= 0.05f) {
    gLastHeatC = heatSpC;
    gLastCoolC = coolSpC;
    gThermostat.setCoolingHeatingSetpoints(heatSpC, coolSpC);
  }
}

bool commissioned() { return gBegun && Matter.isDeviceCommissioned(); }
const char* manualCode() { return gManualCode; }
const char* qrPayload() { return gQrPayload; }

}  // namespace matter_glue
#endif  // SLYTHERM_MATTER
