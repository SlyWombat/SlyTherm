// CaptureBuffer.h — bounded ring buffer of sniffed CT-485 frames + operator
// annotations, drained as text lines in the EXACT input format that
// tools/ct485_decode.py ingests (issue #57).
//
// Drain line formats (tools/ct485_decode.py parse_line contract):
//   frame:       "<millis> <gap_us> <HEX> <HEX> ..."   [gap_us zero-padded to
//                >= 3 digits so it can never read as a hex-pair byte token]
//                                                      [" !fcs" suffix when the
//                Fletcher check failed — a trailing non-hex token, which the
//                decoder ignores; its resync slider then skips the bad bytes]
//   annotation:  "# <millis>ms <text>"                 [comment line, skipped]
// Changing either format breaks the PC-side toolchain; the round-trip test in
// test/test_capture_buffer re-implements the decoder's line parser to pin it.
//
// Memory: one heap allocation of `budgetBytes` at construction, never resized.
// When full, the OLDEST records are evicted to make room (drop counter kept),
// so a download always yields the most recent traffic.
//
// Pure C++17, no Arduino dependencies; time is injected by the caller.

#pragma once
#include <cstddef>
#include <cstdint>

namespace capture {

// Default budget ~256 typical frames: observed CT-485 traffic is dominated by
// short Set-Control/status frames (raw 16-40 B; worst case 252 B). 256 *
// (11 B record header + ~50 B raw headroom) ≈ 16 KiB — trivial against the
// ESP32's ~300 KiB heap, generous for a bench capture between downloads.
constexpr size_t kDefaultBudgetBytes = 16 * 1024;

// Annotations longer than this are truncated (operator note, not a transcript).
constexpr size_t kMaxAnnotationLen = 200;

// Worst-case drained line: 252 raw bytes * 3 chars + millis/gap prefix + "!fcs"
// + NUL. Callers pass a buffer of at least this size to drainNext().
constexpr size_t kMaxLineLen = 28 + 252 * 3 + 8;

class CaptureBuffer {
 public:
  explicit CaptureBuffer(size_t budgetBytes = kDefaultBudgetBytes);
  ~CaptureBuffer();
  CaptureBuffer(const CaptureBuffer&) = delete;
  CaptureBuffer& operator=(const CaptureBuffer&) = delete;

  // Append one framed capture record, evicting oldest records as needed.
  // Returns false (drops nothing) if the record alone exceeds the budget.
  bool addFrame(uint32_t timestampMs, uint32_t gapUs,
                const uint8_t* raw, size_t len, bool fletcherOk);

  // Append one operator annotation (NUL-terminated; truncated to
  // kMaxAnnotationLen). Same eviction/rejection rules as addFrame.
  bool addAnnotation(uint32_t timestampMs, const char* text);

  // Pop the OLDEST record and render it as one decoder-ingestible line into
  // `out` (NUL-terminated, no trailing newline). Returns the line length, or
  // 0 when the buffer is empty. outCap >= kMaxLineLen renders any record;
  // smaller buffers truncate the line (never overrun).
  size_t drainNext(char* out, size_t outCap);

  void clear();

  size_t frameCount() const { return frames_; }
  size_t annotationCount() const { return annotations_; }
  size_t recordCount() const { return frames_ + annotations_; }
  uint32_t evictedRecords() const { return evicted_; }  // dropped-oldest total
  size_t bytesUsed() const { return used_; }
  size_t budgetBytes() const { return budget_; }

 private:
  // Record wire format inside the ring (little-endian u16/u32):
  //   u8 type (0 frame, 1 annotation), u32 timestampMs, u16 dataLen,
  //   frame only: u32 gapUs, u8 fletcherOk,  then dataLen raw/text bytes.
  void putBytes(const uint8_t* src, size_t n);
  void getBytes(uint8_t* dst, size_t n);   // consumes from head_
  void dropOldest();
  bool makeRoom(size_t need);

  uint8_t* ring_;
  size_t budget_;
  size_t head_ = 0;   // read position
  size_t used_ = 0;
  size_t frames_ = 0;
  size_t annotations_ = 0;
  uint32_t evicted_ = 0;
};

}  // namespace capture
