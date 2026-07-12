# Wall-unit screenshots (for the user manual)

Live 800×480 captures pulled from the SlyTherm wall unit over WiFi with
`tools/slyshot.py <ip> <out.png> <screen>`. The `<screen>` argument now drives
the panel to any tab or Settings sub-sheet before the snapshot (view-only —
never changes control state); see `tools/slyshot.py --list`. Kept current for
the user manual.

## Tabs

| File | Screen | Notes |
|------|--------|-------|
| `home.png` | Home (Cool) | single setpoint card, big hero, status dot |
| `home-cool.png` | Home (Cool) | single setpoint card |
| `home-heat.png` | Home (Heat) | single heat setpoint card |
| `home-auto.png` | Home (Auto) | stacked heat+cool cards |
| `home-off.png` | Home (Off) | system off |
| `presets.png` | Presets | home / away / sleep roster |
| `sensors.png` | Sensors | room sensor list, per-room On/Off participation |
| `system.png` | System | now-running / reading / bus / mode |
| `diag.png` | Diagnostics | alarms, CT-485 bus, links |

## Settings (reorganized into a category menu + sub-sheets, #128)

| File | Screen | Notes |
|------|--------|-------|
| `settings.png` | Settings menu | category cards with live one-line summaries |
| `settings-networking.png` | Networking sheet | WiFi setup / Home system, IP/gateway/subnet/signal/MAC, VPN status |
| `settings-fan.png` | Fan sheet | Auto / On / Circulate, minutes-per-hour, Low/Med/High |
| `settings-display.png` | Display sheet | 12/24-hour clock |
| `settings-security.png` | Security sheet | lock state / PIN |
| `settings-system.png` | System sheet | firmware, device info |
| `settings-wifi.png` | WiFi setup | network picker |
| `settings-home-server.png` | Home system | MQTT broker host/port/credentials |

## Overlays

| File | Screen | Notes |
|------|--------|-------|
| `hold.png` | Hold duration | hold-length chooser |
| `vacation.png` | Vacation | vacation setback schedule |
| `safe-mode.png` | Safe mode | recovery UI |

Recapture after any UI change — the `<screen>` token mirrors `navScreen()` in
`src/ui/ui_overlays.cpp`:

```
tools/slyshot.py <ip> docs/screenshots/settings-networking.png sheet:networking
```

Sheets are mutually exclusive: driving to any tab/sheet first hides every other
overlay, so each capture is single-modal (fixed 2026-07-12).
