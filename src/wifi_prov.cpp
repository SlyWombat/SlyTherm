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
  if(gActiveSsid[0]){ WiFi.begin(gActiveSsid, gActivePass); gLastBeginMs = millis(); }
  return gActiveSsid[0] != 0;
}

bool connected(){ return WiFi.status()==WL_CONNECTED; }

bool hasSavedCredentials(){
  Preferences p; p.begin("wifi", true);
  String s = p.getString("ssid", ""); p.end();
  return s.length() > 0;
}

void service(uint32_t nowMs){
  // forget
  L(); bool fReq=gForgetReq; gForgetReq=false; U();
  if(fReq){ Preferences p; p.begin("wifi",false); p.remove("ssid"); p.remove("pass"); p.end();
    gActiveSsid[0]=0; gActivePass[0]=0; WiFi.disconnect(); L(); gConnSt=ConnState::kIdle; U(); }

  // connect request
  L(); bool cReq=gConnReq; gConnReq=false; char rs[33],rp[65];
  strlcpy(rs,gReqSsid,sizeof(rs)); strlcpy(rp,gReqPass,sizeof(rp)); U();
  if(cReq && rs[0]){ strlcpy(gActiveSsid,rs,sizeof(gActiveSsid)); strlcpy(gActivePass,rp,sizeof(gActivePass));
    WiFi.disconnect(); WiFi.begin(gActiveSsid,gActivePass); gConnStartMs=nowMs;
    L(); gConnSt=ConnState::kConnecting; U(); }

  // connect state machine + maintenance
  L(); ConnState cs=gConnSt; U();
  if(cs==ConnState::kConnecting){
    if(WiFi.status()==WL_CONNECTED){ saveCreds(gActiveSsid,gActivePass); L(); gConnSt=ConnState::kConnected; U(); }
    else if(nowMs-gConnStartMs>12000){ L(); gConnSt=ConnState::kFailed; U(); }
  } else if(gActiveSsid[0] && WiFi.status()!=WL_CONNECTED && nowMs-gLastBeginMs>15000){
    WiFi.begin(gActiveSsid,gActivePass); gLastBeginMs=nowMs;
  }

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
