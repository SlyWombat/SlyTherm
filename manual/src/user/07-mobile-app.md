# Mobile app and remote control

## The short version

There is no "SlyTherm app" to download — and that's a feature. Phone
control uses the free **Home Assistant Companion app** (iOS and Android),
talking to *your own* Home Assistant system inside your own home. No cloud
account, no subscription, no company server between your finger and your
furnace.

From the app you can:

- See the current temperature and adjust both setpoints.
- Change modes (Off / Heat / Cool / Auto) and presets (Home / Away / Sleep).
- See what's running (heating, cooling, defrosting) and the outdoor
  temperature.
- Edit schedules, view temperature history graphs, and receive alert
  notifications.

You also don't have to build the smart parts yourself: the thermostat ships
with a **ready-made Home Assistant starter package** that gives you a weekly
schedule, a vacation calendar, a filter-change reminder based on real blower
hours, temperature and humidity alerts, and automatic Away when everyone's
phone leaves the house — all out of the box. Your installer normally loads
it at commissioning; everything it creates is then visible and editable in
the app (see *Schedules and presets*).

## Setting it up

Your installer normally completes this at commissioning. If you're adding a
phone later:

1. Install **Home Assistant Companion** from the App Store or Google Play.
2. Open the app **while connected to your home Wi-Fi**. It discovers your
   Home Assistant system automatically (or enter its address, e.g.
   `http://homeassistant.local:8123`).
3. Sign in with the Home Assistant user account your installer created for
   your household (each family member can have their own).
4. Open the **thermostat card** — your installer will have placed it on the
   main dashboard. Tap the star to favourite it if you like.

That's all — on your home Wi-Fi, control is instant and fully local.

## Control from outside the home

Because there's no cloud middleman, reaching the thermostat from outside
your house requires a secure tunnel into your home network. Two good
options, both typically set up by your installer:

- **Tailscale or WireGuard (recommended, free).** A private, encrypted
  "virtual cable" between your phone and your home network. Once installed
  on the phone, the Companion app works from anywhere exactly as it does at
  home. Tailscale in particular is a simple app install plus account — no
  router surgery.
- **Home Assistant Cloud (Nabu Casa, paid subscription).** A zero-setup
  relay service from the Home Assistant developers (~US$6.50/month). It is
  the easy button, but it does route through a third-party server, which
  runs against the local-only philosophy of this product. Your choice.

**Privacy note:** with the recommended setup, your temperature data,
occupancy data, and control of your furnace never leave your home network.

## If the app can't connect

The thermostat does not need the app to function — the wall screen always
works, and temperature control continues uninterrupted. App connection
problems are network problems, in this order of likelihood:

1. Your phone is not on the home Wi-Fi (or the VPN/Tailscale is off, if
   you're away from home).
2. The Home Assistant computer is off or restarting.
3. Your router or Wi-Fi is down.

If Home Assistant stays unreachable for more than 30 minutes, the wall
thermostat quietly switches to its built-in fallback comfort range (heat to
18 °C / cool above 27 °C) until contact resumes — see *Schedules and
presets*.
