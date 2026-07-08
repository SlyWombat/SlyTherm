// Ct485Frame.h — CT-485 frame encode/decode, Fletcher-16 checksum, and
// gap-delimited frame reassembly (docs/02-protocol-climatetalk.md §2-§3).
//
// CT-485 has no preamble or sync byte: frames are delimited ONLY by a
// >= 3.5 ms idle gap on the bus (docs/02 §1). All timing lives in the caller
// (UART RX-timeout interrupt or host test); this module is pure C++17 with
// no Arduino dependencies and no heap.

#pragma once
#include <cstddef>
#include <cstdint>

#include "Ct485Core.h"

namespace ct485 {

// Fletcher-16 variant per docs/02 §3 (ported exactly from the Net485
// reference): seed s1=0xAA / s2=0x00, modulus 0xFF (255, not 256), summed
// over the entire header + payload (offsets 0 .. 10+payloadLen-1).
// Writes the two checksum bytes at buf[10+payloadLen] and buf[11+payloadLen].
void fletcher16(uint8_t* buf, uint8_t payloadLen);

// Validation accumulates header + payload AND both checksum bytes;
// a valid frame yields s1 == 0 && s2 == 0.
bool fletcherOk(const uint8_t* buf, uint8_t payloadLen);

// Serialize frame into out (caller provides >= frame.totalLen() bytes,
// kMaxFrame always suffices). Appends the Fletcher checksum. Returns the
// total length written (header + payload + checksum), or 0 if
// frame.payloadLen > kMaxPayload.
size_t encode(const Frame& frame, uint8_t* out);

// Parse and validate one raw frame. Accepts only if
// len == buf[kOffPayloadLen] + 12 (docs/02 §2 receiver algorithm),
// the declared payload fits kMaxPayload, and the Fletcher check passes.
// On failure `out` is untouched.
bool decode(const uint8_t* buf, size_t len, Frame& out);

// Reassembles raw frames from the RX byte stream. PRIMARY framing is
// length-based: once the 10-byte header is in, the frame closes exactly at
// buf[9] + 12 bytes — immune to fake gaps from UART read batching and to
// back-to-back frames merging. The caller's >= 3.5 ms gap detection
// (gapBefore=true on the first byte after a gap) remains the RESYNC path:
// it closes out whatever is accumulated (rejecting torn fragments) so a
// corrupted or mid-joined stream re-locks at the next real gap. flush()
// closes the in-progress frame when the bus goes idle with no following
// byte. Counters feed the sniffer's diagnostics.
class FrameAccumulator {
 public:
  struct Counters {
    uint32_t framesOk    = 0;
    uint32_t badChecksum = 0;  // length OK but Fletcher failed
    uint32_t badLength   = 0;  // runt (< 12 B) or total != buf[9] + 12 (torn/merged frame)
    uint32_t overruns    = 0;  // exceeded kMaxFrame before a gap; counted once per frame
  };

  // Feed one received byte. Returns true when this call completed a VALID
  // frame (i.e. gapBefore closed out the accumulated bytes and they passed
  // length + Fletcher validation). The completed raw frame is then available
  // via frame()/frameLen(); it remains valid until the next completed frame.
  bool feed(uint8_t byte, bool gapBefore);

  // Close the in-progress frame without a following byte (bus idle / end of
  // capture). Same validation and return semantics as a gapBefore close.
  bool flush();

  // Most recently completed valid raw frame (header + payload + checksum).
  const uint8_t* frame() const { return done_; }
  size_t frameLen() const { return doneLen_; }

  const Counters& counters() const { return counters_; }
  void clearCounters() { counters_ = Counters{}; }

  // Bytes from the most recent REJECTED close (badLength/badChecksum) — a
  // torn or merged burst that gap framing could not validate. Copies them to
  // `out` (caller provides >= kMaxFrame), clears the stash, and returns the
  // length; 0 = nothing pending. Single-slot: a second rejection before the
  // take overwrites the first (the Counters still record every rejection).
  // Overrun drops are not stashed — their bytes were never kept.
  size_t takeRejected(uint8_t* out);

  // Drop in-progress bytes and the completed frame; counters are kept.
  void reset();

 private:
  bool closeFrame();
  // Total frame length the accumulated header declares (buf[9] + 12), or 0
  // while the header is incomplete / declares an impossible payload.
  size_t expectedLen() const;

  uint8_t buf_[kMaxFrame] = {};
  size_t  len_ = 0;
  bool    overflowed_ = false;

  uint8_t done_[kMaxFrame] = {};
  size_t  doneLen_ = 0;

  uint8_t rej_[kMaxFrame] = {};
  size_t  rejLen_ = 0;

  Counters counters_;
};

}  // namespace ct485
