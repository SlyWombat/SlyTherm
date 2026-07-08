// OtaCatalog.h — firmware catalog parsing + semver compare for OTA (#60/#113).
//
// Parses firmware/catalog.json (served from the GitHub repo, docs/10) and
// resolves this device's target + hwRev + running version to an update
// decision. Consumed by src/ota_client.cpp on BOTH roles (Controller wall-s3,
// Remote remote-p4); the safety gating around *applying* an update is the
// glue's job (#61/#62), never this module's.
//
// Same tolerance philosophy as lib/HaMqtt's parsers: reject-don't-guess.
// A malformed document yields no entries; a malformed entry is skipped; a
// rejected parse never produces a half-usable result. Unknown keys are
// ignored so the schema can grow compatibly within schema:1.
//
// Pure C++17, no Arduino dependencies, host-testable. The tiny JSON scanner
// is deliberately duplicated from HaMqtt.cpp (anonymous namespace there) —
// keeping this lib self-contained mirrors the UiModel deadband precedent.

#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dettson {
namespace ota {

// ---------- Semver (#113) ----------
// Strict MAJOR.MINOR.PATCH, each a bare non-negative decimal integer.
// A build-metadata suffix introduced by '+' is ignored ("0.3.0+g1a2b3c4"
// compares as 0.3.0); pre-release tags ('-') are NOT supported and fail the
// parse — the release train only ever publishes clean X.Y.Z.
struct Semver {
  int major = 0, minor = 0, patch = 0;
  bool ok = false;
};
Semver parseSemver(const char* s);

// Precondition: both ok. Returns <0, 0, >0 like strcmp.
int cmpSemver(const Semver& a, const Semver& b);

// ---------- Catalog (#60) ----------
constexpr int kCatalogSchema = 1;
constexpr size_t kSha256HexLen = 64;

struct CatalogEntry {
  std::string id;          // target id, e.g. "wall-s3" (required)
  std::string name;        // human label (optional)
  std::string chipFamily;  // informational (optional)
  std::string hwRev;       // hardware revision gate, e.g. "s3-43b-r1.1" (required)
  std::string version;     // latest published semver for this target (required, valid)
  std::string appUrl;      // release-asset URL of the app-only image (required)
  std::string sha256;      // lowercase hex digest of the app image (required, 64 hex)
  std::string sig;         // base64 ECDSA-P256 over the sha256 (optional here;
                           //   the Controller REJECTS an empty sig at apply, #62)
  std::string minVersion;  // oldest running version allowed to jump (optional -> 0.0.0)
  std::string notesUrl;    // release notes link (optional)
  uint32_t appSize = 0;    // bytes (optional; 0 = unknown)
  bool mandatory = false;  // auto-apply hint (#61; Controller still gates, #62)
};

// Returns false (out empty) unless the document is an object with
// "schema": 1 and a "targets" array. Entries missing a required field,
// carrying an invalid version/minVersion/sha256, or duplicating an earlier
// (id, hwRev) pair are skipped. An empty targets array parses true.
bool parseCatalogJson(const char* json, std::vector<CatalogEntry>& out);

// ---------- Resolution ----------
struct Resolution {
  bool targetFound = false;        // an entry matched (id, hwRev) exactly
  bool updateAvailable = false;    // entry.version > running, min-version ok
  bool blockedByMinVersion = false;// newer exists but running < minVersion
  CatalogEntry entry;              // valid when targetFound
};

// Exact-match on (targetId, hwRev) — an incompatible image is never offered
// (docs/10; a new board revision is a NEW catalog entry). runningVersion must
// be valid semver or the result is targetFound-only (never updateAvailable).
Resolution resolve(const std::vector<CatalogEntry>& entries,
                   const char* targetId, const char* hwRev,
                   const char* runningVersion);

}  // namespace ota
}  // namespace dettson
