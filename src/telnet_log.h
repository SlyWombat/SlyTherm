// telnet_log.h — WiFi-accessible debug log for the wall unit (issue #37).
// Mirrors log lines to Serial AND any telnet client on port 23, so the
// mDNS/MQTT handshake and the UI state can be watched over WiFi without a USB
// cable (`telnet <ip> 23`). A small ring buffer replays recent history to a
// client that connects late. Compiled only into env:thermostat_s3.
//
// Not a shell — input is drained/ignored. No secrets are ever logged.
#pragma once

namespace telnet_log {

void begin();                      // create mutex + ring buffer; call once at boot
void poll();                       // accept/drop clients; call from ONE task (the MQTT task)
void log(const char* line);        // thread-safe: Serial + telnet clients + ring
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace telnet_log
