// coredump_server.h — LAN coredump retrieval for USB-less fielded units
// (#124). Both devices already write crash backtraces to their `coredump`
// flash partition; this serves that image on TCP :8082 so
// tools/pull_coredump.py can fetch + decode it with no cable.
//
// Protocol (mirrors the screenshot server's SLYSHOT shape):
//   client connects, optionally sends one command line within 250ms:
//     (none)/GET  -> "SLYCORE <size>\n" + raw partition bytes ("SLYCORE 0\n"
//                    when no dump is present)
//     ERASE       -> "OK\n" after esp_core_dump_image_erase() (explicit only —
//                    a pull never auto-erases)
// Bounded like the screenshot server: 15s overall cap + 2.5s no-progress
// stall detector, so a dead client can't wedge the calling task.
//
// Trust model: plaintext, trusted-LAN-only diagnostics — same posture as the
// :23 telnet log and :8081 screenshot server (documented together in docs/12).
#pragma once

namespace coredump_server {

// Call every loop iteration from ONE task (the MQTT/loop task). Lazily binds
// the listener once WiFi is up; a cheap no-op otherwise.
void poll();

}  // namespace coredump_server
