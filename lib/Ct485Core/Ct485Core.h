// Ct485Core.h — shared CT-485 protocol definitions for the Dettson project.
//
// Single source of truth for constants and the frame struct. Values are
// reconstructed from kpishere/Net485 + kdschlosser/ClimateTalk and are
// documented in docs/02-protocol-climatetalk.md. Anything marked PROVISIONAL
// is single-sourced or disagrees between prior-art implementations and MUST
// be confirmed from real sniffed frames before any TX path trusts it
// (docs/02 §5a offset warning).
//
// Pure C++17, no Arduino dependencies — used by host-side tests and firmware.

#pragma once
#include <cstdint>
#include <cstddef>

namespace ct485 {

// ---------- Physical layer (docs/02 §1) ----------
constexpr uint32_t kBaudDefault        = 9600;    // confirm by sniffing; EcoNet variant uses 38400
constexpr uint32_t kBaudAlt            = 38400;
constexpr uint32_t kInterFrameGapUs    = 3500;    // >3.5 ms idle = frame boundary (no preamble)
constexpr uint32_t kInterPacketDelayUs = 100000;  // 100 ms idle required before TX
constexpr uint32_t kDePrePostUs        = 300;     // DE pre/post-drive hold (200-500 allowed)

// ---------- Network timing (docs/02 §6, Net485-derived) ----------
constexpr uint32_t kAnetSlotLoMs      = 6000;   // AutoNet Node-Discovery reply slot window:
constexpr uint32_t kAnetSlotHiMs      = 30000;  //  random 6-30 s (ANET_SLOTLO/ANET_SLOTHI)
constexpr uint32_t kResponseTimeoutMs = 3000;   // per-exchange RESPONSE_TIMEOUT
constexpr uint8_t  kMsgResendAttempts = 3;      // MSG_RESEND_ATTEMPTS (total transmissions)

// ---------- Frame geometry (docs/02 §2) ----------
constexpr size_t kHeaderLen   = 10;
constexpr size_t kMaxPayload  = 240;
constexpr size_t kChecksumLen = 2;
constexpr size_t kMaxFrame    = kHeaderLen + kMaxPayload + kChecksumLen;  // 252

// Header byte offsets
enum HeaderOffset : uint8_t {
  kOffDst         = 0,
  kOffSrc         = 1,
  kOffSubnet      = 2,
  kOffSendMethod  = 3,
  kOffSendParamHi = 4,  // for Set Control Command: the command code
  kOffSendParamLo = 5,
  kOffSrcNodeType = 6,
  kOffMsgType     = 7,
  kOffPacketNum   = 8,
  kOffPayloadLen  = 9,  // payload only; total frame = payloadLen + 12
};

// ---------- Addresses & subnets (docs/02 §6) ----------
constexpr uint8_t kAddrBroadcast   = 0x00;
constexpr uint8_t kAddrThermostat  = 0x01;  // thermostat/zone control always node 1
constexpr uint8_t kAddrVirtualSub  = 0xF1;
constexpr uint8_t kAddrArbitration = 0xFE;
constexpr uint8_t kAddrCoordinator = 0xFF;

constexpr uint8_t kSubnetBroadcast   = 0x00;
constexpr uint8_t kSubnetMaintenance = 0x01;
constexpr uint8_t kSubnetV1          = 0x02;
constexpr uint8_t kSubnetV2          = 0x03;

// ---------- Node types (docs/02 §6) ----------
enum class NodeType : uint8_t {
  kThermostat     = 0x01,
  kGasFurnace     = 0x02,  // the Chinook IFC
  kAirHandler     = 0x03,
  kAirConditioner = 0x04,
  kHeatPump       = 0x05,  // open question: K03085 interface board may enumerate as this...
  kElectricFurnace= 0x06,
  kCrossover      = 0x09,  // ...or as crossover/OBBI (docs/02 §8) — never assume
  kUnitaryControl = 0x0C,
  kZoneControl    = 0x15,
  kCoordinator    = 0xA6,
};

// ---------- Message types, header offset 7 (docs/02 §4). Response = request | 0x80 ----------
enum class MsgType : uint8_t {
  kR2R              = 0x00,  // request-to-receive / token poll (dataflow bit set)
  kGetConfig        = 0x01,
  kGetStatus        = 0x02,
  kSetControlCmd    = 0x03,  // the write channel (demands, mode, setpoints)
  kSetDisplayMsg    = 0x04,
  kSetDiagnostics   = 0x05,
  kGetDiagnostics   = 0x06,  // response payload = null-separated fault strings
  kGetSensorData    = 0x07,  // TLV (db_id, len, data); OAT expected at MDI id 0 (confirm)
  kSetIdentification= 0x0D,
  kGetIdentification= 0x0E,
  kDmaRead          = 0x1D,  // raw MDI table read: best telemetry harvest channel
  kDmaReadMotor     = 0x1E,
  kSetMfgGeneric    = 0x1F,
  kGetMfgGeneric    = 0x20,
  kGetUserMenu      = 0x41,
  kUpdateUserMenu   = 0x42,
  kEcho             = 0x5A,
  kNetworkStateReq  = 0x75,
  kTokenOffer       = 0x77,
  kVersionAnnounce  = 0x78,
  kNodeDiscovery    = 0x79,
  kSetAddress       = 0x7A,
  kGetNodeId        = 0x7B,
};
constexpr uint8_t kResponseFlag = 0x80;

// Packet-number byte (offset 8) bit fields
constexpr uint8_t kPktNumDataflowBit = 0x80;  // set on R2R/ACK
constexpr uint8_t kPktNumVersionBit  = 0x20;  // 1 = CT-485 v1.0, 0 = v2.0
constexpr uint8_t kPktNumChunkMask   = 0x1F;

// ---------- Control command codes, header offset 4 (docs/02 §5a) ----------
enum class Command : uint8_t {
  kHeatSetPointModify = 0x01,
  kCoolSetPointModify = 0x02,
  kSystemSwitchModify = 0x05,  // payload: see SystemSwitch below; persistent, NO refresh timer
  kFanKeySelection    = 0x07,
  kSetPointTempHold   = 0x47,
  kDehumSetPoint      = 0x5D,
  kHumSetPoint        = 0x5E,
  kDamperPosition     = 0x60,
  kSubsystemBusy      = 0x61,  // coordinator->stat readiness handshake after operator-
                               // initiated transitions; value 0 = not busy (field-observed
                               // 2026-07-08/09; named per kdschlosser/ClimateTalk)
  kDehumDemand        = 0x62,
  kHumDemand          = 0x63,
  kHeatDemand         = 0x64,  // gas heat capacity request; Chinook valid band 40-100% (+0)
  kCoolDemand         = 0x65,
  kFanDemand          = 0x66,
  kBackupHeatDemand   = 0x67,
  kDefrostDemand      = 0x68,
  kAuxHeatDemand      = 0x69,
  kSetMotorSpeed      = 0x6A,
  kSetMotorTorque     = 0x6B,
  kSetAirflowDemand   = 0x6C,
  kSetMotorTorquePct  = 0x70,
};

// SYSTEM_SWITCH_MODIFY values
enum class SystemSwitch : uint8_t {
  kOff        = 0x00,
  kCool       = 0x01,
  kAuto       = 0x02,
  kHeat       = 0x03,
  kBackupHeat = 0x04,  // maps to our EMERGENCY_HEAT (gas-only)
};

// PROVISIONAL demand payload layout for Set Control Command (0x03) frames
// (docs/02 §5a OFFSET WARNING — kdschlosser write path says timer at [13],
// demand at [14]; its read path and earlier notes say [12]/[13]. Single-sourced;
// resolve from real captures, then delete the wrong variant.)
constexpr size_t kDemandCmdEchoOffset    = 10;  // 16-bit command code echo, little-endian
constexpr size_t kDemandTimerOffsetVarA  = 12;  // variant A: [12]=refresh timer, [13]=demand
constexpr size_t kDemandValueOffsetVarA  = 13;
constexpr size_t kDemandTimerOffsetVarB  = 13;  // variant B: [13]=refresh timer, [14]=demand
constexpr size_t kDemandValueOffsetVarB  = 14;
// Demand byte = percent * 2 (0-200 = 0-100%).
// Refresh-timer byte: high nibble = minutes (0-15), low nibble = 3.75 s units.

// ---------- ACK/NAK codes carried in response payloads (docs/02 §4) ----------
constexpr uint8_t kAck1 = 0x06;  // valid control command
constexpr uint8_t kAck2 = 0x0A;  // undesired parameter
constexpr uint8_t kAck3 = 0x0D;  // minimum params incomplete
constexpr uint8_t kNak1 = 0x15;  // bad CRC
constexpr uint8_t kNak2 = 0x1B;  // invalid for that application (pairing/ownership rejection)

// ---------- Parsed frame ----------
struct Frame {
  uint8_t dst         = 0;
  uint8_t src         = 0;
  uint8_t subnet      = 0;
  uint8_t sendMethod  = 0;
  uint8_t sendParamHi = 0;
  uint8_t sendParamLo = 0;
  uint8_t srcNodeType = 0;
  uint8_t msgType     = 0;
  uint8_t packetNum   = 0;
  uint8_t payloadLen  = 0;
  uint8_t payload[kMaxPayload] = {};

  bool isResponse() const { return (msgType & kResponseFlag) != 0; }
  uint8_t baseMsgType() const { return msgType & static_cast<uint8_t>(~kResponseFlag); }
  size_t totalLen() const { return kHeaderLen + payloadLen + kChecksumLen; }
};

}  // namespace ct485
