# SlyLog side-car sources (reference copies)

The SlyLog stack lives on the Dockge host (`kdocker2:/data/stacks/slylog/`) and
is **not** part of this git repo. These are reference copies of SlyTherm-owned
additions so they're version-controlled alongside the firmware that consumes them.

## `graph_publisher.py` — System-tab trend graph (#156)

Publishes the retained `slytherm/graph/system` series the device renders on its
System tab. A **separate process** from the collector (which by design never
publishes): it downsamples the last 12 h from TimescaleDB into 144 five-minute
buckets and posts three int-deci-degree arrays — `oat` (outside), `set` (active
setpoint), `room` (fused/averaged room temp) — every 5 min.

**Deploy** (on kdocker2):
1. `cp graph_publisher.py /data/stacks/slylog/collector/slylog_collector/`
2. Merge `graph-publisher.compose.yaml`'s `graph-publisher:` service into
   `/data/stacks/slylog/compose.yaml` (reuses the collector image, distinct
   `command: python -m slylog_collector.graph_publisher`).
3. `cd /data/stacks/slylog && docker compose up -d --build graph-publisher`

Firmware side (this repo): `slytherm/graph/system` is subscribed on the
Controller and Remotes; parsed by `ui_main.cpp::ingestGraphSeries` (MQTT task),
applied to the LVGL chart by `applyGraphIfDirty` (UI task). The `onMqttMessage`
copy buffers are sized to hold the ~1.8 KiB payload (a smaller buffer silently
drops it).

## `capture-receiver/` — #181 audit captures (who changed the thermostat)

The camera Remote POSTs a JPEG + metadata to `:8093/capture` whenever a person
makes a manual change at the panel (see `src/remote_capture.cpp`). Photos land
in `/data/slylog/audit-captures/YYYY-MM-DD/` (NOT `captures/` — that's the
CT-485 frame archive), the event index in `audit-captures/events.jsonl`, and
`http://kdocker2:8093/` serves a review page (newest first, inline photos).

**Deploy** (on kdocker2):
1. `mkdir -p /data/slylog/audit-captures && cp -r capture-receiver /data/stacks/slylog/`
2. Merge `capture-receiver.compose.yaml`'s service into
   `/data/stacks/slylog/compose.yaml`.
3. `cd /data/stacks/slylog && docker compose up -d capture-receiver`

Firmware side: receiver URL defaults to `http://192.168.10.12:8093/capture`,
overridable via retained MQTT `slytherm/cmd/capture_url` (`""` = default,
`off` = disable). One photo per 30 s cooldown; bursts coalesce; camera-down /
OTA-paused events still arrive metadata-only.
