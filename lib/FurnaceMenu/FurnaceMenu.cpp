// FurnaceMenu.cpp — see FurnaceMenu.h.

#include "FurnaceMenu.h"

#include <cstdio>
#include <cstring>

namespace ct485 {

namespace {

bool timeReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

// Integer text of a numeric menu value; false when the value is not a number.
bool intText(const MenuRow& r, std::string& out) {
  if (!r.numeric) return false;
  char b[16];
  snprintf(b, sizeof(b), "%ld", static_cast<long>(r.number < 0 ? r.number - 0.5f : r.number + 0.5f));
  out = b;
  return true;
}

}  // namespace

const char* defaultFurnacePageName(uint8_t page) {
  static const char* const kNames[kFurnaceMenuPages] = {
      "MENU", "STATUS 1", "STATUS 2", "2WK HIST", "LIFE HIST",
      "FAULT HIST", "UNIT INFO", "SETUP"};
  return page < kFurnaceMenuPages ? kNames[page] : "PAGE";
}

std::string formatAge(uint32_t ageMs) {
  const uint32_t s = ageMs / 1000;
  char b[24];
  if (s < 60) snprintf(b, sizeof(b), "%lu s", static_cast<unsigned long>(s));
  else if (s < 3600) snprintf(b, sizeof(b), "%lu min", static_cast<unsigned long>(s / 60));
  else if (s < 86400) snprintf(b, sizeof(b), "%lu h", static_cast<unsigned long>(s / 3600));
  else snprintf(b, sizeof(b), "%lu d", static_cast<unsigned long>(s / 86400));
  return b;
}

// ---------------------------------------------------------------- store

bool FurnaceMenuStore::ingest(const Frame& f, uint32_t nowMs, bool answeredUs) {
  if (f.msgType != (static_cast<uint8_t>(MsgType::kGetUserMenu) | kResponseFlag)) return false;
  UserMenuPage p = decodeUserMenuPage(f);
  if (!p.ok || p.page >= kFurnaceMenuPages) return false;
  // A page that decoded to nothing (torn, or a format we cannot read) must not
  // replace a good copy: keep the last readable one.
  const bool readable = p.page == 0 ? !p.menuTitles.empty() : !p.rows.empty();
  if (!readable) return false;
  Slot& s = slots_[p.page];
  s.seen = true;
  s.answeredUs = answeredUs;
  s.heardMs = nowMs;
  s.gen = ++gen_;
  s.page = std::move(p);
  return true;
}

bool FurnaceMenuStore::seen(uint8_t page) const {
  return page < kFurnaceMenuPages && slots_[page].seen;
}

uint32_t FurnaceMenuStore::heardMs(uint8_t page) const {
  return seen(page) ? slots_[page].heardMs : 0;
}

bool FurnaceMenuStore::answeredUs(uint8_t page) const {
  return seen(page) && slots_[page].answeredUs;
}

uint32_t FurnaceMenuStore::pageGeneration(uint8_t page) const {
  return seen(page) ? slots_[page].gen : 0;
}

const UserMenuPage* FurnaceMenuStore::page(uint8_t page) const {
  return seen(page) ? &slots_[page].page : nullptr;
}

uint8_t FurnaceMenuStore::pageCount() const {
  if (seen(0)) {
    const size_t n = slots_[0].page.menuTitles.size();
    if (n > 0) return static_cast<uint8_t>(n < kFurnaceMenuPages - 1 ? n : kFurnaceMenuPages - 1);
  }
  return kFurnaceMenuPages - 1;
}

std::string FurnaceMenuStore::pageName(uint8_t page) const {
  if (page == 0) return defaultFurnacePageName(0);
  if (seen(0) && page <= slots_[0].page.menuTitles.size())
    return slots_[0].page.menuTitles[page - 1];
  if (seen(page) && !slots_[page].page.title.empty()) return slots_[page].page.title;
  return defaultFurnacePageName(page);
}

std::string FurnaceMenuStore::renderPage(uint8_t page, uint32_t nowMs) const {
  std::string out = pageName(page);
  if (!seen(page)) {
    out += "\nNot heard since the controller started.\n\n"
           "The furnace sends this page only when it is asked for. Open\n"
           "Installer Menu > Communicating Devices > Furnace > " + pageName(page) +
           "\non the OEM thermostat and it appears here within ~10 s.";
    return out;
  }
  const Slot& s = slots_[page];
  out += "    heard " + formatAge(nowMs - s.heardMs) + " ago, ";
  out += s.answeredUs ? "answering SlyTherm" : "via the OEM thermostat";
  if (!s.page.complete) out += "  (partial page)";
  out += "\n";
  for (const MenuRow& r : s.page.rows) {
    out += "\n" + r.label + "   " + r.value;
    if (r.editable && !r.options.empty()) {
      out += "   (set on the OEM thermostat; also ";
      for (size_t i = 0; i < r.options.size(); i++) {
        if (i) out += ", ";
        out += r.options[i];
      }
      out += ")";
    }
  }
  return out;
}

// ---------------------------------------------------------------- HA values

std::vector<FurnaceStatusValue> furnaceStatusValues(const UserMenuPage& p) {
  std::vector<FurnaceStatusValue> out;
  for (const MenuRow& r : p.rows) {
    std::string v;
    if (r.label == "GAS VLV PRCNT") {
      if (intText(r, v)) out.push_back({"gas_valve_pct", v});
    } else if (r.label == "BLOWER CFM") {
      if (intText(r, v)) out.push_back({"blower_cfm", v});
    } else if (r.label == "APPROX IDM RPM") {
      if (r.value == "OFF") out.push_back({"inducer_rpm", "0"});
      else if (intText(r, v)) out.push_back({"inducer_rpm", v});
    } else if (r.label == "MOTOR RPM") {
      if (intText(r, v)) out.push_back({"motor_rpm", v});
    } else if (r.label == "FLAME") {
      if (!r.value.empty()) out.push_back({"flame", r.value});
    } else if (r.label == "MODE") {
      if (!r.value.empty()) out.push_back({"mode", r.value});
    } else if (r.label == "EAC OUTPUT") {
      if (r.value == "ON" || r.value == "OFF") out.push_back({"eac", r.value});
    }
  }
  return out;
}

// ---------------------------------------------------------------- poller

uint8_t FurnaceMenuPoller::due(uint32_t nowMs, bool permitted, bool demandActive) {
  demandActive_ = demandActive;
  if (!permitted) {           // bus not ours: forget the cycle, start fresh later
    pending_ = 0;
    next_ = 0;
    return 0;
  }
  if (pending_ != 0) {
    if (!timeReached(nowMs, pendingSinceMs_ + cfg_.answerWindowMs)) return 0;
    pending_ = 0;             // unanswered: a miss
    misses_++;
    if (misses_ >= cfg_.missLimit) {
      stalled_ = true;
      next_ = 0;
      holdUntilMs_ = nowMs + cfg_.backoffMs;
      cycleEndMs_ = nowMs;
      return 0;
    }
    finishStep(nowMs);
  }
  if (next_ == 0) {
    if (stalled_ && !timeReached(nowMs, holdUntilMs_)) return 0;
    const uint32_t interval = demandActive ? cfg_.activeIntervalMs : cfg_.idleIntervalMs;
    if (everCycled_ && !timeReached(nowMs, cycleEndMs_ + interval)) return 0;
    next_ = kFurnacePageStatus1;
  }
  return next_;
}

void FurnaceMenuPoller::onRequested(uint8_t page, uint32_t nowMs) {
  if (page == 0 || page != next_) return;
  pending_ = page;
  pendingSinceMs_ = nowMs;
  requests_++;
}

void FurnaceMenuPoller::onPageHeard(uint8_t page, uint32_t nowMs) {
  if (pending_ == 0 || page != pending_) return;
  pending_ = 0;
  misses_ = 0;
  stalled_ = false;
  finishStep(nowMs);
}

void FurnaceMenuPoller::finishStep(uint32_t nowMs) {
  if (next_ == kFurnacePageStatus1) {
    next_ = kFurnacePageStatus2;
  } else {
    next_ = 0;
    everCycled_ = true;
    cycleEndMs_ = nowMs;
  }
}

}  // namespace ct485
