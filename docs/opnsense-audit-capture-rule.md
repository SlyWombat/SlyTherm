# OPNsense change request: one firewall rule for the SlyTherm audit capture (#181)

**For:** the firewall manager. **Requested:** 2026-07-19.
**Goal:** let the off-LAN SlyTherm camera Remote (`dc25b0`, WireGuard peer
`10.20.30.2`) deliver its audit snapshots to the capture receiver on the
broker host. One new pass rule on the existing SLYWG interface; nothing else.

## Background (why)

SlyTherm v1.3.0 added a security feature (issue #181, owner-approved): when a
person makes a manual change on the camera Remote's panel, the device
photographs them and POSTs the JPEG + event metadata to a receiver on the
broker host, port **8093** (review page at `http://kdocker2:8093/`).

The Remote reaches the LAN through the existing `slytherm-remote` WireGuard
tunnel. The SLYWG ruleset is default-deny and currently passes only:

- TCP 1883 → broker-host alias (MQTT — the device's main job)
- TCP 8090 → broker-host alias (LAN OTA mirror, approved 2026-07-10)
- ICMP → 192.168.10.0/24 (diagnostics)

The new POST is therefore dropped. Confirmed live 2026-07-19: the device logs
`[capture] POST failed: HTTP -1` (TCP connect never completes) while MQTT and
the 8090 OTA path work normally over the same tunnel.

## The change (exact copy of the 8090 rule, different port)

**Firewall → Rules → SLYWG → Add:**

| Field | Value |
|---|---|
| Action | Pass |
| TCP/IP version | IPv4 |
| Protocol | TCP |
| Source | `10.20.30.2/32` |
| Destination | the existing broker-host alias (192.168.10.11 + 192.168.10.12) |
| Destination port | **8093** |
| Description | `slytherm audit capture (#181)` |

No NAT changes, no new interface, no WAN change, nothing touching Tailscale.

The destination stays the two-IP alias for the same reason as the MQTT rule
(the broker host deliberately answers on both `.11` and `.12`; the device's
compiled default targets `.12`, but the URL is runtime-configurable and may
follow whichever IP the broker discovery stored).

## Verification

1. Make any setpoint change on the camera Remote's touchscreen.
2. Within ~5 s an event (photo + metadata) appears at `http://kdocker2:8093/`.
3. Failure signature, if still blocked: `nc 10.20.30.2 23` (device live log)
   prints `[capture] POST failed: HTTP -1`.

## Rollback

Delete the one rule. The device logs-and-drops failed uploads by design —
nothing queues, nothing else degrades.
