// CompressorGuard.h — primary compressor anti-short-cycle protection.
//
// Enforces min-OFF, min-ON, max starts/hour, post-boot hold-off, and the
// reset-loop lockout (docs/04-safety.md §1a/§3, docs/05 defaults table).
// This module is the primary anti-short-cycle protection for the AUTOMATIC
// control loop; the ODU's internal ~3-minute restart delay is a hardware
// backstop underneath it.
//
// Stop semantics (docs/04 §1 prime directive — fail toward no-demand):
//   - Stopping for SAFETY is ALWAYS allowed, immediately, regardless of
//     min-ON (requestStop(now, /*safety=*/true)). Shedding demand must never
//     be blocked.
//   - min-ON is ADVISORY for COMFORT stops only (setpoint satisfied a moment
//     after start): a comfort stop inside min-ON returns allowed=false with
//     the remaining seconds, and the caller keeps the compressor running.
//   - Restart after ANY *automatic* stop honors min-OFF.
//
// Start semantics — the MANUAL bypass (deliberate design relaxation):
//   min-OFF exists to keep the AUTOMATIC control loop from oscillating the
//   compressor. It is NOT meant to block an explicit human request. When a
//   user deliberately changes the setpoint/mode asking for conditioning, the
//   demand should go out immediately: requestStart(now, /*manual=*/true)
//   bypasses min-OFF ONLY. This is safe because the ODU enforces its own
//   ~3-minute internal restart delay (docs/04 §1a) as the physical backstop,
//   and it is the equipment's job, not ours, to protect the compressor on a
//   human-initiated call. Every OTHER protection is preserved even for a
//   manual start: reset-loop lockout, post-boot hold-off, and max-starts/hour
//   ALL still apply (a misbehaving/oscillating command is still capped at
//   maxStartsPerHour). This is a deliberate departure from the historical
//   "no code path may bypass min-OFF" rule, narrowed to genuine user intent
//   with the documented equipment backstop as the justification.
//
// Persistence: timer state is exported/imported as a POD blob (PersistBlob)
// so a reboot cannot erase a pending min-off (docs/04 §2 brownout row). The
// caller owns storage (NVS/RTC) — no NVS dependency here. Timestamps in the
// blob are in the caller's nowS timebase, which therefore must survive a
// reboot (RTC/epoch seconds, NOT millis-since-boot). A missing/corrupt blob
// = unknown state = full boot hold-off (assume the worst).
//
// Pure C++17, no Arduino dependencies; time injected as uint32_t nowS.

#pragma once
#include <cstddef>
#include <cstdint>

#include "DettsonConfig.h"

namespace dettson {

class CompressorGuard {
 public:
  struct Config {
    uint32_t minOffS          = kCompressorMinOffS;
    uint32_t minOnS           = kCompressorMinOnS;
    uint8_t  maxStartsPerHour = kCompressorMaxStartsPerH;
    uint32_t bootHoldoffS     = kBootCompressorHoldoffS;
    uint8_t  resetLoopCount   = kResetLoopLockoutCount;
    uint32_t resetLoopWindowS = kResetLoopWindowS;
  };

  enum class Deny : uint8_t {
    kNone = 0,          // allowed
    kAlreadyInState,    // allowed (idempotent: start while running / stop while stopped)
    kMinOff,
    kMinOn,             // comfort stop only — advisory
    kBootHoldoff,
    kStartsPerHour,
    kResetLoopLockout,  // latched; manualClear() required
  };

  struct Decision {
    bool     allowed = false;
    uint32_t waitS   = 0;  // seconds until the request would be allowed; kForeverS if latched
    Deny     reason  = Deny::kNone;
  };

  static constexpr uint32_t kForeverS        = 0xFFFFFFFFu;
  static constexpr size_t   kMaxStartHistory = 16;  // >= any sane maxStartsPerHour
  static constexpr size_t   kMaxBootHistory  = 8;

  // POD, fixed layout. crc covers every byte after the crc field itself.
  struct PersistBlob {
    uint32_t magic   = 0;
    uint16_t version = 0;
    uint16_t crc     = 0;
    uint8_t  running = 0;
    uint8_t  hasLastStart = 0;
    uint8_t  hasLastStop  = 0;
    uint8_t  lockedOut    = 0;
    uint8_t  startCount   = 0;  // entries used in startTimesS
    uint8_t  bootCount    = 0;  // entries used in bootTimesS
    uint8_t  pad0 = 0;
    uint8_t  pad1 = 0;
    uint32_t lastStartS = 0;
    uint32_t lastStopS  = 0;
    uint32_t startTimesS[kMaxStartHistory] = {};
    uint32_t bootTimesS[kMaxBootHistory]   = {};
  };
  static constexpr uint32_t kBlobMagic   = 0x43475244u;  // "CGRD"
  static constexpr uint16_t kBlobVersion = 1;

  CompressorGuard();
  explicit CompressorGuard(const Config& cfg);

  // Call exactly once at boot, before any request. blob == nullptr or
  // failed validation -> unknown state -> full hold-off from nowS.
  // jitterS: caller-supplied 0-60 s randomization (docs/05 table) — injected
  // so this module stays deterministic. abnormalReset (watchdog/brownout/
  // panic, from the caller's reset-reason register) feeds the reset-loop
  // lockout; a normal power-on does not count toward it.
  void bootRestore(const PersistBlob* blob, uint32_t nowS, bool abnormalReset,
                   uint32_t jitterS = 0);

  // Commits the start when allowed. Idempotent while running.
  // manual=true: a user-initiated request (MQTT/on-panel setpoint or mode
  // change) — bypasses min-OFF ONLY (see header "MANUAL bypass"). All other
  // gates (reset-loop lockout, boot hold-off, max-starts/hour) still apply.
  Decision requestStart(uint32_t nowS, bool manual = false);

  // safety=true: always allowed, commits immediately (see header note).
  // safety=false: denied (advisory) until min-ON has elapsed.
  Decision requestStop(uint32_t nowS, bool safety = false);

  // Clears the latched reset-loop lockout and its boot history. Deliberate
  // human action only (docs/05 table: "manual clear"). Other timers
  // (min-off, hold-off) remain in force — clearing the latch must not grant
  // an instant start.
  void manualClear();

  void save(PersistBlob* out) const;

  bool     running() const { return running_; }
  bool     lockedOut() const { return lockedOut_; }
  uint32_t startsInWindow(uint32_t nowS) const;
  const Config& config() const { return cfg_; }
  void setConfig(const Config& cfg) { cfg_ = cfg; }

 private:
  // Backwards time (RTC step) is treated as zero elapsed — conservative.
  static uint32_t elapsedS(uint32_t nowS, uint32_t thenS) {
    return nowS >= thenS ? nowS - thenS : 0;
  }
  static uint16_t blobCrc(const PersistBlob& b);
  bool restoreFromBlob(const PersistBlob& blob, uint32_t nowS);
  void recordBoot(uint32_t nowS);

  Config   cfg_;
  bool     booted_       = false;
  bool     running_      = false;
  bool     lockedOut_    = false;
  bool     hasLastStart_ = false;
  bool     hasLastStop_  = false;
  uint32_t lastStartS_   = 0;
  uint32_t lastStopS_    = 0;
  uint32_t holdoffEndS_  = 0;  // absolute time the boot hold-off expires
  uint8_t  startCount_   = 0;
  uint32_t startTimesS_[kMaxStartHistory] = {};
  uint8_t  bootCount_ = 0;
  uint32_t bootTimesS_[kMaxBootHistory] = {};
};

}  // namespace dettson
