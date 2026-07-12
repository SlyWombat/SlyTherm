#include "CompressorGuard.h"

#include <cstring>

namespace dettson {

CompressorGuard::CompressorGuard() : CompressorGuard(Config{}) {}

CompressorGuard::CompressorGuard(const Config& cfg) : cfg_(cfg) {}

uint16_t CompressorGuard::blobCrc(const PersistBlob& b) {
  // Fletcher-16 over everything after the crc field (same family as the
  // CT-485 frame checksum; collision-resistant enough for an NVS blob).
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&b);
  const size_t start = offsetof(PersistBlob, crc) + sizeof(b.crc);
  uint16_t sum1 = 0, sum2 = 0;
  for (size_t i = start; i < sizeof(PersistBlob); ++i) {
    sum1 = static_cast<uint16_t>((sum1 + p[i]) % 255);
    sum2 = static_cast<uint16_t>((sum2 + sum1) % 255);
  }
  return static_cast<uint16_t>((sum2 << 8) | sum1);
}

bool CompressorGuard::restoreFromBlob(const PersistBlob& blob, uint32_t nowS) {
  if (blob.magic != kBlobMagic || blob.version != kBlobVersion) return false;
  if (blob.crc != blobCrc(blob)) return false;
  if (blob.startCount > kMaxStartHistory || blob.bootCount > kMaxBootHistory) return false;

  lockedOut_    = blob.lockedOut != 0;
  hasLastStart_ = blob.hasLastStart != 0;
  hasLastStop_  = blob.hasLastStop != 0;
  lastStartS_   = blob.lastStartS;
  lastStopS_    = blob.lastStopS;
  startCount_   = blob.startCount;
  std::memcpy(startTimesS_, blob.startTimesS, sizeof(startTimesS_));
  bootCount_ = blob.bootCount;
  std::memcpy(bootTimesS_, blob.bootTimesS, sizeof(bootTimesS_));

  // Boot = no demand (docs/04 §3). If the blob says the compressor was
  // running when we died, its true stop time is unknown — assume it stopped
  // just now so the full min-off applies before any restart.
  running_ = false;
  if (blob.running) {
    hasLastStop_ = true;
    lastStopS_   = nowS;
  }
  return true;
}

void CompressorGuard::recordBoot(uint32_t nowS) {
  if (bootCount_ < kMaxBootHistory) {
    bootTimesS_[bootCount_++] = nowS;
  } else {
    std::memmove(bootTimesS_, bootTimesS_ + 1,
                 (kMaxBootHistory - 1) * sizeof(uint32_t));
    bootTimesS_[kMaxBootHistory - 1] = nowS;
  }
  uint8_t inWindow = 0;
  for (uint8_t i = 0; i < bootCount_; ++i) {
    if (elapsedS(nowS, bootTimesS_[i]) <= cfg_.resetLoopWindowS) ++inWindow;
  }
  if (inWindow >= cfg_.resetLoopCount) lockedOut_ = true;  // latched
}

void CompressorGuard::bootRestore(const PersistBlob* blob, uint32_t nowS,
                                  bool abnormalReset, uint32_t jitterS) {
  booted_ = true;
  const bool restored = (blob != nullptr) && restoreFromBlob(*blob, nowS);
  if (!restored) {
    // Unknown state -> assume the worst: full hold-off (docs/04 §2).
    hasLastStart_ = hasLastStop_ = false;
    startCount_ = 0;
    bootCount_  = 0;
    lockedOut_  = false;
    running_    = false;
    holdoffEndS_ = nowS + cfg_.bootHoldoffS + jitterS;
  } else {
    // Persisted timers carry the protection; hold-off only adds jitter slack.
    holdoffEndS_ = nowS + jitterS;
  }
  if (abnormalReset) recordBoot(nowS);
}

uint32_t CompressorGuard::startsInWindow(uint32_t nowS) const {
  uint32_t n = 0;
  for (uint8_t i = 0; i < startCount_; ++i) {
    if (elapsedS(nowS, startTimesS_[i]) < 3600) ++n;
  }
  return n;
}

CompressorGuard::Decision CompressorGuard::requestStart(uint32_t nowS, bool manual) {
  if (lockedOut_) return {false, kForeverS, Deny::kResetLoopLockout};
  if (running_) return {true, 0, Deny::kAlreadyInState};

  uint32_t wait = 0;
  Deny reason = Deny::kNone;

  if (!booted_) {  // bootRestore() not called: treat as unknown state
    wait = cfg_.bootHoldoffS;
    reason = Deny::kBootHoldoff;
  }
  if (booted_ && nowS < holdoffEndS_) {
    wait = holdoffEndS_ - nowS;
    reason = Deny::kBootHoldoff;
  }
  // min-OFF gates AUTOMATIC restarts only. A MANUAL (user-initiated) request
  // bypasses it — the demand goes out immediately; the ODU's own ~3-min
  // restart delay is the physical backstop (docs/04 §1a; see header). Every
  // other gate above/below (boot hold-off, lockout, max-starts/hour) still
  // applies to a manual start.
  if (!manual && hasLastStop_) {
    const uint32_t off = elapsedS(nowS, lastStopS_);
    if (off < cfg_.minOffS && cfg_.minOffS - off > wait) {
      wait = cfg_.minOffS - off;
      reason = Deny::kMinOff;
    }
  }
  if (startsInWindow(nowS) >= cfg_.maxStartsPerHour) {
    // Allowed again when the oldest start in the window ages out.
    uint32_t oldestWait = 0;
    bool first = true;
    for (uint8_t i = 0; i < startCount_; ++i) {
      const uint32_t age = elapsedS(nowS, startTimesS_[i]);
      if (age < 3600) {
        const uint32_t w = 3600 - age;
        if (first || w < oldestWait) oldestWait = w;
        first = false;
      }
    }
    if (oldestWait > wait) {
      wait = oldestWait;
      reason = Deny::kStartsPerHour;
    }
  }

  if (wait > 0) return {false, wait, reason};

  running_      = true;
  hasLastStart_ = true;
  lastStartS_   = nowS;
  if (startCount_ < kMaxStartHistory) {
    startTimesS_[startCount_++] = nowS;
  } else {
    std::memmove(startTimesS_, startTimesS_ + 1,
                 (kMaxStartHistory - 1) * sizeof(uint32_t));
    startTimesS_[kMaxStartHistory - 1] = nowS;
  }
  return {true, 0, Deny::kNone};
}

CompressorGuard::Decision CompressorGuard::requestStop(uint32_t nowS, bool safety) {
  if (!running_) return {true, 0, Deny::kAlreadyInState};

  if (!safety && hasLastStart_) {
    const uint32_t on = elapsedS(nowS, lastStartS_);
    if (on < cfg_.minOnS) return {false, cfg_.minOnS - on, Deny::kMinOn};
  }

  running_     = false;
  hasLastStop_ = true;
  lastStopS_   = nowS;
  return {true, 0, Deny::kNone};
}

void CompressorGuard::manualClear() {
  lockedOut_ = false;
  bootCount_ = 0;
}

void CompressorGuard::save(PersistBlob* out) const {
  if (out == nullptr) return;
  *out = PersistBlob{};
  out->magic        = kBlobMagic;
  out->version      = kBlobVersion;
  out->running      = running_ ? 1 : 0;
  out->hasLastStart = hasLastStart_ ? 1 : 0;
  out->hasLastStop  = hasLastStop_ ? 1 : 0;
  out->lockedOut    = lockedOut_ ? 1 : 0;
  out->startCount   = startCount_;
  out->bootCount    = bootCount_;
  out->lastStartS   = lastStartS_;
  out->lastStopS    = lastStopS_;
  std::memcpy(out->startTimesS, startTimesS_, sizeof(startTimesS_));
  std::memcpy(out->bootTimesS, bootTimesS_, sizeof(bootTimesS_));
  out->crc = blobCrc(*out);
}

}  // namespace dettson
