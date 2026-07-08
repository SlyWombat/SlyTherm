// coredump_server.cpp — see coredump_server.h.

#include "coredump_server.h"

#include <Arduino.h>
#include <WiFi.h>

#include "esp_partition.h"
#if __has_include("esp_core_dump.h")
#include "esp_core_dump.h"
#define CDS_HAS_COREDUMP 1
#endif

namespace coredump_server {
namespace {

WiFiServer* gSrv = nullptr;

// Locate the dump by API where available (gives the true image size), fall
// back to partition-typed lookup. NEVER hardcode the address — the S3 (8MB
// table) and P4 (16MB table) differ.
bool dumpInfo(size_t& outSize, const esp_partition_t*& outPart) {
  outPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                     ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  if (outPart == nullptr) return false;
#ifdef CDS_HAS_COREDUMP
  size_t addr = 0, size = 0;
  if (esp_core_dump_image_get(&addr, &size) == ESP_OK && size > 0 &&
      size <= outPart->size) {
    outSize = size;
    return true;
  }
  return false;
#else
  return false;
#endif
}

void serveClient(WiFiClient& c) {
  // Optional single command line (250ms window, same as the screenshot server).
  String cmd = "";
  uint32_t t0 = millis();
  while (millis() - t0 < 250) {
    if (c.available()) {
      char ch = static_cast<char>(c.read());
      if (ch == '\n' || ch == '\r') break;
      cmd += ch;
      if (cmd.length() > 8) break;
    }
  }

  if (cmd == "ERASE") {
#ifdef CDS_HAS_COREDUMP
    const bool ok = (esp_core_dump_image_erase() == ESP_OK);
#else
    const bool ok = false;
#endif
    c.print(ok ? "OK\n" : "ERR\n");
    return;
  }

  size_t size = 0;
  const esp_partition_t* part = nullptr;
  if (!dumpInfo(size, part)) {
    c.print("SLYCORE 0\n");
    return;
  }
  char hdr[24];
  const int hn = snprintf(hdr, sizeof(hdr), "SLYCORE %u\n", static_cast<unsigned>(size));
  c.write(reinterpret_cast<const uint8_t*>(hdr), static_cast<size_t>(hn));

  // Bounded send: 15s cap + 2.5s stall detector (the screenshot server's
  // hard-won recipe — a dead client must never freeze the calling task).
  uint8_t buf[1024];
  size_t sent = 0;
  const uint32_t deadline = millis() + 15000;
  uint32_t lastProg = millis();
  while (sent < size && c.connected() && static_cast<int32_t>(millis() - deadline) < 0) {
    size_t chunk = size - sent;
    if (chunk > sizeof(buf)) chunk = sizeof(buf);
    if (esp_partition_read(part, sent, buf, chunk) != ESP_OK) break;
    const int w = c.write(buf, chunk);
    if (w <= 0) {
      if (millis() - lastProg > 2500) break;
      delay(2);
      continue;
    }
    sent += static_cast<size_t>(w);
    lastProg = millis();
  }
}

}  // namespace

void poll() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (gSrv == nullptr) {
    gSrv = new WiFiServer(8082);
    gSrv->begin();
    gSrv->setNoDelay(true);
  }
  WiFiClient c = gSrv->available();
  if (!c) return;
  serveClient(c);
  c.stop();
}

}  // namespace coredump_server
