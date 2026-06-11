// main_sniffer.cpp — Phase 1 RX-only CT-485 bus sniffer (issue #6).
// Bench rig: ESP32-DevKitC + 3.3 V RS-485 transceiver in permanent receive,
// tapped in PARALLEL with the existing thermostat (docs/02 §8 methodology,
// docs/05 Phase 1).
//
// ============================ RX-ONLY GUARANTEE =============================
// This firmware is physically incapable of driving the bus:
//   - UART2 is opened with txPin = -1 (sniffer::kTxPin): no GPIO is ever
//     attached to the UART TX signal.
//   - No DE/RE GPIO is configured or touched anywhere in this file; the
//     transceiver's DE pin is strapped LOW (RE# low) in hardware.
//   - Serial2 is never written to. The only TX path is USB (Serial / UART0),
//     which is not connected to the RS-485 transceiver.
// Do not add any of the above to this file. TX belongs to Phase 3 firmware
// only, after the docs/05 Phase 2 field-dictionary gate.
// ============================================================================
//
// Behavior:
//   - Non-blocking byte pump; micros()-based inter-byte gap measurement.
//     The first byte after a >= 3.5 ms (ct485::kInterFrameGapUs) idle gap
//     starts a new frame; bytes feed ct485::FrameAccumulator (lib/Ct485Frame).
//   - Auto-baud: alternates 9600/38400 on a dwell timer until one baud
//     clearly wins on Fletcher-valid frame count; reports the chosen baud.
//   - One line per valid frame on USB serial (115200): millis timestamp,
//     preceding gap in us, raw hex, decoded dst/src/msgType (+ Set-Control
//     command byte when msgType == 0x03).
//   - 10 s heartbeat with byte/frame/error counters and current baud — the
//     near-silent-bus diagnostic (docs/05 Phase 1 caveat).
//   - Optional MQTT frame streaming only under -DSNIFFER_MQTT (default OFF:
//     no network dependency, no credentials in the default build).
//
// Timing caveat: gaps are measured when bytes are DRAINED from the UART
// driver ring, not on the wire. The pump therefore keeps the loop fast
// (buffered USB TX, no blocking calls); if the loop ever stalls past one
// gap time, adjacent frames merge and are counted as badLength rather than
// silently corrupting data — the 2 KiB driver ring guarantees no byte loss.

#include <Arduino.h>

#include "Ct485Core.h"
#include "Ct485Frame.h"
#include "sniffer_config.h"

#ifdef SNIFFER_MQTT
#include <WiFi.h>
#include <PubSubClient.h>
#endif

namespace {

ct485::FrameAccumulator gAcc;

// Gap / frame bookkeeping (drain-time measurement, see caveat above).
uint32_t gLastByteUs     = 0;
bool     gHaveInProgress = false;   // bytes accumulated since the last gap
uint32_t gCurFrameGapUs  = 0;       // gap that preceded the in-progress frame
uint32_t gCurFrameStartMs = 0;

// Auto-baud.
constexpr uint32_t kBauds[2] = {ct485::kBaudDefault, ct485::kBaudAlt};
uint8_t  gBaudIdx       = 0;
bool     gBaudLocked    = false;
uint32_t gBaudDwellMs   = 0;        // dwell start (millis)
uint32_t gValidAtBaud[2] = {0, 0};

// Counters for the heartbeat.
uint32_t gTotalBytes      = 0;
uint32_t gLastHeartbeatMs = 0;

char gLine[1200];                   // worst case: 252-byte frame -> ~920 chars

#ifdef SNIFFER_MQTT
WiFiClient   gWifiClient;
PubSubClient gMqtt(gWifiClient);
uint32_t     gLastMqttAttemptMs = 0;

void mqttPump(uint32_t nowMs) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!gMqtt.connected()) {
    // Rate-limit (re)connect attempts; PubSubClient::connect blocks briefly.
    if (nowMs - gLastMqttAttemptMs < sniffer::kMqttReconnectMs) return;
    gLastMqttAttemptMs = nowMs;
    gMqtt.connect(sniffer::kMqttClientId, SNIFFER_MQTT_USER, SNIFFER_MQTT_PASS);
    return;
  }
  gMqtt.loop();
}

void mqttPublishLine(const char* line) {
  if (gMqtt.connected()) gMqtt.publish(sniffer::kMqttTopicFrames, line);
}
#else
inline void mqttPublishLine(const char*) {}
#endif

const char* msgTypeName(uint8_t base) {
  using ct485::MsgType;
  switch (static_cast<MsgType>(base)) {
    case MsgType::kR2R:               return "R2R";
    case MsgType::kGetConfig:         return "GetConfig";
    case MsgType::kGetStatus:         return "GetStatus";
    case MsgType::kSetControlCmd:     return "SetCtrlCmd";
    case MsgType::kSetDisplayMsg:     return "SetDispMsg";
    case MsgType::kSetDiagnostics:    return "SetDiag";
    case MsgType::kGetDiagnostics:    return "GetDiag";
    case MsgType::kGetSensorData:     return "GetSensor";
    case MsgType::kSetIdentification: return "SetIdent";
    case MsgType::kGetIdentification: return "GetIdent";
    case MsgType::kDmaRead:           return "DMARead";
    case MsgType::kDmaReadMotor:      return "DMAReadMot";
    case MsgType::kSetMfgGeneric:     return "SetMfg";
    case MsgType::kGetMfgGeneric:     return "GetMfg";
    case MsgType::kGetUserMenu:       return "GetUserMenu";
    case MsgType::kUpdateUserMenu:    return "UpdUserMenu";
    case MsgType::kEcho:              return "Echo";
    case MsgType::kNetworkStateReq:   return "NetState";
    case MsgType::kTokenOffer:        return "TokenOffer";
    case MsgType::kVersionAnnounce:   return "VerAnnounce";
    case MsgType::kNodeDiscovery:     return "NodeDisc";
    case MsgType::kSetAddress:        return "SetAddr";
    case MsgType::kGetNodeId:         return "GetNodeId";
    default:                          return "?";
  }
}

const char* commandName(uint8_t cmd) {
  using ct485::Command;
  switch (static_cast<Command>(cmd)) {
    case Command::kHeatSetPointModify: return "HEAT_SETPOINT";
    case Command::kCoolSetPointModify: return "COOL_SETPOINT";
    case Command::kSystemSwitchModify: return "SYS_SWITCH";
    case Command::kFanKeySelection:    return "FAN_KEY";
    case Command::kSetPointTempHold:   return "SETPOINT_HOLD";
    case Command::kHeatDemand:         return "HEAT_DEMAND";
    case Command::kCoolDemand:         return "COOL_DEMAND";
    case Command::kFanDemand:          return "FAN_DEMAND";
    case Command::kBackupHeatDemand:   return "BACKUP_HEAT_DEMAND";
    case Command::kDefrostDemand:      return "DEFROST_DEMAND";
    case Command::kAuxHeatDemand:      return "AUX_HEAT_DEMAND";
    default:                           return "?";
  }
}

void emitFrameLine(const uint8_t* raw, size_t len, uint32_t gapUs, uint32_t tMs) {
  size_t pos = static_cast<size_t>(
      snprintf(gLine, sizeof(gLine), "F t=%lums gap=%luus baud=%lu len=%u |",
               static_cast<unsigned long>(tMs),
               static_cast<unsigned long>(gapUs),
               static_cast<unsigned long>(kBauds[gBaudIdx]),
               static_cast<unsigned>(len)));
  for (size_t i = 0; i < len && pos + 4 < sizeof(gLine); ++i) {
    pos += static_cast<size_t>(
        snprintf(gLine + pos, sizeof(gLine) - pos, " %02X", raw[i]));
  }

  // FrameAccumulator only completes Fletcher-valid frames, so decode is
  // expected to succeed; guard anyway.
  ct485::Frame f;
  if (ct485::decode(raw, len, f) && pos + 1 < sizeof(gLine)) {
    pos += static_cast<size_t>(snprintf(
        gLine + pos, sizeof(gLine) - pos,
        " | dst=%02X src=%02X type=%02X(%s%s)",
        f.dst, f.src, f.msgType, msgTypeName(f.baseMsgType()),
        f.isResponse() ? " rsp" : ""));
    if (f.baseMsgType() == static_cast<uint8_t>(ct485::MsgType::kSetControlCmd) &&
        !f.isResponse() && pos + 1 < sizeof(gLine)) {
      // Set-Control command code rides in the header (offset 4).
      snprintf(gLine + pos, sizeof(gLine) - pos, " cmd=%02X(%s)",
               f.sendParamHi, commandName(f.sendParamHi));
    }
  }

  Serial.println(gLine);
  mqttPublishLine(gLine);
}

void emitHeartbeat(uint32_t nowMs) {
  const ct485::FrameAccumulator::Counters& c = gAcc.counters();
  snprintf(gLine, sizeof(gLine),
           "HB t=%lums baud=%lu(%s) bytes=%lu ok=%lu badcrc=%lu badlen=%lu "
           "overrun=%lu v%lu=%lu v%lu=%lu",
           static_cast<unsigned long>(nowMs),
           static_cast<unsigned long>(kBauds[gBaudIdx]),
           gBaudLocked ? "locked" : "scanning",
           static_cast<unsigned long>(gTotalBytes),
           static_cast<unsigned long>(c.framesOk),
           static_cast<unsigned long>(c.badChecksum),
           static_cast<unsigned long>(c.badLength),
           static_cast<unsigned long>(c.overruns),
           static_cast<unsigned long>(kBauds[0]),
           static_cast<unsigned long>(gValidAtBaud[0]),
           static_cast<unsigned long>(kBauds[1]),
           static_cast<unsigned long>(gValidAtBaud[1]));
  Serial.println(gLine);
}

void maybeLockBaud() {
  if (gBaudLocked) return;
  const uint32_t mine  = gValidAtBaud[gBaudIdx];
  const uint32_t other = gValidAtBaud[gBaudIdx ^ 1];
  if (mine >= sniffer::kAutobaudLockFrames &&
      mine >= other * sniffer::kAutobaudLockRatio) {
    gBaudLocked = true;
    snprintf(gLine, sizeof(gLine), "AB locked baud=%lu (valid %lu vs %lu)",
             static_cast<unsigned long>(kBauds[gBaudIdx]),
             static_cast<unsigned long>(mine),
             static_cast<unsigned long>(other));
    Serial.println(gLine);
  }
}

void autobaudPump(uint32_t nowMs) {
  if (gBaudLocked) return;
  if (nowMs - gBaudDwellMs < sniffer::kAutobaudDwellMs) return;
  if (gHaveInProgress) return;  // never re-baud mid-frame
  gBaudIdx ^= 1;
  gBaudDwellMs = nowMs;
  Serial2.updateBaudRate(kBauds[gBaudIdx]);  // keeps RX-only pin config
  gAcc.reset();                              // drop cross-baud partials; counters kept
  snprintf(gLine, sizeof(gLine), "AB trying baud=%lu",
           static_cast<unsigned long>(kBauds[gBaudIdx]));
  Serial.println(gLine);
}

void onFrameComplete(uint32_t gapUs, uint32_t tMs) {
  gValidAtBaud[gBaudIdx]++;
  emitFrameLine(gAcc.frame(), gAcc.frameLen(), gapUs, tMs);
  maybeLockBaud();
}

}  // namespace

void setup() {
  Serial.setTxBufferSize(sniffer::kUsbTxBufBytes);
  Serial.begin(sniffer::kUsbBaud);

  // RX-only: txPin = -1, no GPIO ever drives the transceiver (see header).
  Serial2.setRxBufferSize(sniffer::kUartRxBufBytes);
  Serial2.begin(kBauds[gBaudIdx], SERIAL_8N1, sniffer::kRxPin, sniffer::kTxPin);

  gLastByteUs     = micros();
  gBaudDwellMs    = millis();
  gLastHeartbeatMs = millis();

  Serial.println();
  Serial.println("Dettson CT-485 sniffer (Phase 1, RX-ONLY) — UART2 RX=GPIO16, no TX pin");
  snprintf(gLine, sizeof(gLine), "AB trying baud=%lu",
           static_cast<unsigned long>(kBauds[gBaudIdx]));
  Serial.println(gLine);

#ifdef SNIFFER_MQTT
  WiFi.mode(WIFI_STA);
  WiFi.begin(SNIFFER_WIFI_SSID, SNIFFER_WIFI_PASS);
  gMqtt.setServer(SNIFFER_MQTT_HOST, SNIFFER_MQTT_PORT);
  gMqtt.setBufferSize(sniffer::kMqttBufBytes);
#endif
}

void loop() {
  // ---- Byte pump (non-blocking) ----
  int avail = Serial2.available();
  while (avail-- > 0) {
    const int c = Serial2.read();
    if (c < 0) break;
    const uint32_t nowUs = micros();
    const uint32_t gapUs = nowUs - gLastByteUs;
    gLastByteUs = nowUs;
    gTotalBytes++;

    const bool gapBefore =
        !gHaveInProgress || gapUs >= ct485::kInterFrameGapUs;
    // feed() closes the PREVIOUS frame when gapBefore is set, so report it
    // with the gap/time captured at ITS first byte before rolling those over.
    if (gAcc.feed(static_cast<uint8_t>(c), gapBefore)) {
      onFrameComplete(gCurFrameGapUs, gCurFrameStartMs);
    }
    if (gapBefore) {
      gCurFrameGapUs   = gapUs;
      gCurFrameStartMs = millis();
    }
    gHaveInProgress = true;
  }

  // ---- Idle close-out: bus quiet >= one gap ends the in-progress frame ----
  if (gHaveInProgress &&
      static_cast<uint32_t>(micros() - gLastByteUs) >= ct485::kInterFrameGapUs) {
    gHaveInProgress = false;
    if (gAcc.flush()) onFrameComplete(gCurFrameGapUs, gCurFrameStartMs);
  }

  const uint32_t nowMs = millis();
  autobaudPump(nowMs);

  if (nowMs - gLastHeartbeatMs >= sniffer::kHeartbeatMs) {
    gLastHeartbeatMs = nowMs;
    emitHeartbeat(nowMs);
  }

#ifdef SNIFFER_MQTT
  mqttPump(nowMs);
#endif
}
