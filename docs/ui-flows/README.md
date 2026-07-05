# Wall-UI flows — approved mockups (the spec)

Locked reference for the on-device UX. Implement to match these; source of
truth for layout, states, and **plain-language copy** (no broker/roster/MQTT/HA
jargon reaches the homeowner — #66).

| File | Screen | Status |
| --- | --- | --- |
| `01-wifi-setup.html` | WiFi | state machine; **implemented** |
| `02-home-system-connect.html` | Home-system connection | mDNS auto-discovery, silent; manual under Installer→Advanced; **implemented (backend)** |
| `04-home.html` | Home | **mode-aware** target (Heat/Cool→one, Auto→range, Off→none) + focal hero + logo mark (mark before wordmark) + presence rule + online/outdoor status |
| `05-presets-system-diag.html` | Presets / System / Diag | Presets active+apply+edit; System status rows (no "(valid)"); Diag alarms drill-in + read-only CT-485 bus monitor + Links |
| `03-sensors.html` | Sensors | plain-language chooser: use / follow-when-occupied / nudge; **"When no one's home"** default |

**Presence rule** (Home "reading from" line): Present → Last entered X min ago
(<1 h) → Last entered Y hr ago (rounded, 1–3 h) → after 3 h no presence anywhere
→ Nobody home · averaging N rooms.

Onboarding: WiFi → silent home-system connect → Sensors. Build tracked in the
UI v2 Stage A / Stage B issues.
