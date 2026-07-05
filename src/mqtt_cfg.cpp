// mqtt_cfg.cpp — see mqtt_cfg.h. NVS-backed broker config. The MQTT task owns
// the connection: it reads current() at boot, auto-discovers a broker over mDNS
// when none is saved (silent), and re-applies on takeDirty() after a UI save.

#include "mqtt_cfg.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace mqtt_cfg {
namespace {

SemaphoreHandle_t gMux = nullptr;
inline void L(){ if(gMux) xSemaphoreTake(gMux, portMAX_DELAY); }
inline void U(){ if(gMux) xSemaphoreGive(gMux); }

char     gHost[64] = {}, gUser[48] = {}, gPass[64] = {};
uint16_t gPort = 1883;
bool     gDirty = false, gConnected = false;

}  // namespace

bool begin(const char* fbHost, uint16_t fbPort, const char* fbUser, const char* fbPass){
  gMux = xSemaphoreCreateMutex();
  Preferences p; p.begin("mqtt", true);
  String h = p.getString("host", ""), u = p.getString("user", ""), pw = p.getString("pass", "");
  uint16_t port = static_cast<uint16_t>(p.getUInt("port", 0));
  p.end();
  if(h.length()==0 && fbHost && fbHost[0]){ h = fbHost; port = fbPort; u = fbUser?fbUser:""; pw = fbPass?fbPass:""; }
  strlcpy(gHost, h.c_str(), sizeof(gHost));
  strlcpy(gUser, u.c_str(), sizeof(gUser));
  strlcpy(gPass, pw.c_str(), sizeof(gPass));
  gPort = port ? port : (fbPort ? fbPort : 1883);
  return gHost[0] != 0;
}

void current(char* host, size_t hn, uint16_t* port, char* user, size_t un, char* pass, size_t pn){
  L();
  if(host) strlcpy(host, gHost, hn);
  if(port) *port = gPort;
  if(user) strlcpy(user, gUser, un);
  if(pass) strlcpy(pass, gPass, pn);
  U();
}

void save(const char* host, uint16_t port, const char* user, const char* pass){
  L();
  strlcpy(gHost, host?host:"", sizeof(gHost));
  gPort = port ? port : 1883;
  strlcpy(gUser, user?user:"", sizeof(gUser));
  strlcpy(gPass, pass?pass:"", sizeof(gPass));
  gDirty = true;
  U();
  Preferences p; p.begin("mqtt", false);
  p.putString("host", gHost); p.putUInt("port", gPort);
  p.putString("user", gUser); p.putString("pass", gPass);
  p.end();
}

bool takeDirty(){ L(); bool d = gDirty; gDirty = false; U(); return d; }
bool hostSet(){ L(); bool s = gHost[0] != 0; U(); return s; }
void setConnected(bool c){ gConnected = c; }
bool connected(){ return gConnected; }

}  // namespace mqtt_cfg
