// FurnaceMenu.h — the furnace's own status pages (CT-485 Get User Menu,
// 0x41 request / 0xC1 reply), kept for the wall UI, Home Assistant and the
// post-switch-back poller. SlyTherm #221 (decoder), #226 (Diag page), #227 (HA
// sensors), #228 (polling), #229 (no writes).
//
// Three pure pieces, no Arduino, no heap beyond std::string/vector, no clock:
//
//   FurnaceMenuStore   — last reply per page index, with when it was heard and
//                        whether it answered SlyTherm's own request. Fed every
//                        decoded 0xC1 frame on the bus; works in listen mode,
//                        where the pages arrive only while someone has the
//                        Furnace menu open on the OEM thermostat.
//   furnaceStatusValues — the handful of readings HA gets (gas valve %, flame,
//                        blower CFM, inducer rpm, mode, motor rpm, air cleaner),
//                        picked by LABEL, never by row position.
//   FurnaceMenuPoller  — cadence for SlyTherm asking for Status 1 + 2 itself
//                        (#228). Decides only WHEN; the caller still has to
//                        get the frame past Ct485Thermostat's gates (silent in
//                        listen mode, grant discipline, read-only whitelist).
//
// The page layout is the furnace's: titles, labels and row order come from
// the reply, so a page format not yet seen renders instead of breaking.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Ct485Core.h"
#include "Ct485Parser.h"

namespace ct485 {

// Page indices seen on the Dettson C105-MV (2026-10-18). Index 0 is the menu
// list; the furnace advertises the rest in it, so names come from the reply
// when it has been heard and from this table otherwise.
constexpr uint8_t kFurnaceMenuPages = 8;  // 0 = list, 1..7 = pages
constexpr uint8_t kFurnacePageStatus1 = 1;
constexpr uint8_t kFurnacePageStatus2 = 2;
const char* defaultFurnacePageName(uint8_t page);  // "STATUS 1"... or "PAGE n"

class FurnaceMenuStore {
 public:
  // Store a 0xC1 reply. `answeredUs` = it was addressed to SlyTherm's own node
  // (a reply to our #228 poll) rather than overheard. Returns false (and
  // stores nothing) for anything that is not a recognisable menu page.
  bool ingest(const Frame& f, uint32_t nowMs, bool answeredUs = false);

  bool seen(uint8_t page) const;
  uint32_t heardMs(uint8_t page) const;      // 0 when never seen
  bool answeredUs(uint8_t page) const;
  uint32_t generation() const { return gen_; }        // +1 per stored page
  uint32_t pageGeneration(uint8_t page) const;        // gen at its last store
  const UserMenuPage* page(uint8_t page) const;       // nullptr if never seen

  // Number of pages to offer in a page list (from the menu list when heard,
  // else the 7 known ones) and each one's display name.
  uint8_t pageCount() const;
  std::string pageName(uint8_t page) const;

  // Plain text for the wall Diag screen: a header line (name + age + source)
  // then one "LABEL  value" line per row; editable rows list their other
  // choices but are never presented as editable. Never-seen pages explain how
  // to make them appear.
  std::string renderPage(uint8_t page, uint32_t nowMs) const;

 private:
  struct Slot {
    bool seen = false;
    bool answeredUs = false;
    uint32_t heardMs = 0;
    uint32_t gen = 0;
    UserMenuPage page;
  };
  Slot slots_[kFurnaceMenuPages];
  uint32_t gen_ = 0;
};

// "12 s", "4 min", "3 h", "2 d" — the age wording the Diag screen uses.
std::string formatAge(uint32_t ageMs);

// One HA reading carried by a page. `key` is the topic leaf under
// slytherm/state/furnace/ and the HA unique-id suffix.
struct FurnaceStatusValue {
  const char* key;
  std::string value;  // numeric keys: integer text ("100"); others: "GOOD", "AC", "ON"
};

// Readings a decoded page carries, by label. Values that do not parse as
// expected are skipped, never published as garbage. Keys:
//   gas_valve_pct  (GAS VLV PRCNT)   flame      (FLAME)
//   blower_cfm     (BLOWER CFM)      inducer_rpm (APPROX IDM RPM, OFF -> 0)
//   mode           (MODE)            motor_rpm  (MOTOR RPM)
//   eac            (EAC OUTPUT, ON/OFF)
std::vector<FurnaceStatusValue> furnaceStatusValues(const UserMenuPage& p);

// #228 cadence: Status 1 then Status 2, every activeIntervalMs while any
// demand is nonzero, every idleIntervalMs otherwise. One request at a time;
// an unanswered request counts a miss after answerWindowMs. missLimit
// consecutive misses = stalled: alarm-worthy, and the next attempt waits
// backoffMs instead of retrying hot.
class FurnaceMenuPoller {
 public:
  struct Config {
    uint32_t activeIntervalMs = 30000;
    uint32_t idleIntervalMs   = 300000;
    uint32_t answerWindowMs   = 15000;   // request waits for a grant, then ~1 s reply
    uint8_t  missLimit        = 3;
    uint32_t backoffMs        = 900000;
  };
  FurnaceMenuPoller() : FurnaceMenuPoller(Config{}) {}
  explicit FurnaceMenuPoller(const Config& c) : cfg_(c) {}

  // Page to request now (1 or 2), or 0. `permitted` = polling enabled AND the
  // bus is ours to talk on (not listen, not silent, addressed, furnace known);
  // while false the poller forgets any request in flight and stays idle.
  uint8_t due(uint32_t nowMs, bool permitted, bool demandActive);
  void onRequested(uint8_t page, uint32_t nowMs);   // the request was queued
  void onPageHeard(uint8_t page, uint32_t nowMs);   // a reply for that page arrived

  bool stalled() const { return stalled_; }
  uint32_t misses() const { return misses_; }       // consecutive
  uint32_t requests() const { return requests_; }   // total queued
  uint8_t pending() const { return pending_; }

 private:
  void finishStep(uint32_t nowMs);
  Config cfg_;
  bool everCycled_ = false;
  bool stalled_ = false;
  bool demandActive_ = false;
  uint8_t next_ = 0;       // next page in the current cycle, 0 = between cycles
  uint8_t pending_ = 0;    // page requested and not yet answered
  uint32_t pendingSinceMs_ = 0;
  uint32_t cycleEndMs_ = 0;
  uint32_t holdUntilMs_ = 0;   // backoff after a stall
  uint32_t misses_ = 0;
  uint32_t requests_ = 0;
};

}  // namespace ct485
