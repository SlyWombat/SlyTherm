// main_sniffer.cpp — Phase 1/2 RX-only CT-485 bus sniffer with on-device web
// admin console (issues #6, #57).
// Bench rig: ESP32-DevKitC + 3.3 V RS-485 transceiver in permanent receive,
// tapped in PARALLEL with the existing thermostat (docs/02 §8 methodology,
// docs/05 Phase 1).
//
// ============================ RX-ONLY GUARANTEE =============================
// This firmware is physically incapable of driving the bus:
//   - UART2 is opened with txPin = -1 (sniffer::kTxPin, static_assert below):
//     no GPIO is ever attached to the UART TX signal.
//   - No DE/RE GPIO is configured or touched anywhere in this file; the
//     transceiver's DE pin is strapped LOW (RE# low) in hardware.
//   - Serial2 is never written to. The only TX paths are USB (Serial/UART0)
//     and Wi-Fi (HTTP/WebSocket/optional MQTT) — neither touches the RS-485
//     transceiver. Every web-console command (force baud / auto-baud / pause /
//     clear / annotate / download) only reconfigures the RX side or touches
//     RAM; there is no command that can emit a bus byte.
// Do not add any of the above to this file. TX belongs to Phase 3 firmware
// only, after the docs/05 Phase 2 field-dictionary gate.
// ============================================================================
//
// Behavior:
//   - Non-blocking byte pump; micros()-based inter-byte gap measurement.
//     The first byte after a >= 3.5 ms (ct485::kInterFrameGapUs) idle gap
//     starts a new frame; bytes feed ct485::FrameAccumulator (lib/Ct485Frame).
//     A shadow copy of the in-progress bytes also captures Fletcher-BAD
//     closes (the accumulator only exposes valid frames).
//   - Auto-baud: alternates 9600/38400 on a dwell timer until one baud
//     clearly wins on Fletcher-valid frame count; overridable from the console.
//   - One line per valid frame on USB serial (115200) + WebSocket live feed;
//     frames are also archived in capture::CaptureBuffer and downloadable at
//     GET /capture in the exact line format tools/ct485_decode.py ingests.
//   - Web console at http://<ip>/ (single PROGMEM page, WS on :81): live
//     feed, counters, per-msgType census, baud control, pause/resume,
//     operator annotations, capture download, runtime MQTT toggle.
//   - Wi-Fi: STA with src/sniffer_secrets.h credentials; falls back to a
//     SoftAP (sniffer_config.h) after kWifiStaTimeoutMs or with no creds.
//   - MQTT frame push only when compiled with -DSNIFFER_MQTT AND toggled on
//     from the console (runtime default OFF).
//
// Timing caveat: gaps are measured when bytes are DRAINED from the UART
// driver ring, not on the wire. The pump therefore keeps the loop fast
// (buffered USB TX, no blocking calls, RX re-pumped between capture-download
// chunks); if the loop ever stalls past one gap time, adjacent frames merge
// and are counted as badLength rather than silently corrupting data — the
// 2 KiB driver ring guarantees no byte loss.

#include <Arduino.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>

#include <cstring>
#include <string>

#include "CaptureBuffer.h"
#include "Ct485Core.h"
#include "Ct485Frame.h"
#include "Ct485Parser.h"
#include "sniffer_config.h"

#ifdef SNIFFER_MQTT
#include <PubSubClient.h>
#endif

#ifdef SNIFFER_DISPLAY
// Optional on-screen dashboard (Waveshare ESP32-S3 board only; read-only view
// of the same counters — never touches the RS-485 UART). See sniffer_display.*.
#include "sniffer_display.h"
#endif

static_assert(sniffer::kTxPin == -1,
              "RX-ONLY GUARANTEE: the sniffer must never attach a UART TX pin");

namespace {

ct485::FrameAccumulator gAcc;
capture::CaptureBuffer  gCapture;  // capture::kDefaultBudgetBytes, heap, once

// Gap / frame bookkeeping (drain-time measurement, see caveat above).
uint32_t gLastByteUs      = 0;
bool     gHaveInProgress  = false;  // bytes accumulated since the last gap
uint32_t gCurFrameGapUs   = 0;      // gap that preceded the in-progress frame
uint32_t gCurFrameStartMs = 0;

// Shadow of the in-progress frame so Fletcher-bad closes can be archived.
uint8_t gShadow[ct485::kMaxFrame];
size_t  gShadowLen = 0;

// Auto-baud.
constexpr uint32_t kBauds[2] = {ct485::kBaudDefault, ct485::kBaudAlt};
uint8_t  gBaudIdx        = 0;
bool     gBaudLocked     = false;
uint32_t gBaudDwellMs    = 0;       // dwell start (millis)
uint32_t gValidAtBaud[2] = {0, 0};

// Counters.
uint32_t gTotalBytes      = 0;
uint32_t gLastHeartbeatMs = 0;
uint32_t gCensus[256]     = {};     // valid frames by msgType byte

char gLine[1200];                   // worst case: 252-byte frame -> ~920 chars
char gJson[1600];                   // WS frame/status JSON

// Console / network.
WebServer        gHttp(sniffer::kHttpPort);
WebSocketsServer gWs(sniffer::kWsPort);
bool     gStreamPaused   = false;   // WS live feed only; capture continues
bool     gWifiAp         = false;
bool     gWifiAnnounced  = false;
uint32_t gWifiStaSinceMs = 0;
uint32_t gLastStatusMs   = 0;

#ifdef SNIFFER_MQTT
WiFiClient   gWifiClient;
PubSubClient gMqtt(gWifiClient);
bool         gMqttEnabled       = false;  // runtime toggle; default OFF
uint32_t     gLastMqttAttemptMs = 0;

void mqttPump(uint32_t nowMs) {
  if (!gMqttEnabled) {
    if (gMqtt.connected()) gMqtt.disconnect();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) return;
  if (!gMqtt.connected()) {
    // Rate-limit (re)connect attempts; PubSubClient::connect blocks briefly.
    if (nowMs - gLastMqttAttemptMs < sniffer::kMqttReconnectMs) return;
    gLastMqttAttemptMs = nowMs;
    gMqtt.connect(sniffer::kMqttClientId, SNIFFER_MQTT_USER, SNIFFER_MQTT_PASS);
    return;
  }
  gMqtt.loop();
}

void mqttPublishLine(const char* line) {
  if (gMqttEnabled && gMqtt.connected())
    gMqtt.publish(sniffer::kMqttTopicFrames, line);
}
int mqttState() { return gMqttEnabled ? 1 : 0; }
#else
inline void mqttPublishLine(const char*) {}
constexpr int mqttState() { return -1; }  // not compiled in
#endif

// ---------------------------------------------------------------------------
// Decode / formatting
// ---------------------------------------------------------------------------

std::string decodeOneLiner(const uint8_t* raw, size_t len) {
  ct485::Frame f;
  if (!ct485::decode(raw, len, f)) return "undecodable (bad length/FCS)";
  std::string s = "dst=" + ct485::hexByte(f.dst) +
                  " src=" + ct485::hexByte(f.src) + " " +
                  ct485::msgTypeName(f.msgType);
  if (f.baseMsgType() == static_cast<uint8_t>(ct485::MsgType::kSetControlCmd)) {
    s += " cmd=" + ct485::commandName(f.sendParamHi);
    const ct485::SetControlDecode sc = ct485::decodeSetControl(f);
    if (sc.isSystemSwitch && sc.hasSwitchValue) s += "->" + sc.switchName;
  }
  return s;
}

void jsonEscape(const char* in, char* out, size_t cap) {
  size_t o = 0;
  for (; *in && o + 7 < cap; ++in) {
    const unsigned char c = static_cast<unsigned char>(*in);
    if (c == '"' || c == '\\') {
      out[o++] = '\\';
      out[o++] = static_cast<char>(c);
    } else if (c < 0x20) {
      o += static_cast<size_t>(snprintf(out + o, cap - o, "\\u%04x", c));
    } else {
      out[o++] = static_cast<char>(c);
    }
  }
  out[o] = '\0';
}

void emitFrameLine(const uint8_t* raw, size_t len, uint32_t gapUs, uint32_t tMs) {
  size_t pos = static_cast<size_t>(
      snprintf(gLine, sizeof(gLine), "F t=%lums gap=%luus baud=%lu len=%u |",
               static_cast<unsigned long>(tMs),
               static_cast<unsigned long>(gapUs),
               static_cast<unsigned long>(kBauds[gBaudIdx]),
               static_cast<unsigned>(len)));
  for (size_t i = 0; i < len && pos + 4 < sizeof(gLine); ++i) {
    pos += static_cast<size_t>(
        snprintf(gLine + pos, sizeof(gLine) - pos, " %02X", raw[i]));
  }
  const std::string d = decodeOneLiner(raw, len);
  if (pos + 4 < sizeof(gLine))
    snprintf(gLine + pos, sizeof(gLine) - pos, " | %s", d.c_str());

  Serial.println(gLine);
  mqttPublishLine(gLine);
}

void wsPushFrame(const uint8_t* raw, size_t len, bool ok,
                 uint32_t gapUs, uint32_t tMs) {
  if (gStreamPaused || gWs.connectedClients() == 0) return;
  size_t pos = static_cast<size_t>(snprintf(
      gJson, sizeof(gJson), "{\"k\":\"f\",\"t\":%lu,\"g\":%lu,\"ok\":%d,\"hex\":\"",
      static_cast<unsigned long>(tMs), static_cast<unsigned long>(gapUs),
      ok ? 1 : 0));
  for (size_t i = 0; i < len && pos + 4 < sizeof(gJson); ++i) {
    pos += static_cast<size_t>(snprintf(gJson + pos, sizeof(gJson) - pos,
                                        i ? " %02X" : "%02X", raw[i]));
  }
  const std::string d = ok ? decodeOneLiner(raw, len) : std::string();
  if (pos + d.size() + 8 < sizeof(gJson)) {
    pos += static_cast<size_t>(snprintf(gJson + pos, sizeof(gJson) - pos,
                                        "\",\"d\":\"%s\"}", d.c_str()));
    gWs.broadcastTXT(gJson, pos);
  }
}

void wsPushMsg(const char* text) {
  char esc[256];
  jsonEscape(text, esc, sizeof(esc));
  const int n = snprintf(gJson, sizeof(gJson), "{\"k\":\"msg\",\"text\":\"%s\"}", esc);
  gWs.broadcastTXT(gJson, static_cast<size_t>(n));
}

void wsPushStatus(uint32_t nowMs) {
  if (gWs.connectedClients() == 0) return;
  const ct485::FrameAccumulator::Counters& c = gAcc.counters();
  size_t pos = static_cast<size_t>(snprintf(
      gJson, sizeof(gJson),
      "{\"k\":\"st\",\"baud\":%lu,\"locked\":%d,\"v9600\":%lu,\"v38400\":%lu,"
      "\"bytes\":%lu,\"ok\":%lu,\"badcrc\":%lu,\"badlen\":%lu,\"overrun\":%lu,"
      "\"cap\":%u,\"capb\":%u,\"evict\":%lu,\"heap\":%lu,\"rssi\":%d,"
      "\"up\":%lu,\"paused\":%d,\"mqtt\":%d,\"ap\":%d,\"census\":{",
      static_cast<unsigned long>(kBauds[gBaudIdx]), gBaudLocked ? 1 : 0,
      static_cast<unsigned long>(gValidAtBaud[0]),
      static_cast<unsigned long>(gValidAtBaud[1]),
      static_cast<unsigned long>(gTotalBytes),
      static_cast<unsigned long>(c.framesOk),
      static_cast<unsigned long>(c.badChecksum),
      static_cast<unsigned long>(c.badLength),
      static_cast<unsigned long>(c.overruns),
      static_cast<unsigned>(gCapture.recordCount()),
      static_cast<unsigned>(gCapture.bytesUsed()),
      static_cast<unsigned long>(gCapture.evictedRecords()),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      gWifiAp ? 0 : static_cast<int>(WiFi.RSSI()),
      static_cast<unsigned long>(nowMs), gStreamPaused ? 1 : 0, mqttState(),
      gWifiAp ? 1 : 0));
  bool first = true;
  for (int i = 0; i < 256 && pos + 16 < sizeof(gJson); ++i) {
    if (!gCensus[i]) continue;
    pos += static_cast<size_t>(snprintf(
        gJson + pos, sizeof(gJson) - pos, "%s\"%02X\":%lu", first ? "" : ",",
        i, static_cast<unsigned long>(gCensus[i])));
    first = false;
  }
  if (pos + 3 < sizeof(gJson)) {
    pos += static_cast<size_t>(snprintf(gJson + pos, sizeof(gJson) - pos, "}}"));
    gWs.broadcastTXT(gJson, pos);
  }
}

// ---------------------------------------------------------------------------
// Frame completion
// ---------------------------------------------------------------------------

void maybeLockBaud() {
  if (gBaudLocked) return;
  const uint32_t mine  = gValidAtBaud[gBaudIdx];
  const uint32_t other = gValidAtBaud[gBaudIdx ^ 1];
  if (mine >= sniffer::kAutobaudLockFrames &&
      mine >= other * sniffer::kAutobaudLockRatio) {
    gBaudLocked = true;
    snprintf(gLine, sizeof(gLine), "AB locked baud=%lu (valid %lu vs %lu)",
             static_cast<unsigned long>(kBauds[gBaudIdx]),
             static_cast<unsigned long>(mine),
             static_cast<unsigned long>(other));
    Serial.println(gLine);
  }
}

void onValidFrame(uint32_t gapUs, uint32_t tMs) {
  gValidAtBaud[gBaudIdx]++;
  const uint8_t* raw = gAcc.frame();
  const size_t   len = gAcc.frameLen();
  gCensus[raw[ct485::kOffMsgType]]++;
  gCapture.addFrame(tMs, gapUs, raw, len, true);
  emitFrameLine(raw, len, gapUs, tMs);
  wsPushFrame(raw, len, true, gapUs, tMs);
  maybeLockBaud();
}

void onInvalidClose(uint32_t gapUs, uint32_t tMs) {
  // Wrong-baud scanning yields a torrent of garbage closes: archive bad
  // frames only once the baud is locked (then they are diagnostic gold);
  // the live WS feed shows them always.
  if (gBaudLocked) gCapture.addFrame(tMs, gapUs, gShadow, gShadowLen, false);
  wsPushFrame(gShadow, gShadowLen, false, gapUs, tMs);
}

void closeShadow(bool valid, uint32_t gapUs, uint32_t tMs) {
  if (valid) {
    onValidFrame(gapUs, tMs);
  } else if (gShadowLen > 0) {
    onInvalidClose(gapUs, tMs);
  }
  gShadowLen = 0;
}

// ---- Byte pump (non-blocking; also re-entered between download chunks) ----
void pumpRx() {
  int avail = Serial2.available();
  while (avail-- > 0) {
    const int c = Serial2.read();
    if (c < 0) break;
    const uint32_t nowUs = micros();
    const uint32_t gapUs = nowUs - gLastByteUs;
    gLastByteUs = nowUs;
    gTotalBytes++;

    const bool gapBefore = !gHaveInProgress || gapUs >= ct485::kInterFrameGapUs;
    // feed() closes the PREVIOUS frame when gapBefore is set, so report it
    // with the gap/time captured at ITS first byte before rolling those over.
    const bool valid = gAcc.feed(static_cast<uint8_t>(c), gapBefore);
    if (gapBefore) {
      closeShadow(valid, gCurFrameGapUs, gCurFrameStartMs);
      gCurFrameGapUs   = gapUs;
      gCurFrameStartMs = millis();
    }
    if (gShadowLen < ct485::kMaxFrame) gShadow[gShadowLen++] = static_cast<uint8_t>(c);
    gHaveInProgress = true;
  }

  // Idle close-out: bus quiet >= one gap ends the in-progress frame.
  if (gHaveInProgress &&
      static_cast<uint32_t>(micros() - gLastByteUs) >= ct485::kInterFrameGapUs) {
    gHaveInProgress = false;
    closeShadow(gAcc.flush(), gCurFrameGapUs, gCurFrameStartMs);
  }
}

// ---------------------------------------------------------------------------
// Auto-baud / heartbeat (serial paths unchanged from issue #6)
// ---------------------------------------------------------------------------

void autobaudPump(uint32_t nowMs) {
  if (gBaudLocked) return;
  if (nowMs - gBaudDwellMs < sniffer::kAutobaudDwellMs) return;
  if (gHaveInProgress) return;  // never re-baud mid-frame
  gBaudIdx ^= 1;
  gBaudDwellMs = nowMs;
  Serial2.updateBaudRate(kBauds[gBaudIdx]);  // keeps RX-only pin config
  gAcc.reset();                              // drop cross-baud partials; counters kept
  gShadowLen = 0;
  snprintf(gLine, sizeof(gLine), "AB trying baud=%lu",
           static_cast<unsigned long>(kBauds[gBaudIdx]));
  Serial.println(gLine);
}

void emitHeartbeat(uint32_t nowMs) {
  const ct485::FrameAccumulator::Counters& c = gAcc.counters();
  snprintf(gLine, sizeof(gLine),
           "HB t=%lums baud=%lu(%s) bytes=%lu ok=%lu badcrc=%lu badlen=%lu "
           "overrun=%lu v%lu=%lu v%lu=%lu",
           static_cast<unsigned long>(nowMs),
           static_cast<unsigned long>(kBauds[gBaudIdx]),
           gBaudLocked ? "locked" : "scanning",
           static_cast<unsigned long>(gTotalBytes),
           static_cast<unsigned long>(c.framesOk),
           static_cast<unsigned long>(c.badChecksum),
           static_cast<unsigned long>(c.badLength),
           static_cast<unsigned long>(c.overruns),
           static_cast<unsigned long>(kBauds[0]),
           static_cast<unsigned long>(gValidAtBaud[0]),
           static_cast<unsigned long>(kBauds[1]),
           static_cast<unsigned long>(gValidAtBaud[1]));
  Serial.println(gLine);
}

// ---------------------------------------------------------------------------
// Console commands (WebSocket text protocol)
// ---------------------------------------------------------------------------

void forceBaud(uint32_t baud) {
  for (uint8_t i = 0; i < 2; ++i) {
    if (kBauds[i] != baud) continue;
    gBaudIdx = i;
    gBaudLocked = true;
    Serial2.updateBaudRate(baud);  // RX-only pin config preserved
    gAcc.reset();
    gShadowLen = 0;
    snprintf(gLine, sizeof(gLine), "console: baud forced to %lu",
             static_cast<unsigned long>(baud));
    Serial.println(gLine);
    wsPushMsg(gLine);
    return;
  }
  wsPushMsg("unsupported baud (9600 or 38400)");
}

void addAnnotation(const char* text) {
  const uint32_t tMs = millis();
  gCapture.addAnnotation(tMs, text);
  snprintf(gLine, sizeof(gLine), "# %lums %s",
           static_cast<unsigned long>(tMs), text);
  Serial.println(gLine);  // also lands in tee'd serial captures
  char esc[256];
  jsonEscape(text, esc, sizeof(esc));
  const int n = snprintf(gJson, sizeof(gJson),
                         "{\"k\":\"note\",\"t\":%lu,\"text\":\"%s\"}",
                         static_cast<unsigned long>(tMs), esc);
  gWs.broadcastTXT(gJson, static_cast<size_t>(n));
}

void handleConsoleCommand(const char* cmd) {
  if (strcmp(cmd, "pause") == 0) {
    gStreamPaused = true;
  } else if (strcmp(cmd, "resume") == 0) {
    gStreamPaused = false;
  } else if (strcmp(cmd, "clear") == 0) {
    gAcc.clearCounters();
    gTotalBytes = 0;
    memset(gCensus, 0, sizeof(gCensus));
    gValidAtBaud[0] = gValidAtBaud[1] = 0;
    wsPushMsg("counters cleared");
  } else if (strcmp(cmd, "autobaud") == 0) {
    gBaudLocked = false;
    gValidAtBaud[0] = gValidAtBaud[1] = 0;
    gBaudDwellMs = millis();
    wsPushMsg("auto-baud scan restarted");
  } else if (strncmp(cmd, "baud ", 5) == 0) {
    forceBaud(strtoul(cmd + 5, nullptr, 10));
  } else if (strncmp(cmd, "note ", 5) == 0) {
    if (cmd[5] != '\0') addAnnotation(cmd + 5);
  } else if (strncmp(cmd, "mqtt ", 5) == 0) {
#ifdef SNIFFER_MQTT
    gMqttEnabled = (strcmp(cmd + 5, "on") == 0) ||
                   (strcmp(cmd + 5, "toggle") == 0 && !gMqttEnabled);
    wsPushMsg(gMqttEnabled ? "MQTT push enabled" : "MQTT push disabled");
#else
    wsPushMsg("MQTT not compiled in (build with -DSNIFFER_MQTT)");
#endif
  } else {
    wsPushMsg("unknown command");
  }
  wsPushStatus(millis());
}

void onWsEvent(uint8_t /*num*/, WStype_t type, uint8_t* payload, size_t len) {
  if (type != WStype_TEXT || len == 0) return;
  char cmd[256];
  if (len >= sizeof(cmd)) len = sizeof(cmd) - 1;
  memcpy(cmd, payload, len);
  cmd[len] = '\0';
  handleConsoleCommand(cmd);
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

// Single self-contained console page (no filesystem).
const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Dettson CT-485 sniffer</title>
<style>
body{font-family:ui-monospace,monospace;background:#111;color:#ddd;margin:0;padding:8px;font-size:13px}
h1{font-size:15px;margin:2px 0 6px}
#st{white-space:pre-wrap;background:#1a1a1a;padding:6px;border:1px solid #333;margin-bottom:6px}
#log{white-space:pre;overflow:auto;height:55vh;background:#000;padding:6px;border:1px solid #333;margin-top:6px}
.bad{color:#f66}.note{color:#fc6}.msg{color:#6cf}
button{margin:2px;background:#333;color:#ddd;border:1px solid #555;padding:4px 8px;cursor:pointer}
input{background:#222;color:#ddd;border:1px solid #555;padding:4px;width:22em}
#conn{font-weight:normal;color:#f66}
</style></head><body>
<h1>Dettson CT-485 sniffer — RX-only console &nbsp;<span id="conn">connecting…</span></h1>
<div id="st">waiting for status…</div>
<div>
<button id="bp" onclick="cmd(paused?'resume':'pause')">pause</button>
<button onclick="cmd('baud 9600')">force 9600</button>
<button onclick="cmd('baud 38400')">force 38400</button>
<button onclick="cmd('autobaud')">re-run auto-baud</button>
<button onclick="cmd('clear')">clear counters</button>
<button id="bm" onclick="cmd('mqtt toggle')">mqtt</button>
<a href="/capture" download="capture.txt"><button>download capture</button></a>
<br>
<input id="note" placeholder="annotation… (Enter to add)" onkeydown="if(event.key=='Enter')addNote()">
<button onclick="addNote()">add annotation</button>
</div>
<div id="log"></div>
<script>
var ws,paused=false,log=document.getElementById('log');
function cmd(c){if(ws&&ws.readyState==1)ws.send(c)}
function addNote(){var n=document.getElementById('note');if(n.value){cmd('note '+n.value);n.value=''}}
function line(t,cls){var d=document.createElement('div');if(cls)d.className=cls;d.textContent=t;
 log.appendChild(d);while(log.childNodes.length>400)log.removeChild(log.firstChild);log.scrollTop=log.scrollHeight}
function st(m){paused=!!m.paused;document.getElementById('bp').textContent=paused?'resume':'pause';
 document.getElementById('bm').textContent='mqtt: '+(m.mqtt<0?'n/a':(m.mqtt?'ON':'off'));
 var c='';for(var k in m.census)c+=' 0x'+k+':'+m.census[k];
 document.getElementById('st').textContent=
  'baud '+m.baud+' ('+(m.locked?'locked':'scanning')+')  valid@9600='+m.v9600+' valid@38400='+m.v38400+
  (m.ap?'  [SoftAP]':'')+'\n'+
  'bytes='+m.bytes+' ok='+m.ok+' badcrc='+m.badcrc+' badlen='+m.badlen+' overrun='+m.overrun+'\n'+
  'capture: '+m.cap+' records, '+m.capb+' B buffered, '+m.evict+' evicted\n'+
  'uptime '+Math.floor(m.up/1000)+'s  heap '+m.heap+' B  rssi '+m.rssi+' dBm\n'+
  'msgType census:'+(c||' (none)')}
function connect(){
 ws=new WebSocket('ws://'+location.hostname+':81/');
 ws.onopen=function(){var e=document.getElementById('conn');e.textContent='live';e.style.color='#6f6'};
 ws.onclose=function(){var e=document.getElementById('conn');e.textContent='disconnected — retrying…';
  e.style.color='#f66';setTimeout(connect,2000)};
 ws.onmessage=function(ev){var m=JSON.parse(ev.data);
  if(m.k=='f')line('t='+m.t+'ms gap='+m.g+'us  '+m.hex+(m.ok?(m.d?'  | '+m.d:''):'  | FCS/FRAMING BAD'),m.ok?'':'bad');
  else if(m.k=='st')st(m);
  else if(m.k=='note')line('# '+m.t+'ms '+m.text,'note');
  else if(m.k=='msg')line('* '+m.text,'msg')}}
connect();
</script></body></html>
)HTML";

void handleRoot() { gHttp.send_P(200, "text/html", kIndexHtml); }

// Streams (and CONSUMES) the capture buffer in the tools/ct485_decode.py
// line format. RX is re-pumped between chunks so the UART ring cannot
// overflow during a long download.
void handleCapture() {
  gHttp.setContentLength(CONTENT_LENGTH_UNKNOWN);
  gHttp.send(200, "text/plain", "");
  char line[capture::kMaxLineLen];
  String chunk;
  chunk.reserve(1500);
  size_t n;
  while ((n = gCapture.drainNext(line, sizeof(line))) > 0) {
    chunk.concat(line, static_cast<unsigned int>(n));
    chunk += '\n';
    if (chunk.length() >= 1300) {
      gHttp.sendContent(chunk);
      chunk = "";
      pumpRx();
    }
  }
  if (chunk.length()) gHttp.sendContent(chunk);
  gHttp.sendContent("");  // end of chunked stream
}

// ---------------------------------------------------------------------------
// Wi-Fi: STA from secrets, SoftAP fallback so the console always reachable.
// ---------------------------------------------------------------------------

void wifiPump(uint32_t nowMs) {
  if (gWifiAp) return;
  if (WiFi.status() == WL_CONNECTED) {
    gWifiStaSinceMs = nowMs;  // grace timer only runs while disconnected
    if (!gWifiAnnounced) {
      gWifiAnnounced = true;
      snprintf(gLine, sizeof(gLine), "WiFi STA '%s' -> console http://%s/",
               SNIFFER_WIFI_SSID, WiFi.localIP().toString().c_str());
      Serial.println(gLine);
    }
    return;
  }
  gWifiAnnounced = false;
  const bool noCreds = SNIFFER_WIFI_SSID[0] == '\0';
  if (noCreds || nowMs - gWifiStaSinceMs >= sniffer::kWifiStaTimeoutMs) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(sniffer::kApSsid, sniffer::kApPass);
    gWifiAp = true;
    snprintf(gLine, sizeof(gLine),
             "WiFi SoftAP '%s' (pass '%s') -> console http://%s/",
             sniffer::kApSsid, sniffer::kApPass,
             WiFi.softAPIP().toString().c_str());
    Serial.println(gLine);
  }
}

#ifdef SNIFFER_DISPLAY
// Build the on-screen stats snapshot from the live counters and push it to the
// panel. Read-only: touches no bus state. Called ~1 Hz from loop().
void updateDisplay(uint32_t nowMs) {
  const ct485::FrameAccumulator::Counters& c = gAcc.counters();
  display::Stats s{};
  s.ap            = gWifiAp;
  s.wifiConnected = gWifiAp || (WiFi.status() == WL_CONNECTED);
  const String ip = gWifiAp ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  strncpy(s.ip, ip.c_str(), sizeof(s.ip) - 1);
  s.rssi     = gWifiAp ? 0 : static_cast<int>(WiFi.RSSI());
  s.baud     = kBauds[gBaudIdx];
  s.locked   = gBaudLocked;
  s.bytes    = gTotalBytes;
  s.ok       = c.framesOk;
  s.badcrc   = c.badChecksum;
  s.badlen   = c.badLength;
  s.overrun  = c.overruns;
  s.v9600    = gValidAtBaud[0];
  s.v38400   = gValidAtBaud[1];
  s.capRecords = gCapture.recordCount();
  s.capBytes   = gCapture.bytesUsed();
  s.uptimeMs = nowMs;
  s.heap     = ESP.getFreeHeap();

  // Top msgTypes by census (simple selection of the 4 largest, ties ignored).
  s.topN = 0;
  for (int rank = 0; rank < 4; ++rank) {
    int      best      = -1;
    uint32_t bestCount = 0;
    for (int i = 0; i < 256; ++i) {
      if (gCensus[i] <= bestCount) continue;
      bool taken = false;
      for (int j = 0; j < s.topN; ++j)
        if (s.topType[j] == i) taken = true;
      if (taken) continue;
      best      = i;
      bestCount = gCensus[i];
    }
    if (best < 0) break;
    s.topType[s.topN]  = static_cast<uint8_t>(best);
    s.topCount[s.topN] = bestCount;
    s.topN++;
  }
  display::update(s);
}
#endif  // SNIFFER_DISPLAY

}  // namespace

void setup() {
  Serial.setTxBufferSize(sniffer::kUsbTxBufBytes);
  Serial.begin(sniffer::kUsbBaud);

  // RX-only: txPin = -1, no GPIO ever drives the transceiver (see header).
  Serial2.setRxBufferSize(sniffer::kUartRxBufBytes);
  Serial2.begin(kBauds[gBaudIdx], SERIAL_8N1, sniffer::kRxPin, sniffer::kTxPin);

  gLastByteUs      = micros();
  gBaudDwellMs     = millis();
  gLastHeartbeatMs = millis();
  gWifiStaSinceMs  = millis();

  Serial.println();
  snprintf(gLine, sizeof(gLine),
           "Dettson CT-485 sniffer (Phase 1/2, RX-ONLY) — UART2 RX=GPIO%d, no TX pin",
           sniffer::kRxPin);
  Serial.println(gLine);
  snprintf(gLine, sizeof(gLine), "AB trying baud=%lu",
           static_cast<unsigned long>(kBauds[gBaudIdx]));
  Serial.println(gLine);

  WiFi.mode(WIFI_STA);
  if (SNIFFER_WIFI_SSID[0] != '\0')
    WiFi.begin(SNIFFER_WIFI_SSID, SNIFFER_WIFI_PASS);

  gHttp.on("/", handleRoot);
  gHttp.on("/capture", handleCapture);
  gHttp.onNotFound([] { gHttp.send(404, "text/plain", "not found"); });
  gHttp.begin();
  gWs.begin();
  gWs.onEvent(onWsEvent);

#ifdef SNIFFER_MQTT
  gMqtt.setServer(SNIFFER_MQTT_HOST, SNIFFER_MQTT_PORT);
  gMqtt.setBufferSize(sniffer::kMqttBufBytes);
#endif

#ifdef SNIFFER_DISPLAY
  display::begin();  // headless-safe: logs and returns if the panel fails
#endif
}

void loop() {
  pumpRx();

  const uint32_t nowMs = millis();
  autobaudPump(nowMs);

  if (nowMs - gLastHeartbeatMs >= sniffer::kHeartbeatMs) {
    gLastHeartbeatMs = nowMs;
    emitHeartbeat(nowMs);
  }

  wifiPump(nowMs);
  gHttp.handleClient();
  gWs.loop();
  if (nowMs - gLastStatusMs >= sniffer::kStatusPeriodMs) {
    gLastStatusMs = nowMs;
    wsPushStatus(nowMs);
  }

#ifdef SNIFFER_MQTT
  mqttPump(nowMs);
#endif
}
