// RemoteLink.cpp — see RemoteLink.h. Flat-object JSON scanner in the same
// style (and with the same tolerance philosophy) as lib/HaMqtt's parsers:
// find a top-level key, parse its token strictly, reject the whole message
// on any malformed required field.

#include "RemoteLink.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace remote_link {
namespace {

const char* skipWs(const char* p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
  return p;
}

// Find the value position of "key" in a FLAT json object (no nesting needed
// for this contract). Returns nullptr if absent. Skips matches inside string
// values by tracking quotes.
const char* findValue(const char* json, const char* key) {
  const size_t klen = strlen(key);
  bool inStr = false;
  for (const char* p = json; *p; ++p) {
    if (*p == '"' && (p == json || p[-1] != '\\')) {
      if (!inStr && strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
        const char* q = skipWs(p + 2 + klen);
        if (*q == ':') return skipWs(q + 1);
      }
      inStr = !inStr;
    }
  }
  return nullptr;
}

bool numberToken(const char* p, float& out) {
  char* end = nullptr;
  const double v = strtod(p, &end);
  if (end == p || !std::isfinite(v)) return false;
  const char* q = skipWs(end);
  if (*q != ',' && *q != '}' && *q != ']') return false;
  out = static_cast<float>(v);
  return true;
}

bool uint32Token(const char* p, uint32_t& out) {
  if (*p < '0' || *p > '9') return false;
  char* end = nullptr;
  const unsigned long v = strtoul(p, &end, 10);
  if (end == p) return false;
  const char* q = skipWs(end);
  if (*q != ',' && *q != '}' && *q != ']') return false;
  out = static_cast<uint32_t>(v);
  return true;
}

bool boolToken(const char* p, bool& out) {
  if (strncmp(p, "true", 4) == 0) { out = true; return true; }
  if (strncmp(p, "false", 5) == 0) { out = false; return true; }
  return false;
}

bool stringToken(const char* p, std::string& out) {
  if (*p != '"') return false;
  out.clear();
  for (++p; *p && *p != '"'; ++p) {
    if (*p == '\\' && p[1]) ++p;  // contract emits no escapes needing decode beyond skip
    out.push_back(*p);
  }
  return *p == '"';
}

}  // namespace

const char* toString(Mode m) {
  switch (m) {
    case Mode::kHeat: return "heat";
    case Mode::kCool: return "cool";
    case Mode::kHeatCool: return "heat_cool";
    default: return "off";
  }
}

const char* toString(Hold h) {
  switch (h) {
    case Hold::kUntilNextPreset: return "until_next_preset";
    case Hold::kTwoHours: return "two_hours";
    case Hold::kFourHours: return "four_hours";
    case Hold::kIndefinite: return "indefinite";
    default: return "none";
  }
}

bool parseRemoteStateJson(const char* json, ControllerEcho& out) {
  out = ControllerEcho{};
  if (json == nullptr || *skipWs(json) != '{') return false;

  ControllerEcho e;
  const char* v = findValue(json, "heatC");
  if (v == nullptr || !numberToken(v, e.heatC)) return false;
  v = findValue(json, "coolC");
  if (v == nullptr || !numberToken(v, e.coolC)) return false;

  std::string s;
  v = findValue(json, "mode");
  if (v == nullptr || !stringToken(v, s)) return false;
  if (s == "off") e.mode = Mode::kOff;
  else if (s == "heat") e.mode = Mode::kHeat;
  else if (s == "cool") e.mode = Mode::kCool;
  else if (s == "heat_cool") e.mode = Mode::kHeatCool;
  else return false;

  v = findValue(json, "emHeat");
  if (v == nullptr || !boolToken(v, e.emHeat)) return false;

  v = findValue(json, "hold");
  if (v == nullptr || !stringToken(v, s)) return false;
  if (s == "none") e.hold = Hold::kNone;
  else if (s == "until_next_preset") e.hold = Hold::kUntilNextPreset;
  else if (s == "two_hours") e.hold = Hold::kTwoHours;
  else if (s == "four_hours") e.hold = Hold::kFourHours;
  else if (s == "indefinite") e.hold = Hold::kIndefinite;
  else return false;

  v = findValue(json, "holdRemainS");
  if (v == nullptr || !uint32Token(v, e.holdRemainS)) return false;

  v = findValue(json, "activePreset");
  if (v == nullptr || !stringToken(v, e.activePreset)) return false;
  if (e.activePreset == "none") e.activePreset.clear();
  if (e.activePreset.size() > kPresetNameMaxLen) return false;

  v = findValue(json, "fusedTempC");
  if (v == nullptr || !numberToken(v, e.fusedTempC)) return false;
  v = findValue(json, "fusedTempValid");
  if (v == nullptr || !boolToken(v, e.fusedTempValid)) return false;

  out = e;
  return true;
}

bool parseControllerStatusJson(const char* json, ControllerStatus& out) {
  out = ControllerStatus{};
  if (json == nullptr || *skipWs(json) != '{') return false;
  ControllerStatus c;
  const char* v = findValue(json, "cid");
  if (v == nullptr || !stringToken(v, c.cid) || c.cid.empty()) return false;
  std::string s;
  v = findValue(json, "status");
  if (v == nullptr || !stringToken(v, s)) return false;
  c.online = (s == "online");
  v = findValue(json, "version");  // informational; optional-tolerant
  if (v != nullptr) stringToken(v, c.version);
  out = c;
  return true;
}

namespace {
std::string idPrefix(uint32_t id) {
  char b[48];
  snprintf(b, sizeof(b), "{\"id\":%lu,\"type\":\"", static_cast<unsigned long>(id));
  return b;
}
}  // namespace

std::string intentSetpointsJson(uint32_t id, float heatC, float coolC) {
  char b[64];
  snprintf(b, sizeof(b), "setpoints\",\"heatC\":%.1f,\"coolC\":%.1f}",
           static_cast<double>(heatC), static_cast<double>(coolC));
  return idPrefix(id) + b;
}

std::string intentModeJson(uint32_t id, Mode m) {
  return idPrefix(id) + "mode\",\"mode\":\"" + toString(m) + "\"}";
}

std::string intentPresetJson(uint32_t id, const std::string& preset) {
  return idPrefix(id) + "preset\",\"preset\":\"" + preset + "\"}";
}

std::string intentHoldJson(uint32_t id, Hold h) {
  return idPrefix(id) + "hold\",\"hold\":\"" + toString(h) + "\"}";
}

std::string intentClearHoldJson(uint32_t id) {
  return idPrefix(id) + "clear_hold\"}";
}

}  // namespace remote_link
