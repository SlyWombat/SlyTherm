// ota_client.cpp — see ota_client.h. Entirely compiled out without
// -DSLYTHERM_OTA (the header provides inline no-ops).
//
// Design notes (#61 issue plan):
// - HTTPClient + Update.h rather than raw esp_https_ota: streaming lets the
//   sha256 be computed as bytes arrive, redirects to
//   objects.githubusercontent.com are followed, and the identical code
//   compiles on Arduino core 2.x (S3) and 3.x (P4).
// - Rollback is APP-LEVEL: stock Arduino cores don't enable bootloader
//   rollback, so bootValidate() + the boot-attempt counter + the self-test
//   confirm/rollback protocol below stand in for it.
// - The mqttTask owns all MQTT I/O; this module only exposes status() and
//   accepts request*() flags. The reboot itself is gated on
//   otaSafeToReboot() (#62) — provided by main_thermostat.cpp (furnace idle)
//   or main_remote.cpp (always true).

#ifdef SLYTHERM_OTA

#include "ota_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <esp_heap_caps.h>  // largest-free-block in failHttp diagnostics
#include <esp_ota_ops.h>

#include <lwip/sockets.h>  // #180: SO_RCVBUF before the GET (see openMirrorGet)

#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

#include <cstring>
#include <ctime>
#include <string>

#include "OtaCatalog.h"
#include "ota_certs.h"

// Target identity + version (injected via platformio.ini / version_flag.py).
#ifndef SLYTHERM_OTA_TARGET
#error "SLYTHERM_OTA needs -DSLYTHERM_OTA_TARGET (e.g. \"wall-s3\")"
#endif
#ifndef SLYTHERM_OTA_HWREV
#error "SLYTHERM_OTA needs -DSLYTHERM_OTA_HWREV (e.g. \"s3-43b-r1.1\")"
#endif
#ifndef SLYTHERM_FW_VERSION
#define SLYTHERM_FW_VERSION "0.0.0-dev"
#endif
// Overridable for bench testing against a local HTTPS server.
#ifndef SLYTHERM_OTA_CATALOG_URL
#define SLYTHERM_OTA_CATALOG_URL \
  "https://raw.githubusercontent.com/SlyWombat/SlyTherm/main/firmware/catalog.json"
#endif

extern "C" bool otaSafeToReboot();


namespace ota {
namespace {

constexpr uint32_t kTaskStack = 16384;   // TLS handshake + parse headroom
constexpr UBaseType_t kTaskPrio = 1;     // below MQTT/control
constexpr uint32_t kCheckIntervalMs = 24u * 60u * 60u * 1000u;  // daily
constexpr uint32_t kCheckRetryMs = 5u * 60u * 1000u;  // base retry after a failure
// #129: a check that crashes the P4's hosted-WiFi transport must not re-fire
// seconds after boot (observed crash-loop: panic at 10s uptime, bootCount 3).
// Auto checks wait out this grace; an operator's explicit check does not.
constexpr uint32_t kBootCheckGraceMs = 2u * 60u * 1000u;
constexpr uint32_t kCheckRetryMaxMs = 60u * 60u * 1000u;  // backoff ceiling
constexpr uint32_t kSelfTestTimeoutMs = 5u * 60u * 1000u;       // 5 min
constexpr uint8_t kMaxBootAttempts = 3;
constexpr uint8_t kMaxApplyAttempts = 3;  // mid-apply resets before going manual-only
constexpr size_t kCatalogMaxBytes = 16 * 1024;
constexpr uint32_t kStreamTimeoutMs = 30 * 1000;  // stall watchdog per read
// #180: small TCP receive window for the tunnelled mirror download — see
// openMirrorGet(). Set BEFORE the GET so the request advertises it and the
// server can't flood past the esp-hosted SDIO RX pool (sdio_drv.c:953). MSS is
// 1436; ~4 KB keeps it well under the pool.
constexpr int kOtaRcvBufBytes = 4096;
// #182: mirror Range-chunk size — the structural in-flight bound for the P4's
// tunnelled download. ~12 TCP segments per chunk, far under the esp-hosted
// SDIO RX pool; at the field tunnel's ~90 ms RTT the ~117 round-trips for a
// 1.9 MB image cost ~10 s. Retries: consecutive zero-progress rounds before
// giving up (transient drops resume at the current offset for free).
constexpr size_t kMirrorRangeBytes = 16 * 1024;
constexpr uint8_t kMirrorRangeRetries = 8;

// NVS namespace "ota": pend = version being validated ("" = none),
// tries = boot attempts while pending, last = last outcome (for the UI).
constexpr const char* kNvsNs = "ota";

portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
Status gStatus;
dettson::ota::CatalogEntry gAvailable;   // task-local once resolved
volatile bool gCheckReq = false;
volatile bool gApplyReq = false;
volatile bool gForceApply = false;  // #182: clear the crash-loop guard once
volatile bool gSelfTestPass = false;
bool gPendingAtBoot = false;             // set by bootValidate()
uint32_t gBootMs = 0;

void setStatus(State s, uint8_t pct, const char* avail, const char* err) {
  portENTER_CRITICAL(&gMux);
  gStatus.state = s;
  gStatus.progressPct = pct;
  if (avail) strlcpy(gStatus.available, avail, sizeof(gStatus.available));
  if (err) strlcpy(gStatus.error, err, sizeof(gStatus.error));
  portEXIT_CRITICAL(&gMux);
}

void fail(const char* err) {
  Serial.printf("[ota] FAIL: %s\n", err);
  setStatus(State::kFailed, 0, nullptr, err);
}

// ---- sha256 across mbedtls 2.x (core 2.x) / 3.x (core 3.x) ----
struct Sha256 {
  mbedtls_sha256_context c;
  Sha256() {
    mbedtls_sha256_init(&c);
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256_starts(&c, 0);
#else
    mbedtls_sha256_starts_ret(&c, 0);
#endif
  }
  ~Sha256() { mbedtls_sha256_free(&c); }
  void update(const uint8_t* d, size_t n) {
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256_update(&c, d, n);
#else
    mbedtls_sha256_update_ret(&c, d, n);
#endif
  }
  void finish(uint8_t out[32]) {
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256_finish(&c, out);
#else
    mbedtls_sha256_finish_ret(&c, out);
#endif
  }
};

bool hexEqual(const uint8_t digest[32], const std::string& hex) {
  if (hex.size() != 64) return false;
  static const char* kHex = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    if (hex[2 * i] != kHex[digest[i] >> 4] ||
        hex[2 * i + 1] != kHex[digest[i] & 0xF])
      return false;
  }
  return true;
}

// ECDSA-P256 over the image sha256, DER signature, base64 in the catalog.
// An empty/invalid signature REJECTS — sha256 alone is integrity, not
// authenticity (#62; CI signs with the OTA_SIGNING_KEY_PEM secret).
bool signatureOk(const uint8_t digest[32], const std::string& sigB64) {
  if (sigB64.empty()) return false;
  uint8_t sig[96];  // DER ECDSA-P256 sig is ~70-72 bytes
  size_t sigLen = 0;
  if (mbedtls_base64_decode(sig, sizeof(sig), &sigLen,
                            reinterpret_cast<const uint8_t*>(sigB64.data()),
                            sigB64.size()) != 0)
    return false;
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  const auto* key = reinterpret_cast<const uint8_t*>(ota_certs::kOtaSigningPubKey);
  const size_t keyLen = strlen(ota_certs::kOtaSigningPubKey) + 1;  // incl. nul
  bool ok = false;
  if (mbedtls_pk_parse_public_key(&pk, key, keyLen) == 0) {
    ok = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest, 32, sig, sigLen) == 0;
  }
  mbedtls_pk_free(&pk);
  return ok;
}

// TLS certificate validation needs a sane wall clock; before NTP syncs every
// handshake is doomed (notBefore in the future). Gate + say so instead of
// reporting an opaque connection error.
bool clockSynced() { return time(nullptr) > 1600000000; }

// Compose "phase: HTTP <code> (tls=<mbedtls detail>, heap=<free>/<largest>)"
// so the REAL failure reason reaches the MQTT status without a serial cable.
// tls may be null (plain-HTTP mirror path). largest = biggest contiguous
// internal block: mbedtls wants ~48KB contiguous, so free=95k largest=20k
// reads as fragmentation, while free=95k largest=80k points at the transport
// (#129 P4 hosted-WiFi SDIO).
void failHttp(const char* phase, int code, WiFiClientSecure* tls) {
  char tlsErr[48] = "";
  int rc = 0;
  if (tls) rc = tls->lastError(tlsErr, sizeof(tlsErr));
  char b[sizeof(Status{}.error)];
  snprintf(b, sizeof(b), "%s: HTTP %d (tls=%d %s heap=%u/%u)", phase, code, rc,
           tlsErr, static_cast<unsigned>(ESP.getFreeHeap()),
           static_cast<unsigned>(
               heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  fail(b);
}

// ---- OTA mirror (#129 escape hatch) ----
// A LAN mirror (e.g. http://kdocker2:8090) removes GitHub TLS from the OTA
// path entirely — the P4 Remotes' dominant failure is the TLS handshake on
// the 2KB catalog fetch, not image size. Integrity is carried by the ECDSA
// signature + sha256 in the catalog, not by the transport, so plain HTTP on
// the trusted LAN is acceptable. Set/clear via MQTT cmd ota_mirror; persisted
// in NVS; empty = GitHub direct (default).
char gMirror[96] = "";

void loadMirror() {
  Preferences p;
  p.begin(kNvsNs, true);
  p.getString("mirror", gMirror, sizeof(gMirror));
  p.end();
  if (gMirror[0])
    Serial.printf("[ota] mirror: %s\n", gMirror);
}

// <mirror>/catalog.json, or the compiled GitHub URL when no mirror is set.
void catalogUrl(char* out, size_t n) {
  if (gMirror[0]) snprintf(out, n, "%s/catalog.json", gMirror);
  else snprintf(out, n, "%s", SLYTHERM_OTA_CATALOG_URL);
}

// Mirror serves release assets flat by their original basename.
void assetUrl(char* out, size_t n, const std::string& upstream) {
  if (!gMirror[0]) { snprintf(out, n, "%s", upstream.c_str()); return; }
  const char* base = strrchr(upstream.c_str(), '/');
  snprintf(out, n, "%s%s", gMirror, base ? base : "/");
}

// ---- catalog check ----
bool doCheck() {
  setStatus(State::kChecking, 0, "", "");
  char url[160];
  catalogUrl(url, sizeof(url));
  const bool secure = strncmp(url, "http://", 7) != 0;
  if (secure && !clockSynced()) {  // cert validation needs a wall clock
    fail("catalog: clock unsynced (NTP pending) — will retry");
    return false;
  }
  Serial.printf("[ota] check: heap=%u\n", static_cast<unsigned>(ESP.getFreeHeap()));
  WiFiClient plain;
  WiFiClientSecure tls;
  if (secure) tls.setCACert(ota_certs::kOtaCaBundle);
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setConnectTimeout(15000);
  http.setTimeout(15000);
  if (!http.begin(secure ? static_cast<WiFiClient&>(tls) : plain, url)) {
    fail("catalog: http.begin failed");
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    failHttp("catalog", code, secure ? &tls : nullptr);
    return false;
  }
  const int len = http.getSize();
  if (len > 0 && static_cast<size_t>(len) > kCatalogMaxBytes) {
    http.end();
    fail("catalog: too large");
    return false;
  }
  std::string body = http.getString().c_str();
  http.end();

  std::vector<dettson::ota::CatalogEntry> entries;
  if (!dettson::ota::parseCatalogJson(body.c_str(), entries)) {
    fail("catalog: parse rejected");
    return false;
  }
  const auto r = dettson::ota::resolve(entries, SLYTHERM_OTA_TARGET,
                                       SLYTHERM_OTA_HWREV, SLYTHERM_FW_VERSION);
  if (!r.targetFound) {
    fail("catalog: no entry for this target/hwRev");
    return false;
  }
  if (r.blockedByMinVersion) {
    fail("catalog: blocked by minVersion (needs stepping-stone release)");
    return false;
  }
  if (!r.updateAvailable) {
    Serial.printf("[ota] up to date (%s)\n", SLYTHERM_FW_VERSION);
    setStatus(State::kUpToDate, 0, "", "");
    return true;
  }
  gAvailable = r.entry;
  Serial.printf("[ota] update available: %s -> %s\n", SLYTHERM_FW_VERSION,
                gAvailable.version.c_str());
  setStatus(State::kUpdateAvailable, 0, gAvailable.version.c_str(), "");
  return true;
}

// ---- mirror HTTP (plain http://, no redirects — the LAN mirror serves
// directly; HTTPS keeps HTTPClient since GitHub-direct isn't tunnelled) ----
//
// #180 established that read SPEED can't be the whole answer; #182 proved it
// in the field: lwIP always advertises the 64 KB default window
// (CONFIG_LWIP_TCP_WND_DEFAULT — compile-time in the prebuilt esp-hosted
// libs; SO_RCVBUF provably does not shrink it), so at WAN RTT (~90 ms on the
// off-LAN camera remote's tunnel) the server fills the whole window and ~45
// segments land as one line-rate burst that overflows the esp-hosted SDIO RX
// pool (sdio_drv.c:953) no matter how fast we read. The bench validated #180
// only because its WG endpoint was on-LAN (~2 ms RTT — no burst). In-flight
// data must be bounded STRUCTURALLY: the P4 requests the image in
// kMirrorRangeBytes `Range:` chunks on a keep-alive connection, so the server
// can never have more than one chunk (~12 segments) in flight. A server
// without Range support answers 200 — fall back to the #180 single-GET path
// (works on-LAN; the mirror's nginx pacing covers the tunnel case).

struct MirrorUrl {
  char host[96];
  char path[96];
  uint16_t port = 80;
};

bool parseMirrorUrl(const char* url, MirrorUrl& m) {
  const char* p = url + 7;  // past "http://"
  const char* slash = strchr(p, '/');
  const size_t hl = slash ? static_cast<size_t>(slash - p) : strlen(p);
  if (hl == 0 || hl >= sizeof(m.host)) return false;
  memcpy(m.host, p, hl);
  m.host[hl] = 0;
  snprintf(m.path, sizeof(m.path), "%s", slash ? slash : "/");
  if (char* colon = strchr(m.host, ':')) { *colon = 0; m.port = atoi(colon + 1); }
  return true;
}

bool mirrorConnect(WiFiClient& c, const MirrorUrl& m) {
  if (!c.connect(m.host, m.port, 15000)) return false;
  // Kept from #180: harmless, and on lwIPs where it DOES clamp the window it
  // only helps. The structural bound is the Range chunk, not this.
  int rb = kOtaRcvBufBytes;
  c.setSocketOption(SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));
  return true;
}

// One GET on an open connection. want > 0 requests bytes [off, off+want).
// Returns the HTTP status code (0 = transport failure), leaves the socket at
// the body. bodyLen = this response's Content-Length; totalLen = full asset
// size (Content-Range total on 206, Content-Length on 200); keepAlive = the
// connection survives past this body.
int mirrorRequest(WiFiClient& c, const MirrorUrl& m, size_t off, size_t want,
                  size_t& bodyLen, size_t& totalLen, bool& keepAlive) {
  if (!c.connected() && !mirrorConnect(c, m)) return 0;
  if (want > 0)
    c.printf("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: SlyTherm-OTA\r\n"
             "Range: bytes=%u-%u\r\nConnection: keep-alive\r\n\r\n",
             m.path, m.host, static_cast<unsigned>(off),
             static_cast<unsigned>(off + want - 1));
  else
    c.printf("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: SlyTherm-OTA\r\n"
             "Connection: close\r\n\r\n", m.path, m.host);
  const String status = c.readStringUntil('\n');  // "HTTP/1.1 206 ...\r"
  const int sp = status.indexOf(' ');
  const int code = sp > 0 ? atoi(status.c_str() + sp + 1) : 0;
  bodyLen = 0;
  totalLen = 0;
  keepAlive = want > 0;  // HTTP/1.1 default; overridden by Connection header
  for (;;) {
    String h = c.readStringUntil('\n');
    if (h.length() == 0 || h == "\r") break;  // blank line ends the headers
    h.toLowerCase();
    if (h.startsWith("content-length:"))
      bodyLen = strtoul(h.c_str() + 15, nullptr, 10);
    else if (h.startsWith("content-range:")) {
      // "content-range: bytes 0-16383/1913840" — the suffix is the full size
      if (const char* sl = strchr(h.c_str(), '/'))
        totalLen = strtoul(sl + 1, nullptr, 10);
    } else if (h.startsWith("connection:")) {
      keepAlive = h.indexOf("keep-alive") >= 0;
    }
  }
  if (code == 200) totalLen = bodyLen;
  return code;
}

// ---- download + verify + stage ----
bool doApply(bool force) {
  if (gAvailable.appUrl.empty()) {
    fail("apply: no update resolved (check first)");
    return false;
  }
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  if (next == nullptr) {
    fail("apply: no inactive OTA slot (#64)");
    return false;
  }
  if (gAvailable.appSize && gAvailable.appSize > next->size) {
    fail("apply: image exceeds OTA slot");
    return false;
  }

  // Crash-loop guard: an apply that RESETS the board mid-flight (e.g. the
  // esp-hosted SDIO assert the streaming path tripped on the P4) must not be
  // retried forever by the auto-apply below. Persist an attempt counter keyed
  // to the target version (a new release resets it); kMaxApplyAttempts
  // strikes -> this version becomes manual-only.
  {
    Preferences p;
    p.begin(kNvsNs, false);
    String v = p.getString("atryv", "");
    uint8_t n = (v == gAvailable.version.c_str()) ? p.getUChar("atry", 0) : 0;
    if (n >= kMaxApplyAttempts) {
      // #182: `cmd/ota_apply` payload "force" is the remote escape — before
      // it, only a new release (or USB) could unlatch a fielded unit.
      if (!force) {
        p.end();
        fail("apply: repeated resets mid-apply — manual retry only");
        return false;
      }
      Serial.printf("[ota] FORCE apply: clearing crash-loop guard (%u strikes)\n", n);
      n = 0;
    }
    p.putString("atryv", gAvailable.version.c_str());
    p.putUChar("atry", static_cast<uint8_t>(n + 1));
    p.end();
  }

  setStatus(State::kDownloading, 0, gAvailable.version.c_str(), "");
  char url[192];
  assetUrl(url, sizeof(url), gAvailable.appUrl);
  const bool secure = strncmp(url, "http://", 7) != 0;

  // #180: allocate the PSRAM buffer BEFORE connecting, so the very first socket
  // read happens the instant the body arrives — no ps_malloc(≈2 MB) gap in which
  // the tunnelled response floods the SDIO RX pool unread. appSize is known from
  // the catalog. Fallback (no PSRAM / unknown size): direct streaming to flash.
  const size_t expect = gAvailable.appSize ? gAvailable.appSize : UPDATE_SIZE_UNKNOWN;
  uint8_t* ram = nullptr;
  if (expect != UPDATE_SIZE_UNKNOWN && psramFound() &&
      ESP.getFreePsram() > expect + (2u << 20)) {
    ram = static_cast<uint8_t*>(ps_malloc(expect));
  }

  WiFiClient plain;
  WiFiClientSecure tls;
  HTTPClient http;
  WiFiClient* stream = nullptr;
  int total = -1;
  MirrorUrl mh{};
  bool ranged = false;      // #182: P4 Range-chunked mirror mode
  size_t chunkBody = 0;     // body bytes pending on the wire (ranged mode)
  bool keepAlive = false;
  // Close whichever transport we actually opened.
  auto closeDl = [&]() { if (secure) http.end(); else plain.stop(); };
  if (!secure) {
    // LAN mirror (plain http, possibly tunnelled) — the path the Remotes use.
    if (!parseMirrorUrl(url, mh)) {
      if (ram) free(ram);
      fail("apply: bad mirror url");
      return false;
    }
    size_t bodyLen = 0, totalLen = 0;
#ifdef CONFIG_IDF_TARGET_ESP32P4
    // #182: probe with a Range request. 206 = chunked mode (in-flight bounded
    // to one chunk — the tunnel can't burst past the SDIO RX pool); 200 = the
    // server has no Range support, single-GET fallback as before.
    const int code = mirrorRequest(plain, mh, 0, kMirrorRangeBytes,
                                   bodyLen, totalLen, keepAlive);
    if (code == 206 && totalLen > 0) {
      ranged = true;
      chunkBody = bodyLen;
      total = static_cast<int>(totalLen);
      Serial.printf("[ota] mirror ranged download: %d B in %u B chunks\n",
                    total, static_cast<unsigned>(kMirrorRangeBytes));
    } else if (code == 200) {
      total = static_cast<int>(bodyLen);
    }
#else
    const int code = mirrorRequest(plain, mh, 0, 0, bodyLen, totalLen, keepAlive);
    if (code == 200) total = static_cast<int>(bodyLen);
#endif
    if (total <= 0) {
      if (ram) free(ram);
      plain.stop();
      Serial.printf("[ota] mirror GET status: %d\n", code);
      fail("apply: mirror GET failed");
      return false;
    }
    stream = &plain;
  } else {
    tls.setCACert(ota_certs::kOtaCaBundle);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setConnectTimeout(15000);
    http.setTimeout(15000);
    if (!http.begin(tls, url)) {
      if (ram) free(ram);
      fail("apply: http.begin failed");
      return false;
    }
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
      if (ram) free(ram);
      http.end();
      failHttp("apply", code, &tls);
      return false;
    }
    stream = http.getStreamPtr();
    total = http.getSize();  // -1 = chunked/unknown
  }
  if (stream == nullptr) {
    if (ram) free(ram);
    closeDl();
    fail("apply: no stream");
    return false;
  }
  if (gAvailable.appSize && total > 0 &&
      static_cast<uint32_t>(total) != gAvailable.appSize) {
    if (ram) free(ram);
    closeDl();
    fail("apply: content-length != catalog appSize");
    return false;
  }
  if (ram == nullptr) {
    if (!Update.begin(expect, U_FLASH)) {
      closeDl();
      fail("apply: Update.begin failed");
      return false;
    }
    Serial.println("[ota] streaming apply (no PSRAM buffer)");
  } else {
    Serial.printf("[ota] buffered apply (%u B to PSRAM first)\n",
                  static_cast<unsigned>(expect));
  }

  Sha256 sha;
  static uint8_t buf[4096];
  size_t written = 0;
  const char* pumpErr = nullptr;  // set on unrecoverable pump failure

  // Read up to `need` body bytes (SIZE_MAX = until the stream ends) into the
  // image (PSRAM or flash), updating sha + progress. Returns bytes consumed
  // this call; a short return with pumpErr unset means the connection ended
  // (ranged mode reconnects and resumes; single-shot mode treats it as EOF).
  auto pump = [&](WiFiClient* s, size_t need) -> size_t {
    size_t got = 0;
    uint32_t lastDataMs = millis();
    while (got < need) {
      const size_t avail = s->available();
      if (avail == 0) {
        // A finished transfer closes the connection; re-check before timing out.
        if (!s->connected()) break;
        if (millis() - lastDataMs > kStreamTimeoutMs) {
          pumpErr = "apply: stream stalled";
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      lastDataMs = millis();
      size_t chunk = avail > sizeof(buf) ? sizeof(buf) : avail;
      if (chunk > need - got) chunk = need - got;
#ifdef CONFIG_IDF_TARGET_ESP32P4
    // Pace the download so the esp-hosted SDIO drainer keeps up. The RX pool
    // asserts (sdio_drv.c:953) when inbound data outruns the sdio_read task —
    // cap the per-iteration read and yield between reads.
#ifdef SLYTHERM_WG
      // #180/#182: never throttle reads on the tunnel — a slow reader backs
      // the socket buffer up until lwIP stops draining the SDIO RX pool. Read
      // the full buffer every iteration; the 1-tick yield hands the RX/
      // WG-decrypt tasks CPU. (In-flight volume is bounded by the Range chunk
      // — #182; the camera keeps running.)
      vTaskDelay(1);
#else
      // #129 LAN pacing (~500 KB/s ceiling; a 2 MB image gains a few seconds).
      if (chunk > 2048) chunk = 2048;
      vTaskDelay(pdMS_TO_TICKS(4));
#endif
#endif
      if (ram && written + chunk > expect) chunk = expect - written;
      if (ram && chunk == 0) break;  // server sent more than expected
      const int n = s->readBytes(
          ram ? reinterpret_cast<char*>(ram + written)
              : reinterpret_cast<char*>(buf), chunk);
      if (n <= 0) continue;
      sha.update(ram ? ram + written : buf, static_cast<size_t>(n));
      if (!ram &&
          Update.write(buf, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
        pumpErr = "apply: flash write failed";
        break;
      }
      written += static_cast<size_t>(n);
      got += static_cast<size_t>(n);
      if (total > 0)
        setStatus(State::kDownloading,
                  static_cast<uint8_t>((written * 100u) / total),
                  gAvailable.version.c_str(), "");
    }
    return got;
  };

  if (!ranged) {
    pump(stream, total > 0 ? static_cast<size_t>(total) : SIZE_MAX);
  } else {
    // #182: sequential Range chunks; a dropped connection resumes at
    // `written`. Only consecutive zero-progress rounds count against the
    // retry budget, so a long download can survive many transient drops.
    uint8_t stalls = 0;
    for (;;) {
      const size_t before = written;
      const size_t got = pump(&plain, chunkBody);
      if (pumpErr != nullptr) break;
      if (got < chunkBody || !keepAlive) plain.stop();
      if (written >= static_cast<size_t>(total)) break;
      stalls = (written == before) ? static_cast<uint8_t>(stalls + 1) : 0;
      if (stalls > kMirrorRangeRetries) {
        pumpErr = "apply: mirror range retries exhausted";
        break;
      }
      const size_t left = static_cast<size_t>(total) - written;
      size_t bodyLen = 0, totalLen = 0;
      const int code = mirrorRequest(plain, mh, written,
                                     left > kMirrorRangeBytes ? kMirrorRangeBytes : left,
                                     bodyLen, totalLen, keepAlive);
      if (code != 206 || bodyLen == 0) {
        plain.stop();
        chunkBody = 0;  // burn a retry round, reconnect next pass
        continue;
      }
      chunkBody = bodyLen;
    }
  }
  closeDl();
  if (pumpErr != nullptr || (total > 0 && written != static_cast<size_t>(total))) {
    if (ram) free(ram); else Update.abort();
    fail(pumpErr != nullptr ? pumpErr : "apply: truncated download");
    return false;
  }

  setStatus(State::kVerifying, 100, gAvailable.version.c_str(), "");
  uint8_t digest[32];
  sha.finish(digest);
  if (!hexEqual(digest, gAvailable.sha256)) {
    if (ram) free(ram); else Update.abort();
    fail("verify: sha256 mismatch");
    return false;
  }
  if (!signatureOk(digest, gAvailable.sig)) {
    if (ram) free(ram); else Update.abort();
    fail("verify: signature rejected");
    return false;
  }
  if (ram) {
    // Verified in RAM — now the flash phase. The network is NOT actually idle:
    // MQTT keepalive + the Controller's retained echo keep flowing in. On the
    // P4 that matters, because the esp-hosted SDIO drainer (`sdio_read`) runs
    // from flash — a large Update.write suspends the XIP cache long enough that
    // that inbound exhausts the SDIO RX pool and asserts (sdio_drv.c:953,
    // fix/sdio-rxpool). So on the P4 we bound each cache-suspend window to a
    // single 4 KB sector-erase and yield generously, giving `sdio_read` a slot
    // to drain the pool between erases. The S3 has native WiFi (no SDIO path),
    // so it keeps the fast 16 KB chunking.
    if (!Update.begin(expect, U_FLASH)) {
      free(ram);
      fail("apply: Update.begin failed");
      return false;
    }
#ifdef CONFIG_IDF_TARGET_ESP32P4
    constexpr size_t kFlashChunk = 4096;
    const TickType_t kFlashYield = pdMS_TO_TICKS(3);
#else
    constexpr size_t kFlashChunk = 16384;
    const TickType_t kFlashYield = 1;
#endif
    for (size_t off = 0; off < written;) {
      const size_t chunk = written - off > kFlashChunk ? kFlashChunk : written - off;
      if (Update.write(ram + off, chunk) != chunk) {
        free(ram);
        Update.abort();
        fail("apply: flash write failed");
        return false;
      }
      off += chunk;
      vTaskDelay(kFlashYield);
    }
    free(ram);
  }
  // Activates the new slot in otadata; the running image keeps control until
  // the (gated) reboot below. A power-cycle while staged boots the new image
  // early — acceptable: boot-to-no-demand holds and bootValidate() arms.
  if (!Update.end(true)) {
    fail("apply: Update.end failed");
    return false;
  }
  Preferences p;
  p.begin(kNvsNs, false);
  p.putString("pend", gAvailable.version.c_str());
  p.putUChar("tries", 0);
  p.putUChar("atry", 0);  // clean apply: reset the crash-loop guard
  p.end();
  Serial.printf("[ota] staged %s -> slot %s; waiting for safe reboot window\n",
                gAvailable.version.c_str(), next->label);
  setStatus(State::kStaged, 100, gAvailable.version.c_str(), "");
  return true;
}

void otaTask(void*) {
  uint32_t lastAutoCheckMs = 0;
  bool staged = false;
  for (;;) {
    // Pending-update self-test window (this boot IS the new image).
    if (gPendingAtBoot) {
      if (gSelfTestPass) {
        Preferences p;
        p.begin(kNvsNs, false);
        String v = p.getString("pend", "");
        p.putString("pend", "");
        p.putUChar("tries", 0);
        p.putString("last", (std::string("ok ") + v.c_str()).c_str());
        p.end();
        gPendingAtBoot = false;
        Serial.printf("[ota] self-test passed — %s confirmed\n", v.c_str());
      } else if (millis() - gBootMs > kSelfTestTimeoutMs) {
        Serial.println("[ota] self-test TIMEOUT — rolling back");
        const esp_partition_t* prev = esp_ota_get_next_update_partition(nullptr);
        Preferences p;
        p.begin(kNvsNs, false);
        p.putString("pend", "");
        p.putString("last", "rollback timeout");
        p.end();
        if (prev) esp_ota_set_boot_partition(prev);
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP.restart();
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      // Daily cadence after a good check; backed-off retry after failures
      // (first boot attempt races WiFi/NTP — don't wait a day to recover,
      // but #129: repeated failures must not hammer a transport that may be
      // crashing under the TLS load: 5 -> 10 -> 20 -> 40 -> 60 min cap).
      static bool lastCheckOk = false;
      static uint8_t checkFails = 0;
      uint32_t interval = kCheckIntervalMs;
      if (!lastCheckOk) {
        const uint8_t shift = checkFails > 4 ? 4 : checkFails;
        interval = kCheckRetryMs << shift;
        if (interval > kCheckRetryMaxMs) interval = kCheckRetryMaxMs;
      }
      const bool auto_due = (lastAutoCheckMs == 0 ||
                             millis() - lastAutoCheckMs >= interval) &&
                            millis() - gBootMs >= kBootCheckGraceMs;  // #129
      if (gCheckReq || (auto_due && !staged)) {
        gCheckReq = false;
        lastAutoCheckMs = millis();
        lastCheckOk = doCheck();
        checkFails = lastCheckOk ? 0 : static_cast<uint8_t>(checkFails + (checkFails < 10));
        // Auto-apply: a found update stages itself (downloads + verifies)
        // without an operator. The REBOOT is still gated (furnace idle on
        // the Controller); the crash-loop guard in doApply() keeps a
        // reset-mid-apply from retrying past kMaxApplyAttempts.
        if (lastCheckOk && status().state == State::kUpdateAvailable)
          gApplyReq = true;
      }
      if (gApplyReq && !staged) {
        gApplyReq = false;
        const bool force = gForceApply;
        gForceApply = false;
        if (status().state == State::kUpdateAvailable) staged = doApply(force);
        else if (status().state != State::kDownloading &&
                 status().state != State::kVerifying)
          fail("apply: nothing available (check first)");
      }
    }

    if (staged && otaSafeToReboot()) {
      Serial.println("[ota] safe window — rebooting into the new image");
      setStatus(State::kRebooting, 100, nullptr, "");
      vTaskDelay(pdMS_TO_TICKS(500));  // let the MQTT surface publish
      ESP.restart();
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

}  // namespace

bool bootValidate() {
  gBootMs = millis();
  Preferences p;
  p.begin(kNvsNs, false);
  String pend = p.getString("pend", "");
  if (pend.length() == 0) {
    p.end();
    return false;
  }
  uint8_t tries = p.getUChar("tries", 0);
  if (tries + 1 >= kMaxBootAttempts) {
    // Crash-looping new image (complements the #80 reset-loop latch): give
    // up and boot the previous slot.
    p.putString("pend", "");
    p.putString("last", "rollback crash-loop");
    p.end();
    const esp_partition_t* prev = esp_ota_get_next_update_partition(nullptr);
    Serial.printf("[ota] boot attempt %u with %s pending — ROLLBACK to %s\n",
                  tries + 1, pend.c_str(), prev ? prev->label : "?");
    if (prev) {
      esp_ota_set_boot_partition(prev);
      delay(200);
      ESP.restart();
    }
    setStatus(State::kRolledBack, 0, "", "crash-loop rollback");
    return true;
  }
  p.putUChar("tries", tries + 1);
  p.end();
  gPendingAtBoot = true;
  Serial.printf("[ota] validating %s (boot attempt %u/%u) — self-test armed\n",
                pend.c_str(), tries + 1, kMaxBootAttempts);
  return false;
}

void noteSelfTestPass() { gSelfTestPass = true; }

void begin() {
  loadMirror();
  xTaskCreatePinnedToCore(otaTask, "ota", kTaskStack, nullptr, kTaskPrio,
                          nullptr, 0);
}

void requestCheck() { gCheckReq = true; }
void requestApply(bool force) {
  if (force) gForceApply = true;
  gApplyReq = true;
}

void setMirror(const char* baseUrl) {
  char clean[sizeof(gMirror)] = "";
  if (baseUrl) strlcpy(clean, baseUrl, sizeof(clean));
  size_t len = strlen(clean);
  while (len && clean[len - 1] == '/') clean[--len] = '\0';  // no trailing /
  // Accept only http(s) URLs or empty (= back to GitHub direct).
  if (len && strncmp(clean, "http://", 7) != 0 &&
      strncmp(clean, "https://", 8) != 0) {
    Serial.printf("[ota] mirror REJECTED (not a URL): %s\n", clean);
    return;
  }
  strlcpy(gMirror, clean, sizeof(gMirror));
  Preferences p;
  p.begin(kNvsNs, false);
  p.putString("mirror", gMirror);
  p.end();
  Serial.printf("[ota] mirror set: %s\n", gMirror[0] ? gMirror : "(github)");
}

Status status() {
  portENTER_CRITICAL(&gMux);
  Status s = gStatus;
  portEXIT_CRITICAL(&gMux);
  return s;
}

}  // namespace ota

#endif  // SLYTHERM_OTA
