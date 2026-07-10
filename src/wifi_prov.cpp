// wifi_prov.cpp — see wifi_prov.h. The MQTT task owns the radio and calls
// begin()/service(); the UI task posts requests + reads results.

#include "wifi_prov.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace wifi_prov {
namespace {

SemaphoreHandle_t gMux = nullptr;
inline void L(){ if(gMux) xSemaphoreTake(gMux, portMAX_DELAY); }
inline void U(){ if(gMux) xSemaphoreGive(gMux); }

char gActiveSsid[33] = {}, gActivePass[65] = {};   // credentials in use

// request/result state (UI <-> owner), guarded by gMux
bool      gScanReq   = false;
ScanState gScanSt    = ScanState::kIdle;
Net       gNets[kMaxNets];
int       gNetCount  = 0;
bool      gConnReq   = false;
char      gReqSsid[33] = {}, gReqPass[65] = {};
ConnState gConnSt    = ConnState::kIdle;
bool      gForgetReq = false;

uint32_t gLastBeginMs = 0, gConnStartMs = 0, gScanStartMs = 0;
bool     gScanning    = false;
uint32_t gAttempts    = 0;   // #109/#121: every WiFi.begin counts
// #146: user-entered creds not yet persisted. The 12s kConnecting window
// regularly expires on a slow association (P4 at a mesh edge), after which
// the maintenance branch connects fine — the save must follow WHENEVER the
// association lands, not only inside the window, or the creds live in RAM
// only and the next reboot returns to the Welcome screen.
bool     gSavePending = false;

// #121 (Remote-only, flag-gated so the Controller binary is untouched): the
// P4 bench sits at the edge of a 2.4GHz mesh — associating to the driver's
// pick fails where pinning the strongest same-SSID BSSID succeeds, and the
// boot-time best node can degrade, so re-scan after repeated failures and
// back the retry cadence off 10s -> 60s (never a hammer). Ported verbatim
// from the retired remote_wifi.cpp (#96/#109 bench-proven).
#ifdef SLYTHERM_WIFI_PIN_BSSID
uint8_t  gBestBssid[6] = {};
int32_t  gBestChannel = 0;
bool     gHaveBest = false;
uint8_t  gFailedTries = 0;
constexpr uint8_t  kRescanAfterTries = 3;
constexpr uint32_t kRetryMinMs = 10000, kRetryMaxMs = 60000;
uint32_t gRetryMs = kRetryMinMs;

void scanForBest(){
  Serial.println("[wifi] scanning for strongest BSSID...");
  WiFi.scanDelete();
  int n = WiFi.scanNetworks(false, false);
  gHaveBest = false;
  int32_t bestRssi = -999;
  for(int i=0;i<n;i++){
    if(WiFi.SSID(i) == gActiveSsid && WiFi.RSSI(i) > bestRssi){
      bestRssi = WiFi.RSSI(i);
      gBestChannel = WiFi.channel(i);
      memcpy(gBestBssid, WiFi.BSSID(i), 6);
      gHaveBest = true;
    }
  }
  WiFi.scanDelete();
  if(gHaveBest) Serial.printf("[wifi] strongest \"%s\": ch=%d rssi=%d\n",
                              gActiveSsid, (int)gBestChannel, (int)bestRssi);
}
#endif

// All (re)association funnels through here so the pin/backoff/attempt
// accounting can't be bypassed. Plain WiFi.begin when not pinning.
void radioBegin(){
  ++gAttempts;
#ifdef SLYTHERM_WIFI_PIN_BSSID
  if(gHaveBest){ WiFi.begin(gActiveSsid, gActivePass, gBestChannel, gBestBssid); return; }
#endif
  WiFi.begin(gActiveSsid, gActivePass);
}

void saveCreds(const char* ssid, const char* pass){
  Preferences p; p.begin("wifi", false);
  p.putString("ssid", ssid); p.putString("pass", pass); p.end();
}

}  // namespace

bool begin(const char* fbSsid, const char* fbPass){
  gMux = xSemaphoreCreateMutex();
  Preferences p; p.begin("wifi", true);
  String s = p.getString("ssid", ""), pw = p.getString("pass", "");
  p.end();
  if(s.length()==0 && fbSsid && fbSsid[0]){ s = fbSsid; pw = fbPass ? fbPass : ""; }
  strlcpy(gActiveSsid, s.c_str(), sizeof(gActiveSsid));
  strlcpy(gActivePass, pw.c_str(), sizeof(gActivePass));
  WiFi.mode(WIFI_STA);
  if(gActiveSsid[0]){
#ifdef SLYTHERM_WIFI_PIN_BSSID
    scanForBest();
#endif
    radioBegin(); gLastBeginMs = millis();
  }
  return gActiveSsid[0] != 0;
}

bool connected(){ return WiFi.status()==WL_CONNECTED; }
uint32_t attempts(){ return gAttempts; }

bool hasSavedCredentials(){
  Preferences p; p.begin("wifi", true);
  String s = p.getString("ssid", ""); p.end();
  return s.length() > 0;
}

void service(uint32_t nowMs){
  // forget
  L(); bool fReq=gForgetReq; gForgetReq=false; U();
  if(fReq){ Preferences p; p.begin("wifi",false); p.remove("ssid"); p.remove("pass"); p.end();
    gActiveSsid[0]=0; gActivePass[0]=0; gSavePending=false;
    WiFi.disconnect(); L(); gConnSt=ConnState::kIdle; U(); }

  // connect request
  L(); bool cReq=gConnReq; gConnReq=false; char rs[33],rp[65];
  strlcpy(rs,gReqSsid,sizeof(rs)); strlcpy(rp,gReqPass,sizeof(rp)); U();
  if(cReq && rs[0]){ strlcpy(gActiveSsid,rs,sizeof(gActiveSsid)); strlcpy(gActivePass,rp,sizeof(gActivePass));
    gSavePending = true;  // #146: persist on association, however long it takes
#ifdef SLYTHERM_WIFI_PIN_BSSID
    gHaveBest = false;  // a user-chosen (possibly different) SSID: let the driver pick first
#endif
    WiFi.disconnect(); radioBegin(); gConnStartMs=nowMs;
    L(); gConnSt=ConnState::kConnecting; U(); }

  // #146: persist user-entered creds on the association that finally lands —
  // including one the maintenance branch made after the 12s UI window
  // expired (UI shows Failed, the driver associates seconds later).
  if(gSavePending && WiFi.status()==WL_CONNECTED){
    saveCreds(gActiveSsid,gActivePass); gSavePending=false;
    Serial.println("[wifi] credentials persisted");
  }

  // connect state machine + maintenance
  L(); ConnState cs=gConnSt; U();
  if(cs==ConnState::kConnecting){
    if(WiFi.status()==WL_CONNECTED){ L(); gConnSt=ConnState::kConnected; U(); }
    else if(nowMs-gConnStartMs>12000){ L(); gConnSt=ConnState::kFailed; U(); }
  }
#ifdef SLYTHERM_WIFI_PIN_BSSID
  else if(WiFi.status()==WL_CONNECTED){ gFailedTries = 0; gRetryMs = kRetryMinMs; }
  else if(gActiveSsid[0] && nowMs-gLastBeginMs>gRetryMs){
    gLastBeginMs=nowMs;
    gRetryMs = gRetryMs + gRetryMs/2; if(gRetryMs > kRetryMaxMs) gRetryMs = kRetryMaxMs;
    WiFi.disconnect();
    if(++gFailedTries >= kRescanAfterTries){ gFailedTries = 0; scanForBest(); }
    radioBegin();
  }
#else
  else if(gActiveSsid[0] && WiFi.status()!=WL_CONNECTED && nowMs-gLastBeginMs>15000){
    radioBegin(); gLastBeginMs=nowMs;
  }
#endif

  // scan request — SYNCHRONOUS. The async scan returns 0 while already
  // associated (confirmed on this ESP32-S3); a blocking scan enumerates
  // reliably whether connected or not. Blocks this task ~2-4 s; the UI keeps
  // animating "Scanning" on its own task.
  (void)gScanning; (void)gScanStartMs;
  L(); bool sReq=gScanReq; gScanReq=false; U();
  if(sReq){
    WiFi.scanDelete();
    int n = WiFi.scanNetworks(false /*sync*/, false /*show_hidden*/);
    Net tmp[kMaxNets]; int c=0;
    for(int i=0;i<n && c<kMaxNets;i++){
      strlcpy(tmp[c].ssid, WiFi.SSID(i).c_str(), sizeof(tmp[c].ssid));
      tmp[c].rssi = WiFi.RSSI(i);
      tmp[c].locked = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      c++;
    }
    WiFi.scanDelete();
    L(); memcpy(gNets, tmp, sizeof(Net)*c); gNetCount = c; gScanSt = ScanState::kDone; U();
  }
}

void requestScan(){ L(); gScanReq=true; gScanSt=ScanState::kScanning; gNetCount=0; U(); }
ScanState scanState(){ L(); ScanState s=gScanSt; U(); return s; }
int scanResults(Net* out,int maxN){ L(); int c=gNetCount<maxN?gNetCount:maxN; memcpy(out,gNets,sizeof(Net)*c); U(); return c; }
void requestConnect(const char* ssid,const char* pass){ L();
  strlcpy(gReqSsid,ssid,sizeof(gReqSsid)); strlcpy(gReqPass,pass?pass:"",sizeof(gReqPass));
  gConnReq=true; gConnSt=ConnState::kConnecting; U(); }
ConnState connState(){ L(); ConnState s=gConnSt; U(); return s; }
void forget(){ L(); gForgetReq=true; U(); }

void status(char* ssidOut,size_t ssidN,char* ipOut,size_t ipN,int8_t* rssiOut,bool* isConn){
  const bool c = WiFi.status()==WL_CONNECTED;
  if(isConn) *isConn = c;
  if(ssidOut) strlcpy(ssidOut, c?WiFi.SSID().c_str():gActiveSsid, ssidN);
  if(ipOut)   strlcpy(ipOut,   c?WiFi.localIP().toString().c_str():"", ipN);
  if(rssiOut) *rssiOut = c?static_cast<int8_t>(WiFi.RSSI()):0;
}

}  // namespace wifi_prov
