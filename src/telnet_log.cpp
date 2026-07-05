// telnet_log.cpp — see telnet_log.h.

#include "telnet_log.h"

#include <Arduino.h>
#include <WiFi.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace telnet_log {
namespace {

constexpr int kPort = 23;
constexpr int kMaxClients = 2;
constexpr int kRingLines = 48;
constexpr int kLineLen = 128;

WiFiServer gServer(kPort);
WiFiClient gClients[kMaxClients];
bool gServerUp = false;
SemaphoreHandle_t gMux = nullptr;

char gRing[kRingLines][kLineLen];
int  gRingHead = 0, gRingCount = 0;

inline void L(){ if(gMux) xSemaphoreTake(gMux, portMAX_DELAY); }
inline void U(){ if(gMux) xSemaphoreGive(gMux); }

void pushRing(const char* s){
  strlcpy(gRing[gRingHead], s, kLineLen);
  gRingHead = (gRingHead + 1) % kRingLines;
  if(gRingCount < kRingLines) gRingCount++;
}

void emit(const char* s){
  Serial.print(s);
  L();
  pushRing(s);
  for(int i = 0; i < kMaxClients; i++)
    if(gClients[i] && gClients[i].connected()) gClients[i].print(s);
  U();
}

}  // namespace

void begin(){ gMux = xSemaphoreCreateMutex(); }

void poll(){
  if(!gServerUp){
    if(WiFi.status() == WL_CONNECTED){ gServer.begin(); gServer.setNoDelay(true); gServerUp = true; }
    else return;
  }
  if(gServer.hasClient()){
    WiFiClient nc = gServer.available();
    L();
    int slot = -1;
    for(int i = 0; i < kMaxClients; i++)
      if(!gClients[i] || !gClients[i].connected()){ slot = i; break; }
    if(slot >= 0){
      gClients[slot] = nc;
      gClients[slot].print("== SlyTherm telnet log ==\r\n");
      int idx = (gRingHead - gRingCount + kRingLines) % kRingLines;
      for(int i = 0; i < gRingCount; i++){ gClients[slot].print(gRing[idx]); idx = (idx + 1) % kRingLines; }
    }
    U();
    if(slot < 0) nc.stop();
  }
  L();
  for(int i = 0; i < kMaxClients; i++)
    if(gClients[i] && gClients[i].connected())
      while(gClients[i].available()) gClients[i].read();  // drain + ignore input
  U();
}

void log(const char* s){ emit(s); }

void logf(const char* fmt, ...){
  char b[kLineLen];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(b, sizeof(b) - 2, fmt, ap);
  va_end(ap);
  if(n < 0) return;
  size_t len = strnlen(b, sizeof(b) - 2);
  if(len == 0 || b[len - 1] != '\n'){ b[len] = '\r'; b[len + 1] = '\n'; b[len + 2] = '\0'; }
  emit(b);
}

}  // namespace telnet_log
