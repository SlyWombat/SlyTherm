// RemoteLink.h — the REMOTE side of the Remote<->Controller private protocol
// (issue #102; wire contract fixed by the Controller-side #104 implementation
// in lib/HaMqtt — see HaMqtt.h "Remote link" on feature/104-controller-remote-
// link). Pure C++17, no Arduino deps, host-tested (test/test_remote_link).
//
// This lib deliberately holds the MIRROR halves of what HaMqtt holds, so the
// two in-flight branches never edit the same file:
//   HaMqtt (Controller):  remoteStateJson() builder + parseRemoteIntentJson()
//   RemoteLink (Remote):  parseRemoteStateJson() + remoteIntentJson() builder
// Consolidating both pairs into one lib is fine AFTER both branches merge.
//
// ---- The wire contract (authoritative copy in HaMqtt.h; duplicated here
//      because a Remote build doesn't carry the Controller's lib) ----
//
// DOWN  slytherm/remote/state  (retained; ONE shared topic for all Remotes)
//   {"heatC":21.0,"coolC":25.0,"mode":"heat","emHeat":false,
//    "hold":"two_hours","holdRemainS":7032,"activePreset":"home",
//    "fusedTempC":21.3,"fusedTempValid":true}
//   mode: off|heat|cool|heat_cool     hold: none|until_next_preset|
//   two_hours|four_hours|indefinite   activePreset: roster name or "none"
//
// DOWN  slytherm/controller/status  (retained; IDENTITY only, no LWT half)
//   {"cid":"8d82f4","status":"online","version":"..."}
//   Controller LIVENESS is the existing slytherm/availability LWT
//   (online/offline) — PubSubClient supports exactly one Will.
//
// UP    slytherm/remote/<id>/intent  (NOT retained — live-only, no queuing)
//   {"id":<uint32 >0, "type":"setpoints"|"mode"|"preset"|"hold"|"clear_hold",
//    ...} + per-type field: heatC+coolC | mode | preset | hold.
//   id is a Remote-local monotonic sequence starting at 1 (0 = Controller
//   dedupe sentinel, rejected).
#pragma once

#include <cstdint>
#include <string>

namespace remote_link {

// Wire mirrors (string values above). Kept as this lib's own enums — the
// UiModel mapping lives in the Remote's src/ glue, same isolation rule the
// Controller uses between HaMqtt and ui::.
enum class Mode : uint8_t { kOff, kHeat, kCool, kHeatCool };
enum class Hold : uint8_t { kNone, kUntilNextPreset, kTwoHours, kFourHours, kIndefinite };

constexpr size_t kPresetNameMaxLen = 15;  // matches HaMqtt/UiModel kUiPresetNameLen-1

// ---- DOWN: slytherm/remote/state ----
struct ControllerEcho {
  float heatC = 0.0f;
  float coolC = 0.0f;
  Mode  mode = Mode::kOff;
  bool  emHeat = false;
  Hold  hold = Hold::kNone;
  uint32_t holdRemainS = 0;
  std::string activePreset;  // "none" normalized to "" (no active preset)
  float fusedTempC = 0.0f;
  bool  fusedTempValid = false;
  // #116/#118 extras — OPTIONAL on the wire (see parse note below):
  std::string action = "idle";      // idle|heating|cooling|fan|defrosting|off
  std::string equipment = "idle";   // idle|gas_heat|hp_heat|cool|defrost
  uint8_t alarmN = 0;               // active alarm count on the Controller
  std::string alarm1, alarm2;       // first raw texts (render via friendlyAlarm)
  bool vacationActive = false;
  std::string vacBanner;
};

// Strict whole-object parse for the ORIGINAL fields (any missing/malformed
// one rejects the whole echo — a Remote must never reconcile to a
// half-parsed authority state). The #116/#118 extras are the deliberate
// exception: OPTIONAL with safe defaults, so a new Remote accepts an old
// Controller's echo during a mixed-version OTA window.
bool parseRemoteStateJson(const char* json, ControllerEcho& out);

// ---- DOWN: slytherm/state/fusion (#117) ----
//   {"temp":22.87,"tier":"fused_remotes","participants":["living","bedroom"],
//    "occupied":false,"dominant":"living"}   (dominant optional, "" if absent)
constexpr size_t kMaxFusionParticipants = 8;
struct FusionView {
  float temp = 0.0f;
  bool occupied = false;
  char participants[kMaxFusionParticipants][24] = {};
  uint8_t participantCount = 0;
  std::string dominant;  // sensor id currently driving demand ("" = none)
};
bool parseFusionJson(const char* json, FusionView& out);

// ---- DOWN: slytherm/sensors/<id>/presence (#117) ----
//   {"occupied":false, "last_seen":1783481508}
struct PresenceView {
  bool occupied = false;
  uint32_t lastSeenEpoch = 0;  // unix seconds; 0 = absent/unknown
};
bool parsePresenceJson(const char* json, PresenceView& out);

// ---- DOWN: slytherm/controller/status ----
struct ControllerStatus {
  std::string cid;      // MAC-derived, lowercase hex — bind to this, never IP
  bool online = false;  // "status" field; liveness still rides slytherm/availability
  std::string version;
};
bool parseControllerStatusJson(const char* json, ControllerStatus& out);

// ---- UP: slytherm/remote/<id>/intent builders ----
// Emitted JSON matches HaMqtt's parseRemoteIntentJson expectations exactly
// (field order irrelevant, but these are also byte-tested against fixtures).
std::string intentSetpointsJson(uint32_t id, float heatC, float coolC);
std::string intentModeJson(uint32_t id, Mode m);
std::string intentPresetJson(uint32_t id, const std::string& preset);
std::string intentHoldJson(uint32_t id, Hold h);   // h != kNone
std::string intentClearHoldJson(uint32_t id);
// #118: vacation window + eco setpoints; clear; alarm acknowledgement.
std::string intentVacationJson(uint32_t id, uint16_t startDays, uint16_t nights,
                               float heatC, float coolC);
std::string intentClearVacationJson(uint32_t id);
std::string intentAckAlarmsJson(uint32_t id);

const char* toString(Mode m);   // exact wire strings
const char* toString(Hold h);

}  // namespace remote_link
