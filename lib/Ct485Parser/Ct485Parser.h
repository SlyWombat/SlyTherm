// Ct485Parser.h — sniff-side decoder for CT-485 frames (Phase 2, docs/02 §4-§5).
//
// READ-ONLY analysis: turns a validated ct485::Frame into labeled fields for
// logging, stimulus-response diffing, and field-dictionary building. It never
// emits demand and never picks a winner where prior art disagrees — in
// particular the Set Control Command demand offsets (docs/02 §5a OFFSET
// WARNING) are reported under BOTH provisional variants until real captures
// resolve them.
//
// Offset convention: all "offset" values here are FRAME offsets (the docs/02
// numbering, where the 10-byte header occupies 0-9 and payload[0] is frame
// offset 10). Subtract ct485::kHeaderLen to index Frame::payload.
//
// Pure C++17, no Arduino dependencies — host-testable.

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Ct485Core.h"

namespace ct485 {

// "0xNN" (uppercase hex) — the fallback rendering for unknown enum values.
std::string hexByte(uint8_t v);

// Name lookups covering every enum value in Ct485Core.h; unknowns -> hex.
// msgTypeName() understands the response flag ("GET_STATUS_RESPONSE").
std::string msgTypeName(uint8_t msgType);
std::string commandName(uint8_t command);
std::string systemSwitchName(uint8_t value);

// payloadLen comes off the wire and may lie (up to 255 > kMaxPayload buffer).
// Every decoder bounds itself with this. docs/04: invalid input must degrade
// safely, never overrun.
size_t effectivePayloadLen(const Frame& f);

// ---------- Set Control Command (0x03/0x83), docs/02 §5a ----------

// One (timer, demand) reading under a single provisional offset variant.
struct DemandCandidate {
  bool valid = false;  // offsets fell inside the bounded payload
  size_t timerFrameOffset = 0;
  size_t valueFrameOffset = 0;
  uint8_t timerRaw = 0;
  uint8_t demandRaw = 0;
  float demandPct = 0.0f;   // raw / 2.0 (reported as-is, no clamping — parser
                            //  surfaces, control modules clamp)
  uint8_t timerMinutes = 0; // high nibble
  uint8_t timerUnits = 0;   // low nibble, 3.75 s units
  float timerTotalS = 0.0f;
};

struct SetControlDecode {
  bool isSetControl = false;  // baseMsgType == 0x03; everything below invalid otherwise
  bool isResponse = false;
  uint8_t commandCode = 0;  // header offset 4 (Send Parameter Hi)
  std::string command;
  bool hasEcho = false;       // 16-bit LE echo present at frame [10..11]
  uint16_t echoCode = 0;
  bool echoMatches = false;   // echo == header command code
  bool isSystemSwitch = false;
  bool hasSwitchValue = false;  // value byte at frame [12] present
  uint8_t switchRaw = 0;
  bool switchKnown = false;
  std::string switchName;
  // BOTH provisional demand layouts (docs/02 §5a: [12]/[13] vs [13]/[14]).
  DemandCandidate varA;
  DemandCandidate varB;
};

SetControlDecode decodeSetControl(const Frame& f);

// ---------- Get Diagnostics response (0x86), docs/02 §5b ----------
// Null-separated fault strings; bounded; tolerates a missing trailing null
// and skips empty segments (consecutive nulls).
std::vector<std::string> decodeDiagnostics(const Frame& f);

// ---------- Get Sensor Data response (0x87), docs/02 §5b ----------
struct SensorRecord {
  uint8_t dbId = 0;
  uint8_t dbLen = 0;            // declared length (may exceed available bytes)
  std::vector<uint8_t> data;    // actual bytes captured (<= dbLen)
  bool truncated = false;       // declared length ran past the bounded payload
  std::string label;            // MDI id 0 -> "OAT candidate (unconfirmed)"
};

struct SensorDataDecode {
  std::vector<SensorRecord> records;
  bool truncated = false;  // TLV walk hit the bounded end mid-record
};

SensorDataDecode decodeSensorData(const Frame& f);

// ---------- Raw view for stimulus-response diffing ----------
// Offset-labeled hex dump of the payload, rows of 8, labels in FRAME offsets
// (decimal) so lines match the [12]/[13]/[14] notation in docs/02 §5a.
std::string byteGrid(const Frame& f);

// ---------- Field dictionary (Phase 2 deliverable artifact) ----------
struct FieldEntry {
  uint8_t msgType = 0;
  uint8_t command = 0;  // 0x00 = not command-specific
  uint16_t frameOffset = 0;
  std::string name;
  std::string notes;
  bool provisional = true;  // unconfirmed by real captures
};

class FieldDictionary {
 public:
  // Built-in starter set from docs/02 §5a/§5b — every entry provisional.
  static FieldDictionary withStarterSet();

  // Adds or replaces the entry with the same (msgType, command, frameOffset).
  void add(const FieldEntry& e);
  const FieldEntry* find(uint8_t msgType, uint8_t command,
                         uint16_t frameOffset) const;
  size_t size() const { return entries_.size(); }
  const std::vector<FieldEntry>& entries() const { return entries_; }

  // Human-readable aligned table (the documented-dictionary deliverable).
  std::string toTable() const;

 private:
  std::vector<FieldEntry> entries_;
};

// Labeled multi-line summary of one frame: header fields, type-specific
// decode (0x03 demand candidates / switch, 0x86 faults, 0x87 records), and
// the byte grid.
std::string summarize(const Frame& f);

}  // namespace ct485
