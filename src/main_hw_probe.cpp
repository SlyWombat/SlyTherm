// main_hw_probe.cpp — bench inventory sketch (env:remote_p4_probe).
// Flash, read serial, reflash the real firmware. Reports chip identity and
// continuously scans the board's exposed I2C bus (GPIO7/8 — the same bus the
// GT911 touch lives on) so hot-plugged devices show up live. Same one-shot
// pattern as the #96 C6 helper: never ships, never runs unattended.

#include <Arduino.h>
#include <Wire.h>

#include "esp_chip_info.h"
#include "esp_mac.h"

namespace {

constexpr int kSda = 7, kScl = 8;  // ui_port_p4.cpp's panel/external bus

const char* guess(uint8_t a) {
  switch (a) {
    case 0x14: case 0x5D: return "GT911 touch (onboard)";
    case 0x20: case 0x21: case 0x22: case 0x23: return "PCF8574/MCP23017 IO expander?";
    case 0x24: return "CH422G mode reg?";
    case 0x36: return "MAX17048 fuel gauge?";
    case 0x38: return "AHT20 temp/hum or CH422G out?";
    case 0x3C: case 0x3D: return "SSD1306 OLED?";
    case 0x40: return "HTU21/SHT2x or INA219?";
    case 0x44: case 0x45: return "SHT31 temp/hum?";
    case 0x48: case 0x49: case 0x4A: case 0x4B: return "ADS1115/TMP102?";
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57: return "EEPROM?";
    case 0x62: return "SCD41 CO2?";
    case 0x68: return "DS3231 RTC or MPU6050?";
    case 0x69: return "MPU6050 (alt)?";
    case 0x76: case 0x77: return "BMP/BME280/680?";
    default: return "unknown";
  }
}

bool rd8(uint8_t addr, uint8_t reg, uint8_t* out) {  // 8-bit reg pointer read
  Wire.beginTransmission(addr); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, 1) != 1) return false;
  *out = Wire.read(); return true;
}
bool rd16reg(uint8_t addr, uint16_t reg, uint8_t* out, int n) {  // 16-bit reg (GT911)
  Wire.beginTransmission(addr); Wire.write(reg >> 8); Wire.write(reg & 0xFF);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, n) != n) return false;
  for (int i = 0; i < n; i++) out[i] = Wire.read();
  return true;
}

void identify(uint8_t a) {  // definitive ID-register interrogation
  uint8_t v = 0, v2 = 0, buf[4] = {};
  switch (a) {
    case 0x14: case 0x5D:
      if (rd16reg(a, 0x8140, buf, 4))
        Serial.printf("    0x%02X product id: '%c%c%c%c' (GT911 says '911')\n",
                      a, buf[0], buf[1], buf[2], buf[3]);
      break;
    case 0x18:
      if (rd8(a, 0x0F, &v) && v == 0x33) { Serial.printf("    0x18 WHO_AM_I=0x33 -> LIS3DH accelerometer\n"); break; }
      if (rd8(a, 0xFD, &v) && rd8(a, 0xFE, &v2))
        Serial.printf("    0x18 regs FD/FE = %02X/%02X (ES8311 codec says 83/11)\n", v, v2);
      break;
    case 0x36: {
      Wire.beginTransmission(0x36); Wire.write(0x08);  // MAX17048 VERSION (16-bit)
      if (Wire.endTransmission(false) == 0 && Wire.requestFrom(0x36, 2) == 2) {
        int ver = (Wire.read() << 8) | Wire.read();
        Serial.printf("    0x36 VERSION reg = 0x%04X (MAX17048 family reads 0x001x)\n", ver);
        Wire.beginTransmission(0x36); Wire.write(0x02);  // VCELL
        if (Wire.endTransmission(false) == 0 && Wire.requestFrom(0x36, 2) == 2) {
          int raw = (Wire.read() << 8) | Wire.read();
          Serial.printf("    0x36 VCELL = %.3f V\n", raw * 78.125e-6);
        }
      }
      {  // 0x36 is ALSO the OV5647 camera sensor's SCCB address (16-bit regs)
        uint8_t id[2] = {};
        if (rd16reg(0x36, 0x300A, id, 2))
          Serial.printf("    0x36 regs 300A/300B = %02X%02X "
                        "(OV5647 camera says 5647)\n", id[0], id[1]);
        else
          Serial.println("    0x36 16-bit reg read NAKed");
        uint8_t id2[2] = {};
        if (rd16reg(0x36, 0x0000, id2, 2))  // some sensors: id at 0x0000/0x0001
          Serial.printf("    0x36 regs 0000/0001 = %02X%02X\n", id2[0], id2[1]);
      }
      break; }
    default:
      if (rd8(a, 0x0F, &v)) Serial.printf("    0x%02X reg0x0F=0x%02X\n", a, v);
      break;
  }
}

int scanOnce(uint32_t hz) {
  Wire.end();
  Wire.begin(kSda, kScl, hz);
  int n = 0;
  for (uint8_t a = 1; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  0x%02X  %s\n", a, guess(a));
      identify(a);
      n++;
    }
    delay(2);
  }
  return n;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2500);
  Serial.println("\n=== SlyTherm hardware probe (remote_p4_probe) ===");

  esp_chip_info_t ci;
  esp_chip_info(&ci);
  Serial.printf("Chip: model=%d rev=%d.%d cores=%d features=0x%08lx\n",
                (int)ci.model, ci.revision / 100, ci.revision % 100, ci.cores,
                (unsigned long)ci.features);
  Serial.printf("Flash: %u MB   PSRAM: %u MB   heap free: %u\n",
                (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)),
                (unsigned)(ESP.getPsramSize() / (1024 * 1024)),
                (unsigned)ESP.getFreeHeap());
  uint8_t mac[6];
  if (esp_efuse_mac_get_default(mac) == ESP_OK)
    Serial.printf("eFuse base MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0],
                  mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.println("Note: TF-card slot and C6 radio are not probed here (SDIO "
                 "pins internal; C6 already verified 2.12.8 via hosted).");
}

void loop() {
  Serial.println("--- I2C scan, GPIO7/8 @ 100 kHz ---");
  int n = scanOnce(100000);
  Serial.printf("--- %d device(s). Rescan @ 400 kHz ---\n", n);
  int n4 = scanOnce(400000);
  if (n4 != n) Serial.printf("!!! count differs at 400 kHz: %d\n", n4);
  Serial.println("--- done; rescanning in 10 s (hot-plug friendly) ---");
  delay(10000);
}
