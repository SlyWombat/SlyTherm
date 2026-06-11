// Ct485Parser.cpp — see header. Pure C++17, bounded against lying lengths.

#include "Ct485Parser.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace ct485 {

namespace {

std::string fmt(const char* format, ...) {
  char buf[512];  // sized for the widest dictionary-table row
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  return buf;
}

const char* baseMsgTypeNameOrNull(uint8_t base) {
  switch (static_cast<MsgType>(base)) {
    case MsgType::kR2R:               return "R2R";
    case MsgType::kGetConfig:         return "GET_CONFIGURATION";
    case MsgType::kGetStatus:         return "GET_STATUS";
    case MsgType::kSetControlCmd:     return "SET_CONTROL_COMMAND";
    case MsgType::kSetDisplayMsg:     return "SET_DISPLAY_MESSAGE";
    case MsgType::kSetDiagnostics:    return "SET_DIAGNOSTICS";
    case MsgType::kGetDiagnostics:    return "GET_DIAGNOSTICS";
    case MsgType::kGetSensorData:     return "GET_SENSOR_DATA";
    case MsgType::kSetIdentification: return "SET_IDENTIFICATION";
    case MsgType::kGetIdentification: return "GET_IDENTIFICATION";
    case MsgType::kDmaRead:           return "DMA_READ";
    case MsgType::kDmaReadMotor:      return "DMA_READ_MOTOR";
    case MsgType::kSetMfgGeneric:     return "SET_MFG_GENERIC_DATA";
    case MsgType::kGetMfgGeneric:     return "GET_MFG_GENERIC_DATA";
    case MsgType::kGetUserMenu:       return "GET_USER_MENU";
    case MsgType::kUpdateUserMenu:    return "UPDATE_USER_MENU";
    case MsgType::kEcho:              return "ECHO";
    case MsgType::kNetworkStateReq:   return "NETWORK_STATE_REQUEST";
    case MsgType::kTokenOffer:        return "TOKEN_OFFER";
    case MsgType::kVersionAnnounce:   return "VERSION_ANNOUNCEMENT";
    case MsgType::kNodeDiscovery:     return "NODE_DISCOVERY";
    case MsgType::kSetAddress:        return "SET_ADDRESS";
    case MsgType::kGetNodeId:         return "GET_NODE_ID";
  }
  return nullptr;
}

const char* commandNameOrNull(uint8_t command) {
  switch (static_cast<Command>(command)) {
    case Command::kHeatSetPointModify: return "HEAT_SET_POINT_MODIFY";
    case Command::kCoolSetPointModify: return "COOL_SET_POINT_MODIFY";
    case Command::kSystemSwitchModify: return "SYSTEM_SWITCH_MODIFY";
    case Command::kFanKeySelection:    return "FAN_KEY_SELECTION";
    case Command::kSetPointTempHold:   return "SET_POINT_TEMP_AND_TEMPORARY_HOLD";
    case Command::kDehumSetPoint:      return "DEHUMIDIFICATION_SET_POINT_MODIFY";
    case Command::kHumSetPoint:        return "HUMIDIFICATION_SET_POINT_MODIFY";
    case Command::kDamperPosition:     return "DAMPER_POSITION_DEMAND";
    case Command::kDehumDemand:        return "DEHUMIDIFICATION_DEMAND";
    case Command::kHumDemand:          return "HUMIDIFICATION_DEMAND";
    case Command::kHeatDemand:         return "HEAT_DEMAND";
    case Command::kCoolDemand:         return "COOL_DEMAND";
    case Command::kFanDemand:          return "FAN_DEMAND";
    case Command::kBackupHeatDemand:   return "BACKUP_HEAT_DEMAND";
    case Command::kDefrostDemand:      return "DEFROST_DEMAND";
    case Command::kAuxHeatDemand:      return "AUX_HEAT_DEMAND";
    case Command::kSetMotorSpeed:      return "SET_MOTOR_SPEED";
    case Command::kSetMotorTorque:     return "SET_MOTOR_TORQUE";
    case Command::kSetAirflowDemand:   return "SET_AIRFLOW_DEMAND";
    case Command::kSetMotorTorquePct:  return "SET_MOTOR_TORQUE_PERCENT";
  }
  return nullptr;
}

const char* systemSwitchNameOrNull(uint8_t value) {
  switch (static_cast<SystemSwitch>(value)) {
    case SystemSwitch::kOff:        return "OFF";
    case SystemSwitch::kCool:       return "COOL";
    case SystemSwitch::kAuto:       return "AUTO";
    case SystemSwitch::kHeat:       return "HEAT";
    case SystemSwitch::kBackupHeat: return "BACKUP_HEAT";
  }
  return nullptr;
}

DemandCandidate readCandidate(const Frame& f, size_t payloadLen,
                              size_t timerFrameOff, size_t valueFrameOff) {
  DemandCandidate c;
  c.timerFrameOffset = timerFrameOff;
  c.valueFrameOffset = valueFrameOff;
  const size_t timerIdx = timerFrameOff - kHeaderLen;
  const size_t valueIdx = valueFrameOff - kHeaderLen;
  if (valueIdx >= payloadLen || timerIdx >= payloadLen) return c;  // valid stays false
  c.valid = true;
  c.timerRaw = f.payload[timerIdx];
  c.demandRaw = f.payload[valueIdx];
  c.demandPct = static_cast<float>(c.demandRaw) / 2.0f;
  c.timerMinutes = static_cast<uint8_t>(c.timerRaw >> 4);
  c.timerUnits = static_cast<uint8_t>(c.timerRaw & 0x0F);
  c.timerTotalS = c.timerMinutes * 60.0f + c.timerUnits * 3.75f;
  return c;
}

}  // namespace

std::string hexByte(uint8_t v) { return fmt("0x%02X", v); }

std::string msgTypeName(uint8_t msgType) {
  const uint8_t base = static_cast<uint8_t>(msgType & ~kResponseFlag);
  const char* n = baseMsgTypeNameOrNull(base);
  if (n == nullptr) return hexByte(msgType);
  if ((msgType & kResponseFlag) != 0) return std::string(n) + "_RESPONSE";
  return n;
}

std::string commandName(uint8_t command) {
  const char* n = commandNameOrNull(command);
  return n != nullptr ? std::string(n) : hexByte(command);
}

std::string systemSwitchName(uint8_t value) {
  const char* n = systemSwitchNameOrNull(value);
  return n != nullptr ? std::string(n) : hexByte(value);
}

size_t effectivePayloadLen(const Frame& f) {
  return std::min(static_cast<size_t>(f.payloadLen), kMaxPayload);
}

SetControlDecode decodeSetControl(const Frame& f) {
  SetControlDecode d;
  if (f.baseMsgType() != static_cast<uint8_t>(MsgType::kSetControlCmd)) return d;
  d.isSetControl = true;
  d.isResponse = f.isResponse();
  d.commandCode = f.sendParamHi;
  d.command = commandName(d.commandCode);

  const size_t pl = effectivePayloadLen(f);
  if (pl >= 2) {
    d.hasEcho = true;
    d.echoCode = static_cast<uint16_t>(f.payload[0] |
                                       (static_cast<uint16_t>(f.payload[1]) << 8));
    d.echoMatches = (d.echoCode == d.commandCode);
  }

  d.isSystemSwitch =
      (d.commandCode == static_cast<uint8_t>(Command::kSystemSwitchModify));
  if (d.isSystemSwitch && pl >= 3) {  // value at frame [12] (provisional)
    d.hasSwitchValue = true;
    d.switchRaw = f.payload[2];
    d.switchKnown = (systemSwitchNameOrNull(d.switchRaw) != nullptr);
    d.switchName = systemSwitchName(d.switchRaw);
  }

  // Both provisional layouts, always reported side by side (docs/02 §5a:
  // the parser surfaces the ambiguity; resolution comes from captures).
  d.varA = readCandidate(f, pl, kDemandTimerOffsetVarA, kDemandValueOffsetVarA);
  d.varB = readCandidate(f, pl, kDemandTimerOffsetVarB, kDemandValueOffsetVarB);
  return d;
}

std::vector<std::string> decodeDiagnostics(const Frame& f) {
  std::vector<std::string> faults;
  const size_t pl = effectivePayloadLen(f);
  size_t start = 0;
  for (size_t i = 0; i <= pl; ++i) {
    if (i == pl || f.payload[i] == 0) {  // bounded: never relies on a trailing null
      if (i > start) faults.emplace_back(reinterpret_cast<const char*>(&f.payload[start]),
                                         i - start);
      start = i + 1;
    }
  }
  return faults;
}

SensorDataDecode decodeSensorData(const Frame& f) {
  SensorDataDecode d;
  const size_t pl = effectivePayloadLen(f);
  size_t i = 0;
  while (i < pl) {
    if (i + 2 > pl) {  // lone db_id byte, no length
      d.truncated = true;
      break;
    }
    SensorRecord r;
    r.dbId = f.payload[i];
    r.dbLen = f.payload[i + 1];
    if (r.dbId == 0) r.label = "OAT candidate (unconfirmed)";  // docs/02 §5b
    const size_t dataStart = i + 2;
    const size_t avail = pl - dataStart;
    const size_t take = std::min(static_cast<size_t>(r.dbLen), avail);
    r.data.assign(f.payload + dataStart, f.payload + dataStart + take);
    if (take < r.dbLen) {
      r.truncated = true;
      d.truncated = true;
      d.records.push_back(std::move(r));
      break;
    }
    d.records.push_back(std::move(r));
    i = dataStart + take;
  }
  return d;
}

std::string byteGrid(const Frame& f) {
  const size_t pl = effectivePayloadLen(f);
  if (pl == 0) return "(no payload)\n";
  std::string out = "        +0 +1 +2 +3 +4 +5 +6 +7\n";
  for (size_t row = 0; row < pl; row += 8) {
    out += fmt("[%3zu]  ", row + kHeaderLen);  // frame offsets, per docs/02 §5a notation
    for (size_t i = row; i < std::min(row + 8, pl); ++i) {
      out += fmt(" %02x", f.payload[i]);
    }
    out += '\n';
  }
  return out;
}

FieldDictionary FieldDictionary::withStarterSet() {
  // Starter entries from docs/02 §5a/§5b — ALL provisional until confirmed
  // from real captures (the Phase 2 "done when" gate).
  FieldDictionary d;
  d.add({0x03, 0x00, 4, "control command code",
         "header Send Parameter Hi; echoed in payload", true});
  d.add({0x03, 0x00, 10, "command code echo (LE16, low byte)",
         "16-bit little-endian echo of header offset 4", true});
  d.add({0x03, 0x00, 11, "command code echo (LE16, high byte)",
         "expected 0x00 for known commands", true});
  d.add({0x03, 0x64, 12, "HEAT_DEMAND refresh timer (variant A)",
         "hi nibble=min, lo nibble=3.75 s units; single-sourced", true});
  d.add({0x03, 0x64, 13, "HEAT_DEMAND: demand pct*2 (variant A) OR refresh timer (variant B)",
         "kdschlosser self-contradicts; resolve by capture diff", true});
  d.add({0x03, 0x64, 14, "HEAT_DEMAND demand pct*2 (variant B)",
         "write-path layout in kdschlosser", true});
  d.add({0x03, 0x05, 12, "SYSTEM_SWITCH_MODIFY value",
         "0=OFF 1=COOL 2=AUTO 3=HEAT 4=BACKUP_HEAT; persistent, no refresh timer", true});
  d.add({0x86, 0x00, 10, "fault strings start",
         "null-separated strings to end of payload", true});
  d.add({0x87, 0x00, 10, "first sensor TLV record (db_id)",
         "TLV (db_id, db_len, data); MDI id 0 = OAT candidate (unconfirmed)", true});
  return d;
}

void FieldDictionary::add(const FieldEntry& e) {
  for (auto& existing : entries_) {
    if (existing.msgType == e.msgType && existing.command == e.command &&
        existing.frameOffset == e.frameOffset) {
      existing = e;
      return;
    }
  }
  entries_.push_back(e);
}

const FieldEntry* FieldDictionary::find(uint8_t msgType, uint8_t command,
                                        uint16_t frameOffset) const {
  for (const auto& e : entries_) {
    if (e.msgType == msgType && e.command == command &&
        e.frameOffset == frameOffset) {
      return &e;
    }
  }
  return nullptr;
}

std::string FieldDictionary::toTable() const {
  size_t nameW = 4;
  for (const auto& e : entries_) nameW = std::max(nameW, e.name.size());
  std::string out = fmt("%-8s %-8s %-6s %-*s %-12s %s\n", "msgType", "command",
                        "offset", static_cast<int>(nameW), "name", "status", "notes");
  for (const auto& e : entries_) {
    out += fmt("%-8s %-8s %-6u %-*s %-12s %s\n", hexByte(e.msgType).c_str(),
               e.command == 0 ? "-" : hexByte(e.command).c_str(), e.frameOffset,
               static_cast<int>(nameW), e.name.c_str(),
               e.provisional ? "PROVISIONAL" : "confirmed", e.notes.c_str());
  }
  return out;
}

std::string summarize(const Frame& f) {
  std::string out =
      fmt("%s src=%s dst=%s subnet=%s nodeType=%s pkt=%s len=%u\n",
          msgTypeName(f.msgType).c_str(), hexByte(f.src).c_str(),
          hexByte(f.dst).c_str(), hexByte(f.subnet).c_str(),
          hexByte(f.srcNodeType).c_str(), hexByte(f.packetNum).c_str(),
          static_cast<unsigned>(f.payloadLen));
  if (f.payloadLen > kMaxPayload) {
    out += fmt("  WARNING: payloadLen %u exceeds max %zu, clamped\n",
               static_cast<unsigned>(f.payloadLen), kMaxPayload);
  }

  const uint8_t base = f.baseMsgType();
  if (base == static_cast<uint8_t>(MsgType::kSetControlCmd)) {
    const SetControlDecode d = decodeSetControl(f);
    out += fmt("  command: %s (%s), echo %s\n", d.command.c_str(),
               hexByte(d.commandCode).c_str(),
               !d.hasEcho ? "absent"
                          : (d.echoMatches ? "matches" : "MISMATCH"));
    if (d.hasSwitchValue) {
      out += fmt("  system switch: %s (%s)\n", d.switchName.c_str(),
                 hexByte(d.switchRaw).c_str());
    }
    out += "  demand candidates (PROVISIONAL offsets, docs/02 5a):\n";
    for (const DemandCandidate* c : {&d.varA, &d.varB}) {
      out += fmt("    [%zu/%zu]: ", c->timerFrameOffset, c->valueFrameOffset);
      out += c->valid ? fmt("timer=%s (%.2f s) demand=%u -> %.1f%%\n",
                            hexByte(c->timerRaw).c_str(), c->timerTotalS,
                            static_cast<unsigned>(c->demandRaw), c->demandPct)
                      : std::string("(payload too short)\n");
    }
  } else if (f.msgType ==
             (static_cast<uint8_t>(MsgType::kGetDiagnostics) | kResponseFlag)) {
    for (const auto& s : decodeDiagnostics(f)) out += "  fault: " + s + "\n";
  } else if (f.msgType ==
             (static_cast<uint8_t>(MsgType::kGetSensorData) | kResponseFlag)) {
    const SensorDataDecode d = decodeSensorData(f);
    for (const auto& r : d.records) {
      out += fmt("  sensor db_id=%u len=%u", static_cast<unsigned>(r.dbId),
                 static_cast<unsigned>(r.dbLen));
      if (!r.label.empty()) out += " " + r.label;
      if (r.truncated) out += " TRUNCATED";
      out += "\n";
    }
    if (d.truncated) out += "  WARNING: TLV walk truncated\n";
  }
  out += byteGrid(f);
  return out;
}

}  // namespace ct485
