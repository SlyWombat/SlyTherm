// Ct485Frame.cpp — see Ct485Frame.h. Checksum ported EXACTLY from the
// reference C++ in docs/02-protocol-climatetalk.md §3 (Net485-derived).

#include "Ct485Frame.h"

#include <cstring>

namespace ct485 {

void fletcher16(uint8_t* buf, uint8_t payloadLen) {
  uint8_t s1 = 0xAA, s2 = 0x00;  // CT_ISUM1 / CT_ISUM2 "New Fletcher Seed"
  const uint16_t n = static_cast<uint16_t>(payloadLen) + kHeaderLen;
  for (uint16_t i = 0; i < n; i++) {
    s1 = static_cast<uint8_t>((s1 + buf[i]) % 0xFF);
    s2 = static_cast<uint8_t>((s2 + s1) % 0xFF);
  }
  buf[n]     = static_cast<uint8_t>(0xFF - ((s1 + s2) % 0xFF));
  buf[n + 1] = static_cast<uint8_t>(0xFF - ((s1 + buf[n]) % 0xFF));
}

bool fletcherOk(const uint8_t* buf, uint8_t payloadLen) {
  uint8_t s1 = 0xAA, s2 = 0x00;
  const uint16_t n = static_cast<uint16_t>(payloadLen) + kHeaderLen;
  for (uint16_t i = 0; i < n + kChecksumLen; i++) {
    s1 = static_cast<uint8_t>((s1 + buf[i]) % 0xFF);
    s2 = static_cast<uint8_t>((s2 + s1) % 0xFF);
  }
  return s1 == 0 && s2 == 0;
}

size_t encode(const Frame& frame, uint8_t* out) {
  if (frame.payloadLen > kMaxPayload) return 0;
  out[kOffDst]         = frame.dst;
  out[kOffSrc]         = frame.src;
  out[kOffSubnet]      = frame.subnet;
  out[kOffSendMethod]  = frame.sendMethod;
  out[kOffSendParamHi] = frame.sendParamHi;
  out[kOffSendParamLo] = frame.sendParamLo;
  out[kOffSrcNodeType] = frame.srcNodeType;
  out[kOffMsgType]     = frame.msgType;
  out[kOffPacketNum]   = frame.packetNum;
  out[kOffPayloadLen]  = frame.payloadLen;
  std::memcpy(out + kHeaderLen, frame.payload, frame.payloadLen);
  fletcher16(out, frame.payloadLen);
  return frame.totalLen();
}

bool decode(const uint8_t* buf, size_t len, Frame& out) {
  if (len < kHeaderLen + kChecksumLen) return false;
  const uint8_t payloadLen = buf[kOffPayloadLen];
  if (payloadLen > kMaxPayload) return false;
  if (len != static_cast<size_t>(payloadLen) + kHeaderLen + kChecksumLen) return false;
  if (!fletcherOk(buf, payloadLen)) return false;

  out.dst         = buf[kOffDst];
  out.src         = buf[kOffSrc];
  out.subnet      = buf[kOffSubnet];
  out.sendMethod  = buf[kOffSendMethod];
  out.sendParamHi = buf[kOffSendParamHi];
  out.sendParamLo = buf[kOffSendParamLo];
  out.srcNodeType = buf[kOffSrcNodeType];
  out.msgType     = buf[kOffMsgType];
  out.packetNum   = buf[kOffPacketNum];
  out.payloadLen  = payloadLen;
  std::memcpy(out.payload, buf + kHeaderLen, payloadLen);
  return true;
}

bool FrameAccumulator::feed(uint8_t byte, bool gapBefore) {
  bool completed = false;
  if (gapBefore) completed = closeFrame();

  if (!overflowed_) {
    if (len_ < kMaxFrame) {
      buf_[len_++] = byte;
      // Length-based close (primary framing): the header declares the total
      // frame length, so close EXACTLY there. Gap timing measured at UART
      // *read* time slices frames at task-poll boundaries and merges
      // back-to-back frames (2026-07-08 field capture: badLen=567/5min on a
      // healthy bus) — the byte count cannot lie. A bogus header (declared
      // payload > kMaxPayload) disables this and gap framing takes over as
      // the resync fallback.
      if (!completed) {
        const size_t want = expectedLen();
        if (want != 0 && len_ == want) completed = closeFrame();
      }
    } else {
      // Count once per runaway frame; drop bytes until the next gap.
      overflowed_ = true;
      counters_.overruns++;
    }
  }
  return completed;
}

size_t FrameAccumulator::expectedLen() const {
  if (len_ < kHeaderLen) return 0;
  const uint8_t plen = buf_[kOffPayloadLen];
  if (plen > kMaxPayload) return 0;
  return static_cast<size_t>(plen) + kHeaderLen + kChecksumLen;
}

bool FrameAccumulator::flush() { return closeFrame(); }

size_t FrameAccumulator::takeRejected(uint8_t* out) {
  const size_t n = rejLen_;
  if (n) {
    std::memcpy(out, rej_, n);
    rejLen_ = 0;
  }
  return n;
}

void FrameAccumulator::reset() {
  len_ = 0;
  overflowed_ = false;
  doneLen_ = 0;
  rejLen_ = 0;
}

bool FrameAccumulator::closeFrame() {
  const size_t len = len_;
  const bool over = overflowed_;
  len_ = 0;
  overflowed_ = false;

  if (len == 0) return false;   // gap with nothing accumulated: not a frame
  if (over) return false;       // already counted in overruns at overflow time

  if (len < kHeaderLen + kChecksumLen ||
      buf_[kOffPayloadLen] > kMaxPayload ||
      len != static_cast<size_t>(buf_[kOffPayloadLen]) + kHeaderLen + kChecksumLen) {
    counters_.badLength++;
    std::memcpy(rej_, buf_, len);   // salvage: torn/merged burst, PC-side resync
    rejLen_ = len;
    return false;
  }
  if (!fletcherOk(buf_, buf_[kOffPayloadLen])) {
    counters_.badChecksum++;
    std::memcpy(rej_, buf_, len);
    rejLen_ = len;
    return false;
  }

  std::memcpy(done_, buf_, len);
  doneLen_ = len;
  counters_.framesOk++;
  return true;
}

}  // namespace ct485
