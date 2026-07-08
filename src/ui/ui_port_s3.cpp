// ui_port_s3.cpp — Controller board port (#114): Waveshare ESP32-S3 4.3B.
// LovyanGFX RGB panel + CH422G IO-expander (backlight/reset) + GT911 raw I2C
// @0x5D — the validated stage-1 stack, moved verbatim from the pre-split
// slytherm_ui.cpp. See ui_port.h for the contract.

#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <Wire.h>
#include <lvgl.h>

#include "ui_port.h"

extern "C" void uiNoteTouch();   // #90 sleep-state touch note (press edge)

// ---- LovyanGFX RGB panel (validated) ---------------------------------------
class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_RGB   _bus;
  lgfx::Panel_RGB _panel;
 public:
  LGFX(){
    { auto c=_panel.config(); c.memory_width=800; c.memory_height=480;
      c.panel_width=800; c.panel_height=480; c.offset_x=0; c.offset_y=0; _panel.config(c); }
    { auto c=_panel.config_detail(); c.use_psram=1; _panel.config_detail(c); }
    { auto c=_bus.config(); c.panel=&_panel;
      c.pin_d0=14;c.pin_d1=38;c.pin_d2=18;c.pin_d3=17;c.pin_d4=10;
      c.pin_d5=39;c.pin_d6=0;c.pin_d7=45;c.pin_d8=48;c.pin_d9=47;c.pin_d10=21;
      c.pin_d11=1;c.pin_d12=2;c.pin_d13=42;c.pin_d14=41;c.pin_d15=40;
      c.pin_henable=5;c.pin_vsync=3;c.pin_hsync=46;c.pin_pclk=7;c.freq_write=14000000;
      c.hsync_polarity=0;c.hsync_front_porch=20;c.hsync_pulse_width=10;c.hsync_back_porch=10;
      c.vsync_polarity=0;c.vsync_front_porch=10;c.vsync_pulse_width=10;c.vsync_back_porch=10;
      c.pclk_active_neg=0;c.de_idle_high=0;c.pclk_idle_high=1; _bus.config(c); }
    _panel.setBus(&_bus); setPanel(&_panel);
  }
};

namespace slytherm_ui {
namespace {

// ---- CH422G + GT911 (validated) ----
constexpr uint8_t kCh422Mode=0x24, kCh422Out=0x38, kBitTpRst=1<<1, kBitBl=1<<2, kBitLcdRst=1<<3;
uint8_t gCh=0;
void ch422M(uint8_t v){ Wire.beginTransmission(kCh422Mode); Wire.write(v); Wire.endTransmission(); }
void ch422O(uint8_t v){ gCh=v; Wire.beginTransmission(kCh422Out); Wire.write(v); Wire.endTransmission(); }
constexpr int kSda=8,kScl=9,kInt=4; constexpr uint8_t kGt=0x5D; bool gTouchOk=false;
void gtReset(){ pinMode(kInt,OUTPUT); digitalWrite(kInt,LOW);
  ch422O(gCh&~kBitTpRst); delay(10); digitalWrite(kInt,LOW); delayMicroseconds(120);
  ch422O(gCh|kBitTpRst); delay(5); digitalWrite(kInt,LOW); delay(50);
  pinMode(kInt,INPUT); delay(50); }
int gtReg(uint16_t r,uint8_t*b,uint8_t n){ Wire.beginTransmission(kGt); Wire.write(r>>8); Wire.write(r&0xFF);
  if(Wire.endTransmission(false)!=0) return 0; uint8_t g=Wire.requestFrom(kGt,n),i=0;
  while(i<g&&Wire.available()) b[i++]=Wire.read(); return i; }
bool gtRead(uint16_t&x,uint16_t&y){ static uint16_t lx=0,ly=0; static bool dn=false; static uint32_t readyMs=0; uint8_t s=0;
  if(gtReg(0x814E,&s,1)==1 && (s&0x80)){ readyMs=millis();
    if((s&0x0F)>0){ uint8_t d[6]={0}; if(gtReg(0x8150,d,6)>=4){ lx=d[0]|(d[1]<<8); ly=d[2]|(d[3]<<8); dn=true; } } else dn=false;
    Wire.beginTransmission(kGt); Wire.write(0x81); Wire.write(0x4E); Wire.write(0); Wire.endTransmission(); }
  else if(dn && millis()-readyMs>150) dn=false;  // no fresh GT911 sample -> released (frees LVGL idle timer)
  x=lx; y=ly; return dn; }

// ---- LVGL glue ----
LGFX gfx;
lv_disp_draw_buf_t drawBuf; lv_color_t buf1[800*40];
lv_disp_drv_t dispDrv; lv_indev_drv_t indDrv;
void flushCb(lv_disp_drv_t*d,const lv_area_t*a,lv_color_t*px){
  gfx.pushImage(a->x1,a->y1,a->x2-a->x1+1,a->y2-a->y1+1,(lgfx::rgb565_t*)px); lv_disp_flush_ready(d); }
void touchCb(lv_indev_drv_t*,lv_indev_data_t*dt){ uint16_t x,y; static bool wasDown=false;
  if(gTouchOk&&gtRead(x,y)){ dt->state=LV_INDEV_STATE_PR; dt->point.x=x; dt->point.y=y;
    if(!wasDown){ wasDown=true; uiNoteTouch(); } }   // #90: press edge -> sleep-state wake
  else { dt->state=LV_INDEV_STATE_REL; wasDown=false; } }

}  // namespace

bool portInit(){
  Wire.begin(kSda,kScl); Wire.setClock(400000);
  ch422M(0x01); ch422O(kBitTpRst|kBitLcdRst); delay(20);
  gfx.init(); gfx.setColorDepth(16); gfx.fillScreen(0);
  ch422O(kBitTpRst|kBitLcdRst|kBitBl);
  gtReset(); Wire.beginTransmission(kGt); gTouchOk=(Wire.endTransmission()==0);
  Serial.printf("[ui] GT911 %s\n", gTouchOk?"present":"NO ACK");
  lv_init();
  lv_disp_draw_buf_init(&drawBuf,buf1,nullptr,800*40);
  lv_disp_drv_init(&dispDrv); dispDrv.hor_res=800; dispDrv.ver_res=480; dispDrv.flush_cb=flushCb; dispDrv.draw_buf=&drawBuf; lv_disp_drv_register(&dispDrv);
  lv_indev_drv_init(&indDrv); indDrv.type=LV_INDEV_TYPE_POINTER; indDrv.read_cb=touchCb; lv_indev_drv_register(&indDrv);
  return true;
}

// #86: the panel backlight is CH422G bit kBitBl — ON/OFF only (no PWM pin on
// the 4.3B); portBacklight(false) leaves the GT911 alive so touch still wakes.
void portBacklight(bool on){ if(on) ch422O(gCh|kBitBl); else ch422O(gCh&~kBitBl); }

bool portTouchOk(){ return gTouchOk; }

}  // namespace slytherm_ui
