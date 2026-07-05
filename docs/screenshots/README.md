# Wall-unit screenshots (for the user manual)

Live 800×480 captures pulled from the SlyTherm wall unit over WiFi with
`tools/slyshot.py <ip> <out.png> <screen 0-5>`. Kept current for the user manual.

| File | Screen | Notes |
|------|--------|-------|
| `home-cool.png` | Home (Cool) | single setpoint card, big hero, status dot |
| `home-auto.png` | Home (Auto) | stacked heat+cool cards |
| `presets.png` | Presets | (captured per build) |
| `sensors.png` | Sensors | room sensor list |
| `system.png` | System | now-running / reading / bus / mode |
| `settings.png` | Settings | PIN / lock, WiFi, home system |
| `diag.png` | Diagnostics | alarms, CT-485 bus, links |

Recapture after any UI change: `tools/slyshot.py 192.168.10.13 docs/screenshots/<name>.png <idx>`.
