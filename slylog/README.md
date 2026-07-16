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
