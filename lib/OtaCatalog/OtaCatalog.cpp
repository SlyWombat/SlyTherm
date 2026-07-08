// OtaCatalog.cpp — see OtaCatalog.h. Pure C++17, no Arduino.
#include "OtaCatalog.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace dettson {
namespace ota {

// ---------- Semver ----------

Semver parseSemver(const char* s) {
  Semver v;
  if (s == nullptr || *s == '\0') return v;
  // Optional '+build' metadata is ignored; '-prerelease' is rejected.
  int parts[3] = {0, 0, 0};
  const char* p = s;
  for (int i = 0; i < 3; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(*p))) return v;
    char* end = nullptr;
    long n = std::strtol(p, &end, 10);
    if (n < 0 || n > 100000) return v;
    parts[i] = static_cast<int>(n);
    p = end;
    if (i < 2) {
      if (*p != '.') return v;
      ++p;
    }
  }
  if (*p != '\0' && *p != '+') return v;  // trailing junk / pre-release tag
  v.major = parts[0];
  v.minor = parts[1];
  v.patch = parts[2];
  v.ok = true;
  return v;
}

int cmpSemver(const Semver& a, const Semver& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  return 0;
}

// ---------- Minimal JSON scanner (duplicated from HaMqtt.cpp by design) ----------
namespace {

const char* skipWs(const char* p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
  return p;
}

// First character of the value for "key": ..., or nullptr.
const char* findValue(const char* json, const char* key) {
  std::string pat = "\"";
  pat += key;
  pat += "\"";
  const char* p = std::strstr(json, pat.c_str());
  while (p != nullptr) {
    const char* q = skipWs(p + pat.size());
    if (*q == ':') return skipWs(q + 1);
    p = std::strstr(p + 1, pat.c_str());
  }
  return nullptr;
}

bool numberToken(const char* q, double& out) {
  char* end = nullptr;
  double v = std::strtod(q, &end);
  if (end == q || !std::isfinite(v)) return false;
  const char* t = skipWs(end);
  if (*t != ',' && *t != '}' && *t != '\0') return false;
  out = v;
  return true;
}

bool keywordToken(const char* q, const char* kw) {
  size_t n = std::strlen(kw);
  if (std::strncmp(q, kw, n) != 0) return false;
  char c = q[n];
  return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_');
}

// JSON string token; \" \\ \/ escapes only; empty strings fail.
bool stringToken(const char* q, std::string& out) {
  if (*q != '"') return false;
  ++q;
  out.clear();
  while (*q != '\0' && *q != '"') {
    if (*q == '\\') {
      ++q;
      if (*q == '"' || *q == '\\' || *q == '/') out += *q++;
      else return false;
    } else if (static_cast<unsigned char>(*q) < 0x20) {
      return false;
    } else {
      out += *q++;
    }
  }
  return *q == '"' && !out.empty();
}

// p at '{': pointer to the matching '}' (string-aware), or nullptr.
const char* objEnd(const char* p) {
  int depth = 0;
  bool inStr = false, esc = false;
  for (; *p != '\0'; ++p) {
    if (inStr) {
      if (esc) esc = false;
      else if (*p == '\\') esc = true;
      else if (*p == '"') inStr = false;
    } else if (*p == '"') {
      inStr = true;
    } else if (*p == '{') {
      ++depth;
    } else if (*p == '}') {
      if (--depth == 0) return p;
    }
  }
  return nullptr;
}

bool isLowerHex(const std::string& s) {
  for (char c : s)
    if (!std::isdigit(static_cast<unsigned char>(c)) && !(c >= 'a' && c <= 'f'))
      return false;
  return true;
}

// Optional string field: absent -> "", present-but-malformed -> reject entry.
bool optionalStr(const char* entry, const char* key, std::string& out, bool& valid) {
  const char* v = findValue(entry, key);
  if (v == nullptr) { out.clear(); return true; }
  if (!stringToken(v, out)) { valid = false; return false; }
  return true;
}

}  // namespace

// ---------- Catalog ----------

bool parseCatalogJson(const char* json, std::vector<CatalogEntry>& out) {
  out.clear();
  if (json == nullptr) return false;
  if (*skipWs(json) != '{') return false;

  double schema = 0;
  const char* sv = findValue(json, "schema");
  if (sv == nullptr || !numberToken(sv, schema) ||
      static_cast<int>(schema) != kCatalogSchema) {
    return false;
  }

  const char* p = findValue(json, "targets");
  if (p == nullptr || *p != '[') return false;
  p = skipWs(p + 1);
  while (*p != ']') {
    if (*p != '{') { out.clear(); return false; }  // structural junk
    const char* e = objEnd(p);
    if (e == nullptr) { out.clear(); return false; }
    const std::string entry(p, e + 1);  // nul-terminated slice for findValue

    CatalogEntry ce;
    bool valid = true;

    // Required strings.
    const char* v = findValue(entry.c_str(), "id");
    if (v == nullptr || !stringToken(v, ce.id)) valid = false;
    v = findValue(entry.c_str(), "hwRev");
    if (valid && (v == nullptr || !stringToken(v, ce.hwRev))) valid = false;
    v = findValue(entry.c_str(), "version");
    if (valid && (v == nullptr || !stringToken(v, ce.version) ||
                  !parseSemver(ce.version.c_str()).ok)) valid = false;
    v = findValue(entry.c_str(), "appUrl");
    if (valid && (v == nullptr || !stringToken(v, ce.appUrl))) valid = false;
    v = findValue(entry.c_str(), "sha256");
    if (valid && (v == nullptr || !stringToken(v, ce.sha256) ||
                  ce.sha256.size() != kSha256HexLen || !isLowerHex(ce.sha256)))
      valid = false;

    // Optional fields (absent OK; present-but-malformed rejects the entry).
    if (valid) optionalStr(entry.c_str(), "name", ce.name, valid);
    if (valid) optionalStr(entry.c_str(), "chipFamily", ce.chipFamily, valid);
    if (valid) optionalStr(entry.c_str(), "sig", ce.sig, valid);
    if (valid) optionalStr(entry.c_str(), "notesUrl", ce.notesUrl, valid);
    if (valid) {
      if (optionalStr(entry.c_str(), "minVersion", ce.minVersion, valid) &&
          valid && !ce.minVersion.empty() &&
          !parseSemver(ce.minVersion.c_str()).ok)
        valid = false;
    }
    if (valid) {
      const char* n = findValue(entry.c_str(), "appSize");
      double sz = 0;
      if (n != nullptr) {
        if (numberToken(n, sz) && sz >= 0 && sz <= 4294967295.0)
          ce.appSize = static_cast<uint32_t>(sz);
        else
          valid = false;
      }
    }
    if (valid) {
      const char* m = findValue(entry.c_str(), "mandatory");
      if (m != nullptr) {
        if (keywordToken(m, "true")) ce.mandatory = true;
        else if (keywordToken(m, "false")) ce.mandatory = false;
        else valid = false;
      }
    }
    if (valid) {
      for (const CatalogEntry& prev : out) {
        if (prev.id == ce.id && prev.hwRev == ce.hwRev) { valid = false; break; }
      }
    }
    if (valid) out.push_back(ce);

    p = skipWs(e + 1);
    if (*p == ',') p = skipWs(p + 1);
    else if (*p != ']') { out.clear(); return false; }
  }
  return true;  // an empty targets array is a legitimate "nothing published"
}

Resolution resolve(const std::vector<CatalogEntry>& entries,
                   const char* targetId, const char* hwRev,
                   const char* runningVersion) {
  Resolution r;
  if (targetId == nullptr || hwRev == nullptr) return r;
  for (const CatalogEntry& e : entries) {
    if (e.id != targetId || e.hwRev != hwRev) continue;
    r.targetFound = true;
    r.entry = e;
    const Semver run = parseSemver(runningVersion ? runningVersion : "");
    const Semver avail = parseSemver(e.version.c_str());
    if (!run.ok || !avail.ok) return r;  // unknown running version: never offer
    if (cmpSemver(avail, run) <= 0) return r;  // up to date (or ahead of catalog)
    const Semver minv = e.minVersion.empty() ? parseSemver("0.0.0")
                                             : parseSemver(e.minVersion.c_str());
    if (minv.ok && cmpSemver(run, minv) < 0) {
      r.blockedByMinVersion = true;  // needs a stepping-stone release first
      return r;
    }
    r.updateAvailable = true;
    return r;
  }
  return r;
}

}  // namespace ota
}  // namespace dettson
