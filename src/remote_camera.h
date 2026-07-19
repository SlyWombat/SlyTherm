// remote_camera.h — #150 tier 2: OV02C10 → ISP (RGB565) → hardware JPEG →
// HTTP MJPEG/snapshot server on :8080, for HA's generic-camera integration.
// Compiled only when SLYTHERM_CAM (currently env:remote_p4_vpn — the
// non-critical pilot Remote; coexistence with DSI/LVGL is under evaluation).
//
// Privacy model: the sensor streams internally from boot (it has no
// hardware indicator either way); the ENFORCED gate is the HTTP layer —
// when disabled, no frame ever leaves the device. Enable/disable rides
// MQTT (slytherm/remote/<id>/cmd/camera, "0"/"1") and the top bar shows a
// dot whenever a client is actually being served.
#pragma once

#include <cstddef>
#include <cstdint>

namespace remote_camera {

// Bring up XCLK + sensor SCCB + CSI/ISP/JPEG and start the capture + HTTP
// tasks. MUST be called after the UI port has initialized Wire, and BEFORE
// the UI task starts polling touch — the bring-up SCCB burst happens here.
// (Runtime SCCB is limited to the slow AE task, serialized by wireLock.)
void begin();

void setEnabled(bool on);  // privacy switch (MQTT-driven; default on, pilot)
bool enabled();
bool clientActive();       // a stream/snapshot client is being served now
uint32_t frames();         // captured frame count (diagnostics)

// #181 audit capture: encode the latest frame to JPEG into dst and return the
// byte count (0 = no frame yet / paused for OTA / encoder busy past waitMs /
// dst too small). Serialized against the :8080 serve paths (shared JPEG
// engine + PPA buffers) by an internal mutex. Deliberately independent of the
// privacy switch — owner decision on #181: the switch gates the live-view
// HTTP layer only; audit captures always fire. Safe from any task.
uint32_t captureStill(uint8_t* dst, size_t cap, uint8_t quality, int scaleDiv,
                      uint32_t waitMs);

// #180: pause the CSI capture for the duration of an OTA download. The camera's
// DMA + framebuffers starve the esp-hosted SDIO RX pool and a download
// crash-loops (sdio_drv.c:953) unless we free that bandwidth. Call
// suspendForOta() before starting the download; resumeAfterOta() restores
// capture if the download FAILED (on success the node reboots into the new
// image, so resume is only the fallback path). No-ops before begin().
void suspendForOta();
void resumeAfterOta();

// AE follow-up: the OV02C10 has no working internal AEC (0x3503 accepts
// writes but exposure never adapts — bench-verified), so a slow (2 Hz)
// host-side AE task writes the sensor's exposure/gain registers over the
// SHARED I2C bus at runtime. Every other Wire user (the GT911 touch poll in
// ui_port_p4) must bracket its bus access with this lock. No-ops before
// begin().
void wireLock();
void wireUnlock();

}  // namespace remote_camera
