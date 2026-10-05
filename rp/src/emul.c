/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: emul.c
 * Description: MD/JS runtime bootstrap (ROM emulation + JS worker loop).
 */

#include "emul.h"

#include <stdint.h>

#include "debug.h"
#include "js_worker.h"
#include "mdjs_protocol.h"
#include "memfunc.h"
#include "pico/stdlib.h"
#include "romemul.h"
#include "target_firmware.h"

#if !MDJS_NO_NETWORK
#include "network.h"
#endif

#define SLEEP_LOOP_MS 10

void emul_start(void) {
  /* Copy the ST-side cartridge program into ROM-in-RAM. */
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);

  /* Serve cartridge bus requests using the protocol-only DMA lookup handler. */
  init_romemul(NULL, mdjs_dma_irq_handler_lookup, false);

#if !MDJS_NO_NETWORK
  /* Initialise WiFi in STA mode; it connects below. */
  network_wifiInit(WIFI_MODE_STA);
#endif

  /* Launch Core 1 JerryScript worker first, so the ST finds MD/JS at boot
   * however long WiFi takes to connect. */
  js_worker_init();

#if !MDJS_NO_NETWORK
  /* Connect using credentials from config (up to 30 s), serving the ST
   * meanwhile: the connect loop calls js_worker_loop() as it polls. */
  network_setPollingCallback(js_worker_loop);
  network_wifiStaConnect();
  network_setPollingCallback(NULL);
#endif

  while (true) {
    js_worker_loop();
#if !MDJS_NO_NETWORK
    network_safePoll(); /* lwIP's timers: DHCP renewal, ARP... */
#endif
    sleep_ms(SLEEP_LOOP_MS);
  }
}
