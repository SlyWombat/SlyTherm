#include "remote_mqtt.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include <cstring>

#include "mqtt_cfg.h"
#include "remote_wifi.h"

namespace remote_mqtt {
namespace {

WiFiClient gNet;
PubSubClient gMqtt(gNet);

char gId[16] = {};        // last 3 MAC bytes, hex -- e.g. "d12ac1"
char gClientId[32] = {};  // "slytherm-remote-d12ac1"
char gAvailTopic[48] = {};
char gTestTopic[48] = {};

uint32_t gLastDiscoverMs = 0;
uint32_t gLastConnectTryMs = 0;
uint32_t gLastPublishMs = 0;
uint32_t gRoundTripCount = 0;

void onMessage(char* topic, uint8_t* payload, unsigned int len) {
  if (strcmp(topic, gTestTopic) == 0) {
    gRoundTripCount++;
    Serial.printf("[mqtt] round-trip #%u OK: \"%.*s\"\n", gRoundTripCount, len, payload);
  }
}

void deriveIds() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(gId, sizeof(gId), "%02x%02x%02x", mac[3], mac[4], mac[5]);
  snprintf(gClientId, sizeof(gClientId), "slytherm-remote-%s", gId);
  snprintf(gAvailTopic, sizeof(gAvailTopic), "slytherm/remote/%s/status", gId);
  snprintf(gTestTopic, sizeof(gTestTopic), "slytherm/remote/%s/test", gId);
}

void discoverBroker(uint32_t nowMs) {
  if (mqtt_cfg::hostSet() || nowMs - gLastDiscoverMs < 15000) return;
  gLastDiscoverMs = nowMs;
  static bool sMdnsUp = false;
  if (!sMdnsUp) sMdnsUp = MDNS.begin(gClientId);
  IPAddress ip;
  uint16_t pt = 0;
  int n = MDNS.queryService("mqtt", "tcp");
  if (n > 0) {
    ip = MDNS.address(0);
    pt = MDNS.port(0);
  } else {
    n = MDNS.queryService("home-assistant", "tcp");
    if (n > 0) {
      ip = MDNS.address(0);
      pt = 1883;
    }
  }
  if (n <= 0) {
    IPAddress hip = MDNS.queryHost("homeassistant");
    if (hip != IPAddress(0, 0, 0, 0)) {
      ip = hip;
      pt = 1883;
      n = 1;
    }
  }
  if (n > 0 && ip != IPAddress(0, 0, 0, 0)) {
    char host[24];
    snprintf(host, sizeof(host), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    Serial.printf("[mqtt] mDNS discovered broker %s:%u\n", host, pt ? pt : 1883);
    mqtt_cfg::save(host, pt ? pt : 1883, "", "");
  }
}

void tryConnect(uint32_t nowMs) {
  if (!mqtt_cfg::hostSet() || gMqtt.connected() || nowMs - gLastConnectTryMs < 5000) return;
  gLastConnectTryMs = nowMs;

  char host[64] = {}, user[48] = {}, pass[64] = {};
  uint16_t port = 1883;
  mqtt_cfg::current(host, sizeof(host), &port, user, sizeof(user), pass, sizeof(pass));
  gMqtt.setServer(host, port);

  Serial.printf("[mqtt] connecting to %s:%u as \"%s\"...\n", host, port, gClientId);
  const char* muser = user[0] ? user : nullptr;
  const char* mpass = pass[0] ? pass : nullptr;
  if (gMqtt.connect(gClientId, muser, mpass, gAvailTopic, 0, true, "offline")) {
    gMqtt.publish(gAvailTopic, "online", true);
    gMqtt.subscribe(gTestTopic);
    Serial.println("[mqtt] connected, LWT registered, subscribed to test topic");
  } else {
    Serial.printf("[mqtt] connect failed, state=%d\n", gMqtt.state());
  }
}

}  // namespace

void begin() {
  deriveIds();
  Serial.printf("[mqtt] client id: %s\n", gClientId);
  gMqtt.setBufferSize(512);
  gMqtt.setCallback(onMessage);
  mqtt_cfg::begin("", 0, "", "");
}

void loop() {
  if (!remote_wifi::connected()) return;

  const uint32_t nowMs = millis();
  discoverBroker(nowMs);

  if (mqtt_cfg::takeDirty()) {
    gMqtt.disconnect();
  }

  if (gMqtt.connected()) {
    gMqtt.loop();
    if (nowMs - gLastPublishMs >= 10000) {
      gLastPublishMs = nowMs;
      char payload[32];
      snprintf(payload, sizeof(payload), "hello t=%lu", nowMs / 1000);
      gMqtt.publish(gTestTopic, payload);
    }
  } else {
    tryConnect(nowMs);
  }
}

bool connected() { return gMqtt.connected(); }

}  // namespace remote_mqtt
