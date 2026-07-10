# OPNsense changes for the SlyTherm Remote WireGuard uplink (#148)

**For:** the firewall manager. **Requested:** 2026-07-10.
**Goal:** let one SlyTherm wall Remote (`dc25b0`, an ESP32-P4 panel) operate
from a WiFi network outside our LAN (currently the "media" SSID on the Bell
segment; later possibly anywhere) by carrying its MQTT traffic home through a
WireGuard tunnel that terminates on OPNsense.

**Scope guard: we also run Tailscale on this network. Nothing here touches
it.** This is a new, separate WireGuard instance on its own UDP port (51820 —
Tailscale uses 41641), its own interface, and its own tunnel subnet
(10.20.30.0/24 — no overlap with Tailscale's 100.64.0.0/10 or any LAN). Do
not modify any existing Tailscale interface, rule, or route.

## Topology

```
dc25b0 (WiFi "media", Bell segment)
   │  WireGuard, UDP → 192.168.2.2:51820
   ▼
OPNsense WAN (Bell-side, RFC1918)
   │  wg tunnel 10.20.30.0/24   (OPNsense 10.20.30.1, device 10.20.30.2)
   ▼
LAN 192.168.10.0/24 — device needs TCP 1883 to 192.168.10.12 (MQTT broker)
```

The device initiates; nothing ever connects *to* it. It sends a WireGuard
keepalive every 25 s.

## Keys (generated 2026-07-10, X25519)

| What | Value |
|---|---|
| Device **public** key (paste into the Peer) | `s+mlHLjSwfJz5YgJc9nnde+lKcqqnaflrJkgidW212U=` |
| Server **private** key (paste into the Instance) | `eOsGIZSX3QsHYK7C6JUzYqi8TUX45zJJUKwWwsnZl1s=` |
| Server public key (baked into the device firmware) | `BhgJxCQjJe9g9tSgB0OfZy7NoENyyO9xV7jzi+Kav3c=` |
| Preshared key (paste into the Peer) | `hKe5MCC92SB82pDQTfaW2hIXlwH/DuMmShatIkutkss=` |

Both keypairs were pre-generated so the device firmware could be baked in one
pass. If policy says the server key must be born on the firewall: generate a
fresh instance keypair instead, use it, and send back the new **public** key —
the device then needs a one-line secrets change and a reflash (2 minutes, our
side).

## Steps (OPNsense 24.x+)

1. **VPN → WireGuard → Instances → Add**
   - Name: `slytherm-remote`
   - Listen port: **51820**
   - Private key: (server private key above), public key auto-derives —
     confirm it shows `BhgJxCQjJe9g9tSgB0OfZy7NoENyyO9xV7jzi+Kav3c=`
   - Tunnel address: **10.20.30.1/24**
   - Save. Enable WireGuard (checkbox on the Instances tab) if not already
     enabled for another instance.

2. **VPN → WireGuard → Peers → Add**
   - Name: `slytherm-dc25b0`
   - Public key: (device public key above)
   - Pre-shared key: (PSK above)
   - Allowed IPs: **10.20.30.2/32**
   - Endpoint: leave empty (the device roams; it dials in)
   - Keepalive: leave empty (device sends every 25 s)
   - Tie it to the `slytherm-remote` instance.

3. **Interfaces → Assignments**: assign the new `wg` device as an interface
   (suggest name `SLYWG`), enable it, no IP config (the instance holds the
   tunnel address). This gives it its own firewall tab and keeps rules away
   from any Tailscale/WireGuard group rules.

4. **Firewall → Rules → SLYWG** (default deny; add only):
   - Pass IPv4 TCP, source `10.20.30.2/32`, destination an alias holding
     **192.168.10.11 and 192.168.10.12** (the broker host answers on both —
     deliberate, per the owner), port **1883** (MQTT — the device's whole
     job).
   - Pass IPv4 TCP, same source and destination alias, port **8090** — the
     #129 LAN OTA mirror (owner-approved 2026-07-10). Off-LAN devices keep
     their NVS-stored mirror URL and have no GitHub fallback, so without
     this their OTA checks fail.
   - Pass IPv4 ICMP, source `10.20.30.2/32`, destination `192.168.10.0/24`
     (diagnostics; optional but it makes bring-up much easier).
   - Nothing else. Specifically: no rule toward other LAN hosts, other VLANs,
     or the firewall itself.

5. **Firewall → Rules → WAN**: pass IPv4 **UDP**, destination "WAN address"
   port **51820**, source: `192.168.2.0/24` (tighter: the media DHCP range).
   Label it `slytherm wireguard`.
   - ⚠️ **"Block private networks" on the WAN interface must be unchecked**
     for this to work — the handshake arrives with an RFC1918 source. In this
     double-NAT setup (OPNsense WAN already lives behind the Bell router)
     that toggle is usually already off; verify rather than assume.

6. **No NAT changes.** Traffic from 10.20.30.2 to the LAN is plainly routed;
   the reply route back through the tunnel comes from the instance's tunnel
   network. Outbound NAT and the Tailscale config are untouched.

## Later (optional): true internet access, not just "media"

Nothing above changes. Additionally: on the **Bell router**, forward UDP
51820 → `192.168.2.2`, widen the WAN rule's source to `any`, and give
us a stable name for the endpoint (DDNS or the Bell static IP). The device
would be reflashed with the public endpoint. Consider rotating to
fresh keys at that point since these ones transited this doc.

## Gotcha found during bring-up (2026-07-10)

The broker host (kdocker2) intentionally carries **two IPs on the same NIC**
(`192.168.10.11` + `192.168.10.12`) and mosquitto answers on both. Devices
that auto-discover the broker at home typically store `.11` — so the SLYWG
MQTT rule must allow **both** destinations (step 4 above). If the rule was
first created against `.12` only, the symptom is a tunnel that's UP while
MQTT still fails with a plain TCP connect error.

## Verification

- **VPN → WireGuard → Status**: the `slytherm-dc25b0` peer shows a recent
  handshake (updates at least every 2 min once the device is powered and on
  "media"). No handshake = check WAN rule / block-private toggle first.
- On our side the device's telnet log (`nc <device-ip> 23`) prints
  `[wg] tunnel UP` and `[wg] route added`, then MQTT reconnects — the broker
  at 192.168.10.12 will log a session from `10.20.30.2`.

## Rollback

Disable or delete the `slytherm-remote` instance, the peer, the SLYWG
interface, and the two rules. Nothing else in the firewall references any of
it, by design.
