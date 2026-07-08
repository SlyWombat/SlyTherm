// ota_client.h — shared on-device OTA client (issues #61/#62; docs/10 §6).
//
// Compiled into BOTH roles (Controller thermostat_s3 now; Remote remote_p4 in
// #111). Everything is gated behind -DSLYTHERM_OTA: without the flag the
// header still parses but every entry point is an inline no-op, so callers
// need no #ifdefs of their own.
//
// Flow: fetch firmware/catalog.json (raw.githubusercontent.com, pinned CA
// bundle) -> lib/OtaCatalog resolve(target, hwRev, running) -> on apply,
// stream the GitHub release asset into the inactive A/B slot via Update.h
// (sha256 computed over the stream) -> verify sha256 + ECDSA-P256 signature
// BEFORE activation -> stage -> reboot only when otaSafeToReboot() (the #62
// furnace-idle gate; the Remote's implementation just returns true).
//
// Boot half: bootValidate() runs EARLY in setup(). A pending update that
// fails its self-test (noteSelfTestPass() not called within the timeout, or
// too many boot attempts) is rolled back via esp_ota_set_boot_partition to
// the previous slot. Fail-to-no-demand is inherent — boot-to-no-demand is the
// codebase invariant (docs/04 §3).
//
// The main firmware provides one hook:
//   extern "C" bool otaSafeToReboot();   // Controller: furnace idle ≥5 min
//                                        // Remote: return true

#pragma once
#include <cstdint>

namespace ota {

enum class State : uint8_t {
  kIdle = 0,        // no check performed yet this session
  kChecking,        // fetching/parsing the catalog
  kUpToDate,        // catalog resolved: running version is current
  kUpdateAvailable, // catalog resolved: entry.version > running
  kDownloading,     // streaming the app image into the inactive slot
  kVerifying,       // sha256/signature checks
  kStaged,          // written + verified; waiting for otaSafeToReboot()
  kRebooting,       // reboot imminent
  kFailed,          // last check/apply failed (see error)
  kRolledBack,      // this boot rolled back a bad update (info state)
};

inline const char* toString(State s) {
  switch (s) {
    case State::kIdle: return "idle";
    case State::kChecking: return "checking";
    case State::kUpToDate: return "up_to_date";
    case State::kUpdateAvailable: return "update_available";
    case State::kDownloading: return "downloading";
    case State::kVerifying: return "verifying";
    case State::kStaged: return "staged";
    case State::kRebooting: return "rebooting";
    case State::kFailed: return "failed";
    case State::kRolledBack: return "rolled_back";
  }
  return "idle";
}

struct Status {
  State state = State::kIdle;
  uint8_t progressPct = 0;      // download phase only
  char available[16] = "";      // catalog version when kUpdateAvailable+
  char error[64] = "";          // human-readable failure reason
};

#ifdef SLYTHERM_OTA

// Boot-side, call once EARLY in setup() (needs NVS ready). Returns true when
// this boot performed a rollback (surface it in logs/UI).
bool bootValidate();

// Self-test pass signal — call when the app is provably alive (this project:
// MQTT connected). Confirms a pending update; no-op otherwise.
void noteSelfTestPass();

// Spawn the OTA task (core 0, low priority). Call once from setup() after
// the network task exists. Also performs the daily background check.
void begin();

// Async requests (no-ops while a phase is in flight).
void requestCheck();
void requestApply();

// Thread-safe status copy for the MQTT/UI surfaces.
Status status();

#else  // !SLYTHERM_OTA — inline no-ops so callers need no #ifdefs

inline bool bootValidate() { return false; }
inline void noteSelfTestPass() {}
inline void begin() {}
inline void requestCheck() {}
inline void requestApply() {}
inline Status status() { return Status{}; }

#endif  // SLYTHERM_OTA

}  // namespace ota
