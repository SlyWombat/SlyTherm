// thermostat_config.h — build-time configuration for the thermostat firmware
// (src/main_thermostat.cpp, issue #55). Same role as sniffer_config.h: only
// firmware-rig knobs (pins, cadences, glue topics) belong here. Canonical
// control/protection DEFAULTS live in lib/DettsonConfig/DettsonConfig.h and
// the docs/05 table — never duplicate them here.
//
// Wi-Fi/MQTT credentials come from src/thermostat_secrets.h (gitignored;
// copy src/thermostat_secrets.h.example) — never from this file.

#pragma once
#include <cstddef>
#include <cstdint>

namespace thermostat {

// ---------- Pins (ESP32-DevKitC bench rig; -1 = not wired -> no-op) ----------
// CT-485 UART2 (used only under -DSLYTHERM_CT485_UART; matches the sniffer rig
// RX pin so the same transceiver wiring serves both firmwares).
// Waveshare ESP32-S3-Touch-LCD-4.3B onboard isolated RS-485 (auto-direction,
// no DE line) is on GPIO43(RX)/GPIO44(TX); DE = -1 -> no direction pin (#71).
constexpr int kCt485RxPin = 43;
constexpr int kCt485TxPin = 44;
constexpr int kCt485DePin = -1;   // -1 = auto-direction transceiver, no DE/RE

// External hardware watchdog pet line (TPL5010/MAX6369 class, docs/04 §3).
// -1 = not fitted (bench): petExternalWdt() verdicts are logged only.
constexpr int kWdtPetPin = -1;

// Case B relay outputs + blower-proof sense (used only under
// -DSLYTHERM_ACTUATOR_RELAY). -1 = log only. Relays are normally open,
// de-energized at boot (docs/04 §1b); HIGH = energized.
constexpr int kRelayY1Pin = -1;
constexpr int kRelayY2Pin = -1;
constexpr int kRelayObPin = -1;
constexpr int kRelayGPin  = -1;
constexpr int kSenseGPin  = -1;   // 24 V opto blower sense; -1 = no proof ->
                                  //  Y is never closed (docs/04 §2 coil freeze)
constexpr int kSenseDPin  = -1;   // D-wire defrost sense (observation only)

// Local DS18B20s (used only under -DSLYTHERM_DS18B20): indoor fallback +
// outdoor rung 2. Bench default is MQTT-only simulated sensors.
constexpr int kOneWirePin = -1;

// ---------- Task layout ----------
constexpr uint32_t kControlPeriodMs   = 1000;  // fixed-cadence control loop (core 1)
constexpr uint32_t kCt485TickMs       = 20;    // CT-485 task cadence (core 1)
constexpr uint32_t kMqttLoopMs        = 20;    // MQTT task cadence (core 0)
constexpr uint32_t kControlStack      = 8192;
constexpr uint32_t kCt485Stack        = 4096;
constexpr uint32_t kMqttStack         = 10240; // discovery JSON strings live here

// ---------- Network ----------
constexpr const char* kMqttClientId   = "slytherm-thermostat";
constexpr uint32_t kWifiRetryMs       = 15000;
constexpr uint32_t kMqttReconnectMs   = 5000;
constexpr uint16_t kMqttBufBytes      = 2048;  // climate discovery JSON > 1 KiB
constexpr uint32_t kStateHeartbeatS   = 60;    // full state republish cadence

// HA-weather bridge topic for the outdoor ladder's third rung (docs/06 topic
// map; OatRung::kHaWeather). Plausibility-gated -50..55 C on ingest.
constexpr const char* kOatTopic = "slytherm/cmd/outdoor_temp";
constexpr float kOatIngestMinC = -50.0f;
constexpr float kOatIngestMaxC = 55.0f;

// ---------- NVS ----------
constexpr const char* kNvsNamespace = "dettson";
constexpr uint32_t kClockSaveS      = 60;   // monotonic-clock base persist cadence
constexpr uint32_t kGuardSaveS      = 300;  // CompressorGuard blob persist cadence
                                            //  (also saved on every start/stop)

// ---------- Glue shaping knobs (NOT canonical defaults; promotion candidates
// for DettsonConfig.h + the docs/05 table once reviewed in the field) --------
// fan_mode "circulate": blower for the first N minutes of each hour
// (docs/06: duty-cycled FAN_DEMAND).
constexpr uint32_t kFanCirculateMinPerHour = 15;
// Staged-HP request map (HpRelayShaper input; docs/05 "PID->duty" — kept
// proportional, not PID, per the HP-paths-are-staged rule): request ramps
// from kHpDutyMinPct at zero error to 100% at kHpFullScaleErrC past setpoint.
constexpr float kHpDutyMinPct     = 40.0f;
constexpr float kHpFullScaleErrC  = 2.0f;
// DS18B20-only degraded mode demand cap (docs/04 §4 "demand capped").
constexpr float kDegradedGasCapPct = 60.0f;

// Default preset roster until the retained slytherm/config/presets arrives
// (docs/06 default home/away/sleep).
struct DefaultPreset { const char* name; float heatC; float coolC; };
constexpr DefaultPreset kDefaultPresets[] = {
    {"home", 21.0f, 25.0f}, {"away", 17.0f, 28.0f}, {"sleep", 19.0f, 26.0f}};

// ---------- Glue alarm codes (0x4Dxx = main glue; 0x53xx = SafetySupervisor) --
constexpr uint16_t kAlarmDegradedMode    = 0x4D01;  // DS18B20-only fallback active
constexpr uint16_t kAlarmOatFailCold     = 0x4D02;  // all OAT rungs stale
constexpr uint16_t kAlarmDemandConflict  = 0x4D03;  // DemandArbiter invariant latch
constexpr uint16_t kAlarmGasRuntime      = 0x4D04;  // gas 4 h max-runtime trip
constexpr uint16_t kAlarmMqttFallback    = 0x4D05;  // stale-MQTT fallback profile applied
constexpr uint16_t kAlarmBusTxStack      = 0x4D06;  // Ct485Thermostat pairing/comms/starvation

}  // namespace thermostat
