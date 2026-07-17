# Geofencing & Remote Access (competitive P5 / P6)

Two features every reviewer's criteria list expects — and SlyTherm already has
the pieces, they just weren't packaged as marketable, no-vendor-cloud features.
This is the owner-facing recipe.

## Why this closes the gap

Every competitor advertises **geofencing** ("warms up when you head home") and
**app-from-anywhere** access. SlyTherm does both **fully locally, with no vendor
account** — the differentiator. It rides Home Assistant's own presence and
remote paths, so there's no SlyTherm cloud to depend on, breach, or shut down.

---

## 1. Geofencing (P5)

**What you get:** the thermostat drops to the **Away** preset when everyone
leaves and restores **Home** when the first person returns — driven by real
phone location, no manual "I'm leaving" tap.

**How it works:** the Home Assistant **Companion app** on each phone reports a
`person` entity's home/away state from the phone's GPS (that *is* geofencing).
The shipped **`SlyTherm: presence-based Away preset`** blueprint
(`ha/blueprints/slytherm_presence_away.yaml`) turns those `person` entities into
preset changes.

**Setup (5 min):**
1. Install the **Home Assistant Companion app** on each household phone and sign
   in to your HA instance; enable **Location** permission ("Always").
2. In HA, confirm each phone shows up as a `person` entity that flips
   `home`/`not_home` as you leave/arrive (Settings → People).
3. Import the blueprint (Settings → Automations → Blueprints → Import), pick the
   `person` entities that should count, and set the settle delay (default 10 min
   so a quick errand doesn't cycle the system).
4. Optionally set the "home" preset to your scheduled preset instead of a fixed
   one, so arrival hands back to the daily schedule (§ notes in the blueprint).

**Tuning the geofence radius:** that's a phone-side setting — HA Companion app →
Settings → Companion App → Sensors → *Zone-based tracking*, or widen HA's
`zone.home` radius (Settings → Areas & Zones). A 150–300 m home zone gives the
system a few minutes' head start to pre-condition before you walk in.

**Marketing line:** "Geofencing that runs on *your* network — the thermostat
learns you're on your way home from your phone's location, with no SlyTherm
account and no location data leaving your house."

---

## 2. Remote access, app-from-anywhere (P6)

**What you get:** full control and status from your phone anywhere — same
climate card, sensors, schedule — with **no port-forwarding and no vendor
cloud**.

**Recommended: Tailscale (no-cloud-optional, zero config).**
1. Install **Tailscale** on the machine running Home Assistant (the official HA
   add-on if you're on HAOS: Settings → Add-ons → Tailscale).
2. Install Tailscale on each phone and sign in with the same identity.
3. In the HA Companion app, set the **internal URL** to your HA host's Tailscale
   name (e.g. `http://homeassistant:8123` over the tailnet). The app now reaches
   HA from anywhere over the encrypted mesh — no ports opened, nothing exposed.

That's it. Because everything is local + Tailscale, there is **no SlyTherm or HA
cloud account** in the path. (HA Cloud/Nabu Casa is an alternative if you prefer
a paid managed relay, but the Tailscale path keeps the "no vendor cloud" story.)

**Off-LAN wall Remotes** already use this pattern at the firmware level (the
camera remote's WireGuard uplink, `docs/opnsense-vpn.md`) — same idea, applied to
a panel instead of a phone.

**Marketing line:** "Your thermostat, from anywhere, over an encrypted private
mesh — no open ports, no vendor cloud, no monthly fee."

---

## Where this sits vs. competitors

| | SlyTherm | ecobee / Nest / Honeywell |
|---|---|---|
| Geofencing | Yes — local, phone GPS via HA | Yes — via vendor cloud account |
| App from anywhere | Yes — Tailscale, no cloud | Yes — requires vendor cloud login |
| Location data leaves the home | **No** | Yes (vendor servers) |
| Works if the vendor shuts the service down | **Yes** | No |

The capability parity is table stakes; the **local, no-account** posture is the
story.
