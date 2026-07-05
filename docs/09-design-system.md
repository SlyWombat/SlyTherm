# SlyTherm Design System

The single source of truth for SlyTherm's visual identity across every surface:
the **wall touchscreen** (ESP32-S3 / LVGL), the **web installer**, **Home
Assistant** dashboards, the **logo/brand**, and these docs. When a color, size,
or spacing decision comes up, it comes from here — do not hand-pick values.

> **Why this exists:** early screens hand-picked colors and hit real problems —
> dark-grey headers on a dark background, and purple fringing on mid-grey body
> text. Both were contrast/role mistakes this system prevents.

---

## 1. Principles

1. **Dual-fuel is the brand.** Everything lives on the axis between **heat
   (ember)** and **cool (cryo)**. Warm accents mean gas/heat; cool accents mean
   the heat pump. This is functional, not decorative.
2. **Tokens, not hex.** Reference roles (`text.primary`, `accent.heat`,
   `state.critical`) — never a raw hex in a component. Swap the token, not the
   call sites.
3. **Semantic ≠ brand.** State colors (ok / warning / critical) are a *separate*
   family from the ember/cryo brand accents, so "heating" (ember) never reads as
   "warning."
4. **Contrast is a rule, not a preference.** Body text ≥ 4.5:1, large text ≥ 3:1
   against its surface. **Never use a tertiary/muted grey for text you must
   read** — that's the mistake that produced the unreadable headers and the
   purple sensor names.
5. **The wall UI is dark-first** (a device screen in a room); docs/web/HA are
   theme-aware but default light. Both themes are first-class.

---

## 2. Color tokens

Hex is authoritative. On the wall UI, `lv_color_hex(0xRRGGBB)` converts to
RGB565 automatically — use the hex below directly.

### 2a. Neutrals — dark theme (wall UI, default)

Blue-black, biased slightly toward cryo so it reads as "screen," not "paper."

| Token | Hex | Use |
| --- | --- | --- |
| `bg` | `#0B0F14` | app background |
| `surface` | `#151C25` | cards, panels, tab bar |
| `surface.raised` | `#1F2A36` | buttons, keypad keys, active tiles |
| `border` | `#2B3947` | hairlines, card outlines, dividers |
| `text.primary` | `#EAF0F6` | headings, values, primary reading (~13:1 on bg) |
| `text.secondary` | `#AEB9C4` | body copy, labels, list rows (~7:1) |
| `text.tertiary` | `#7A8895` | captions, hints ONLY (~3.6:1 — never body) |
| `text.disabled` | `#4C5763` | disabled controls |

### 2b. Neutrals — light theme (docs, web installer, HA cards)

| Token | Hex | Use |
| --- | --- | --- |
| `bg` | `#F4F6F9` | page background (cool off-white) |
| `surface` | `#FFFFFF` | cards |
| `border` | `#D9E0E7` | hairlines |
| `text.primary` | `#101820` | headings/body |
| `text.secondary` | `#4A5563` | secondary |
| `text.tertiary` | `#7A8895` | captions |

### 2c. Brand accents (functional)

| Token | Hex | Meaning |
| --- | --- | --- |
| `accent.heat` (ember-500) | `#FF7A18` | gas heat, heating action |
| `accent.heat.hi` (ember-300) | `#FFB020` | heat highlight, defrost tempering |
| `accent.heat.deep` (ember-700) | `#C4340B` | pressed/gradient end |
| `accent.cool` (cryo-500) | `#38BDF8` | heat-pump cool, cooling action, primary UI accent |
| `accent.cool.hi` (cryo-300) | `#7DD3FC` | cool highlight, fan |
| `accent.cool.deep` (cryo-700) | `#0E7CA8` | pressed/gradient end |

**Thermostatic gradient** (logo, hero, changeover): `accent.heat → accent.cool`
(`#FF7A18 → #38BDF8`), horizontal.

### 2d. Semantic / state (distinct from brand accents)

| Token | Hex | Use |
| --- | --- | --- |
| `state.ok` | `#37D39A` | online, healthy, satisfied, WiFi/MQTT up |
| `state.warning` | `#F2C14E` | compressor lockout, stale sensor, caution |
| `state.critical` | `#FF5D5D` | alarm, fault, emergency heat |
| `state.info` | `#58B7E8` | neutral info |

---

## 3. HVAC state → color (canonical mapping)

| State | Token |
| --- | --- |
| Heating (gas) | `accent.heat` `#FF7A18` |
| Cooling (heat pump) | `accent.cool` `#38BDF8` |
| Emergency / aux heat | `state.critical` `#FF5D5D` |
| Defrost / tempering | `accent.heat.hi` `#FFB020` |
| Fan only | `accent.cool.hi` `#7DD3FC` |
| Idle | `text.tertiary` `#7A8895` |
| Compressor lockout | `state.warning` `#F2C14E` |

---

## 4. Typography

**Wall UI (LVGL, Montserrat):** enable only the sizes used.

| Role | Size | Weight | Notes |
| --- | --- | --- | --- |
| Display (temperature) | 48 | 700 | the one huge number |
| Heading (screen title) | 28 | 700 | one per screen, top-left |
| Body / value | 20 | 400/700 | **default** — status bar, list rows, setpoints |
| Caption | 16 | 400 | hints under controls (tertiary color) |

Rule: **20 is the floor for anything you read at arm's length** on the 4.3"
panel. 14 is too small for a wall unit — do not use it for content.

**Docs / web / HA:** system UI sans for headings/body
(`-apple-system, "Segoe UI", system-ui, sans-serif`); **monospace**
(`ui-monospace, "Cascadia Code", Menlo, monospace`) for data — CT-485 hex, IPs,
topics, counters (the engineering vernacular).

---

## 5. Spacing & layout

- **Base unit: 4px.** Use multiples (4/8/12/16/24/32).
- **Radius:** cards `14`, buttons `10`, pills/keypad round; logo dial round.
- **Card padding:** 16 (12 on dense tiles).

**Wall unit grid (800×480):**

```
┌───────────────────────────────────────────────┐  ← 16px margins
│ status bar (34px): wifi · mqtt · bus   [logo]  │
├───────────────────────────────────────────────┤
│                                                 │
│   content zone (~390px)                         │
│                                                 │
├───────────────────────────────────────────────┤
│ tab bar (56px): Home Presets Sensors … Diag     │
└───────────────────────────────────────────────┘
```

---

## 6. Component specs

| Component | Spec |
| --- | --- |
| **Card** | `surface` bg, radius 14, no border (or `border` 1px), padding 16 |
| **Button (default)** | `surface.raised` bg, `text.primary`, radius 10, min 64×64 touch |
| **Button (active/selected)** | `accent.cool` bg, `bg`-color text |
| **Setpoint card** | heat = `accent.heat` label; cool = `accent.cool` label; big value `text.primary`; −/+ buttons ≥ 64px, hold-to-repeat |
| **Status pill** | `state.ok`/`state.warning`/`state.critical` text on `surface`; label + value |
| **HVAC action** | text in the HVAC-state color (§3) |
| **Tab bar** | `surface` bg, active tab `accent.cool`, inactive `text.secondary` |
| **Keypad** | modal `surface` + `accent.cool` 2px border; keys `surface.raised`; dots in `text.primary` |
| **Alarm banner** | `state.critical` bar, always visible, never gated by lock |

**Touch:** ≥ 64px hit targets; press-and-hold repeats with acceleration on
steppers; the touch layer only issues re-validated intents (never a demand).

---

## 7. Firmware token mapping (wall UI)

`src/main_ui_s3.cpp` (and the eventual `ui/lvgl` binding) uses these `#define`s.
Keep them in lockstep with §2:

```c
#define COL_BG        0x0B0F14   // neutral bg
#define COL_SURFACE   0x151C25   // surface
#define COL_RAISED    0x1F2A36   // surface.raised
#define COL_BORDER    0x2B3947   // border
#define COL_INK       0xEAF0F6   // text.primary
#define COL_TEXT2     0xAEB9C4   // text.secondary  (body copy, list rows)
#define COL_TEXT3     0x7A8895   // text.tertiary   (captions only)
#define COL_EMBER     0xFF7A18   // accent.heat
#define COL_EMBER_HI  0xFFB020   // accent.heat.hi
#define COL_CRYO      0x38BDF8   // accent.cool
#define COL_CRYO_HI   0x7DD3FC   // accent.cool.hi
#define COL_OK        0x37D39A   // state.ok
#define COL_WARN      0xF2C14E   // state.warning
#define COL_CRIT      0xFF5D5D   // state.critical
```

**Screen defaults:** set `text.primary` as the cascading default text color on
the root object so labels are never dark-on-dark; use `text.secondary` for body
rows; reserve `text.tertiary` for captions/hints.

---

## 8. Screen states — active & ambient idle

The wall UI has two states. **Active** is the full tabbed UI (Home, Presets,
Sensors, …). **Ambient idle** is a calm at-rest "screensaver with value."

**Transition:** after **5 minutes** with no touch → ambient. **Any touch** →
wake to the active **Home/control** screen; the waking touch only wakes (it does
NOT actuate the control it landed on).

**Ambient content** (dimmed design-system tokens — `text.secondary`/`tertiary`
on `bg`; "dimmer" is a low-luminance colour scheme, since the CH422G backlight
is on/off, not PWM):
- The **sensor currently driving demand** — dominant occupied participant name
  (e.g. "living room") — plus the **fused control temperature**, large.
- The **target** (heat or cool setpoint for the active mode).
- A **heat/cool graphic**: ember flame when heating, cryo snowflake when
  cooling, neutral when idle.

**Hard rules for ambient:**
- **Alarms override ambient** — a critical alarm shows full-brightness; ambient
  never hides a fault (docs/04 §1c).
- Ambient is **display-only** — no demand authority, like every UI surface.

**Presence detection:** idle-timeout only (no proximity sensor on the board).
Optional future signals: HA **room occupancy** (dim faster / at night); true
approach-wake would need a **PIR added to the carrier board** (hardware change).

**Data needed:** `SensorFusion` must expose the **dominant (top-weighted)
participant** for the "driving sensor" line, alongside per-sensor last-occupied
presence.

## 9. Rules of thumb (the lessons)

- Headers/values → `text.primary`; body/rows → `text.secondary`; captions →
  `text.tertiary`. **Never** put readable content in tertiary.
- Heating is ember, cooling is cryo — everywhere, always.
- State colors carry meaning; don't reuse them decoratively, and don't use ember
  for "warning."
- Set a cascading default text color + font on the root so new labels inherit
  legible values by default.
- Minimum on-device text size is **20px**.
