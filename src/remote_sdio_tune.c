// #150 camera-remote stability: enlarge the esp-hosted SDIO RX buffer pool so
// heavy INBOUND load (VPN MJPEG stream + snapshot polling + reconnects + any
// inbound flood) does not deplete it and trip
//   assert failed: sdio_rx_get_buffer sdio_drv.c:953 (*buf)
//
// The P4<->C6 WiFi runs over esp-hosted/SDIO. The failing RX buffer pool
// `buf_mp_g` is created in the PRECOMPILED esp-hosted lib
// (libespressif__esp_hosted.a, esp32p4) inside bus_init_internal as
// (rx_queue_size + 11) blocks of ESP_RX_BUFFER_SIZE (1536 B), eagerly
// allocated in internal DMA RAM. It also sizes the RX counting semaphore
// (rx_queue_size * 3). `rx_queue_size` is NOT a baked compile constant:
// bus_init_internal loads it at runtime from the transport config struct
// (esp_hosted_sdio_get_config), using the port default (20) only when 0.
//
// The value flows in through the port default: arduino-esp32's hostedInit()
// (cores/esp32/esp32-hal-hosted.c) does
//     struct esp_hosted_sdio_config conf = INIT_DEFAULT_HOST_SDIO_CONFIG();
//     conf.pin_* = ...;                         // overrides pins only
//     esp_hosted_sdio_set_config(&conf);        // NOT_ALLOWED is fatal there
// INIT_DEFAULT_HOST_SDIO_CONFIG() == esp_hosted_get_default_sdio_config(), the
// port-layer default. We OVERRIDE that default here (both the plain and iomux
// variants, so the vendor's transport-defaults archive member is fully
// shadowed and never linked) and return a larger rx_queue_size. arduino's own
// set_config then carries it — no lib rebuild, no .a patch, no fighting the
// arduino init sequence (pre-seeding the config instead makes set_config fail
// with ESP_ERR_NOT_ALLOWED and kills WiFi). Only compiled into the camera
// build (remote_p4_vpn); tunable via -DSLYTHERM_SDIO_RX_Q.
//
// The pool is eagerly allocated in internal RAM (+1536 B per extra block), so
// the size is a measured trade against general internal-heap headroom.

#include "esp_hosted_transport_config.h"

#ifndef SLYTHERM_SDIO_RX_Q
#define SLYTHERM_SDIO_RX_Q 32
#endif

// Layout guard: our field-by-field fill assumes the vendor struct we
// reverse-engineered (72 bytes). If the pinned framework ever changes it, fail
// the build loudly rather than silently corrupt the SDIO config.
_Static_assert(sizeof(struct esp_hosted_sdio_config) == 72,
               "esp_hosted_sdio_config layout changed - revalidate remote_sdio_tune.c");

// Vendor default (verified against the precompiled port default), with only
// rx_queue_size enlarged. arduino overwrites every pin after this, so the pin
// values are a faithful fallback and not load-bearing.
struct esp_hosted_sdio_config esp_hosted_get_default_sdio_config(void) {
  struct esp_hosted_sdio_config c = {0};
  c.clock_freq_khz = 40000;  // CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ
  c.bus_width = 4;           // CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH
  c.slot = 1;                // CONFIG_ESP_HOSTED_SDIO_SLOT
  c.pin_clk.pin = 18;
  c.pin_cmd.pin = 19;
  c.pin_d0.pin = 14;
  c.pin_d1.pin = 15;
  c.pin_d2.pin = 16;
  c.pin_d3.pin = 17;
  c.pin_reset.pin = 54;
  c.rx_mode = 1;             // H_SDIO_HOST_STREAMING_MODE
  c.block_mode = true;       // H_SDIO_RX_BLOCK_ONLY_XFER
  c.iomux_enable = false;
  c.tx_queue_size = 20;      // unchanged (TX path is not the failing pool)
  c.rx_queue_size = SLYTHERM_SDIO_RX_Q;  // enlarged RX buffer pool (buf_mp_g)
  return c;
}

// iomux variant: vendor zeros the pins and sets iomux_enable. Not used on this
// board's non-iomux path, but must be defined to fully shadow the archive
// member (it is referenced, so a partial override would multiply-define).
struct esp_hosted_sdio_config esp_hosted_get_default_sdio_iomux_config(void) {
  struct esp_hosted_sdio_config c = {0};
  c.clock_freq_khz = 40000;
  c.bus_width = 4;
  c.slot = 1;
  c.rx_mode = 1;
  c.block_mode = true;
  c.iomux_enable = true;
  c.tx_queue_size = 20;
  c.rx_queue_size = SLYTHERM_SDIO_RX_Q;
  return c;
}
