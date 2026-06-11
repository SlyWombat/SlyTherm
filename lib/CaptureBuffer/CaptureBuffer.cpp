#include "CaptureBuffer.h"

#include <cstdio>
#include <cstring>

namespace capture {
namespace {

constexpr uint8_t kTypeFrame = 0;
constexpr uint8_t kTypeAnnotation = 1;

constexpr size_t kCommonHdr = 1 + 4 + 2;       // type, timestampMs, dataLen
constexpr size_t kFrameHdr = kCommonHdr + 4 + 1;  // + gapUs, fletcherOk

void putU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void putU32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}
uint16_t getU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

CaptureBuffer::CaptureBuffer(size_t budgetBytes)
    : ring_(new uint8_t[budgetBytes ? budgetBytes : 1]),
      budget_(budgetBytes ? budgetBytes : 1) {}

CaptureBuffer::~CaptureBuffer() { delete[] ring_; }

void CaptureBuffer::putBytes(const uint8_t* src, size_t n) {
  size_t tail = (head_ + used_) % budget_;
  for (size_t i = 0; i < n; ++i) {
    ring_[tail] = src[i];
    tail = (tail + 1 == budget_) ? 0 : tail + 1;
  }
  used_ += n;
}

void CaptureBuffer::getBytes(uint8_t* dst, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    dst[i] = ring_[head_];
    head_ = (head_ + 1 == budget_) ? 0 : head_ + 1;
  }
  used_ -= n;
}

void CaptureBuffer::dropOldest() {
  uint8_t hdr[kCommonHdr];
  getBytes(hdr, kCommonHdr);
  const size_t dataLen = getU16(hdr + 5);
  const size_t extra = (hdr[0] == kTypeFrame) ? (kFrameHdr - kCommonHdr) : 0;
  // Skip without copying out.
  head_ = (head_ + extra + dataLen) % budget_;
  used_ -= extra + dataLen;
  if (hdr[0] == kTypeFrame) --frames_; else --annotations_;
  ++evicted_;
}

bool CaptureBuffer::makeRoom(size_t need) {
  if (need > budget_) return false;
  while (budget_ - used_ < need) dropOldest();
  return true;
}

bool CaptureBuffer::addFrame(uint32_t timestampMs, uint32_t gapUs,
                             const uint8_t* raw, size_t len, bool fletcherOk) {
  if (len > 0xFFFF) return false;
  if (!makeRoom(kFrameHdr + len)) return false;
  uint8_t hdr[kFrameHdr];
  hdr[0] = kTypeFrame;
  putU32(hdr + 1, timestampMs);
  putU16(hdr + 5, static_cast<uint16_t>(len));
  putU32(hdr + 7, gapUs);
  hdr[11] = fletcherOk ? 1 : 0;
  putBytes(hdr, kFrameHdr);
  putBytes(raw, len);
  ++frames_;
  return true;
}

bool CaptureBuffer::addAnnotation(uint32_t timestampMs, const char* text) {
  size_t len = text ? strlen(text) : 0;
  if (len > kMaxAnnotationLen) len = kMaxAnnotationLen;
  if (!makeRoom(kCommonHdr + len)) return false;
  uint8_t hdr[kCommonHdr];
  hdr[0] = kTypeAnnotation;
  putU32(hdr + 1, timestampMs);
  putU16(hdr + 5, static_cast<uint16_t>(len));
  putBytes(hdr, kCommonHdr);
  putBytes(reinterpret_cast<const uint8_t*>(text), len);
  ++annotations_;
  return true;
}

size_t CaptureBuffer::drainNext(char* out, size_t outCap) {
  if (used_ == 0 || outCap == 0) return 0;
  uint8_t hdr[kFrameHdr];
  getBytes(hdr, kCommonHdr);
  const uint8_t type = hdr[0];
  const uint32_t tMs = getU32(hdr + 1);
  const size_t dataLen = getU16(hdr + 5);

  size_t pos = 0;
  auto emit = [&](const char* fmt, auto... args) {
    if (pos < outCap) {
      int n = snprintf(out + pos, outCap - pos, fmt, args...);
      if (n > 0) pos += (static_cast<size_t>(n) < outCap - pos)
                            ? static_cast<size_t>(n) : outCap - pos - 1;
    }
  };

  if (type == kTypeFrame) {
    getBytes(hdr + kCommonHdr, kFrameHdr - kCommonHdr);
    const uint32_t gapUs = getU32(hdr + 7);
    const bool ok = hdr[11] != 0;
    --frames_;
    // gap padded to >= 3 digits: a bare 2-digit gap is a valid hex-pair token
    // and the decoder's longest-run rule would absorb it into the frame bytes.
    emit("%lu %03lu", static_cast<unsigned long>(tMs),
         static_cast<unsigned long>(gapUs));
    for (size_t i = 0; i < dataLen; ++i) {
      uint8_t b;
      getBytes(&b, 1);
      emit(" %02X", b);
    }
    if (!ok) emit("%s", " !fcs");
  } else {
    --annotations_;
    emit("# %lums ", static_cast<unsigned long>(tMs));
    for (size_t i = 0; i < dataLen; ++i) {
      uint8_t c;
      getBytes(&c, 1);
      // Keep annotation lines single-line comments for the decoder.
      if (c == '\n' || c == '\r') c = ' ';
      emit("%c", c);
    }
  }
  return pos;
}

void CaptureBuffer::clear() {
  head_ = used_ = frames_ = annotations_ = 0;
  evicted_ = 0;
}

}  // namespace capture
