# Wall-UI setup flows — approved mockups (the spec)

Locked reference for the on-device setup UX. Implement to match these; they are
the source of truth for layout, states, and **plain-language copy** (no
broker/roster/MQTT/HA terms reach the homeowner — see issue #66).

| File | Flow | Notes |
| --- | --- | --- |
| `01-wifi-setup.html` | WiFi | state machine: Status → Scanning → List → Password → Connecting → Result; "Other network" for hidden SSIDs (no WPS). **Implemented.** |
| `02-home-system-connect.html` | Home-system connection | mDNS auto-discovery of the broker; homeowner path is **silent + automatic**. Manual host/port entry lives under **Installer → Advanced**, only if discovery fails. |
| `03-sensors.html` | Sensors | plain language: "Looking for sensors…" → choose rooms (use / follow-when-occupied / nudge reading). Firmware tracks sensors it has *seen but isn't using* so they can be offered; choices publish the roster. |

Onboarding order: **WiFi → (silent connect) → Sensors.** The homeowner never
sees "MQTT / broker / Home Assistant"; those live only under Installer.
