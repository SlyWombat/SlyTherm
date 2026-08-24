// heap_debug.h — instrumentation for the #193 heap-corruption panic. Compiled
// ONLY into env:remote_p4_vpn_heapdbg (-DSLYTHERM_HEAPDBG); the fleet image is
// byte-identical without the flag.
//
// The bug: on 2026-08-17 the camera remote panicked inside gdma_default_tx_isr
// because the GDMA TX channel object at 0x4ff5cd90 held a corrupt group
// pointer (0x004ff5d5 — a HP-SRAM pointer shifted right one byte) and a
// nonsense chan_id (0x90000000). Nothing in this firmware deletes that
// channel, so it is not a use-after-free: something WROTE into a live struct.
// The coredump captures stacks + TCBs only, so the victim is all we ever see.
//
// Three nets, weakest to strongest:
//   1. heap integrity check   cheap, periodic, catches a write that crosses a
//      block boundary into a poison canary. NOTE: the framework ships
//      CONFIG_HEAP_POISONING_LIGHT (canaries at block head/tail only), so a
//      partial write INSIDE a block — exactly what #193 looks like — leaves
//      the canaries intact and this check returns true forever. It is the net
//      for a different, adjacent bug; it is NOT the instrument for #193.
//   2. software canary        snapshot N bytes at a known address and compare
//      every tick: proves corruption happened and shows which words changed to
//      what, within one interval. No writer backtrace.
//   3. CPU store watchpoint   the real instrument: the store traps in the
//      WRITER's context, so the coredump carries its backtrace. Armed on BOTH
//      cores (watchpoints are per-CPU) and only after camera/I2S bring-up, so
//      the GDMA driver's own init writes can't trip it.
//
// Detection aborts on purpose: the panic writes a coredump, and the next boot
// reports coredumpFresh on slytherm/remote/<id>/boot — that is the signal that
// reaches us on an off-LAN unit. Pull it with tools/pull_coredump.py.
//
// READING A TRIP — the two detections look different from the outside:
//   canary tick     runs in the heapdbg task: both hexdumps (was/now) hit the
//                   telnet log and Serial, then abort(). You see WHAT changed,
//                   not who wrote it.
//   watchpoint      the debug exception goes straight to the panic handler in
//                   the WRITER's context. die() is never involved and NOTHING
//                   is logged — from the outside it looks like a silent
//                   reboot. That is the better outcome: the coredump carries
//                   the writer's backtrace, which is the whole point.
//   a trip within seconds of boot means the address is aimed at the wrong
//                   object (some other live allocation passed the word-0
//                   plausibility check). Re-aim with -DSLYTHERM_HEAPDBG_ADDR
//                   from a fresh dump, or build -DSLYTHERM_HEAPDBG_WP=0 for
//                   canary-only. There is no automatic escape: this target has
//                   no catalog entry, so prefer deploying it by OTA (where
//                   ota::bootValidate() rolls a crash-looping image back) over
//                   a USB flash, which has no rollback.
#pragma once

#include <stdint.h>

namespace heap_debug {

// Start the watchdog task and arm the canary/watchpoint. Call ONCE, late in
// setup() — after remote_camera::begin(), or the driver's own channel-init
// stores trip the watchpoint immediately.
void begin();

}  // namespace heap_debug
