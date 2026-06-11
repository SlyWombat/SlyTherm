# spike/ — decision-spike evidence, NOT product code

Everything under this directory exists only as **evidence for the firmware-platform
decision** in [`../docs/08-firmware-platform-decision.md`](../docs/08-firmware-platform-decision.md)
(issue #38). It is retained so the decision can be re-examined, **not** maintained,
not tested by CI, and not a starting point for production work.

## Contents

- `esphome/ct485-spike.yaml` — minimal ESPHome node (esp32dev) mounting the
  external component below; Wi-Fi/API/OTA stubs with **placeholder credentials**
  (compile-only — never put real secrets here).
- `esphome/components/ct485_sniffer/` — a real ESPHome external component that
  wraps the repo's pure-C++ `lib/Ct485Frame` + `lib/Ct485Parser` for RX-only
  frame logging (valid-frame count sensor + last-decode text sensor). The repo
  libs are consumed **unmodified** via `platformio_options: lib_extra_dirs`
  (ESPHome generates `lib_ldf_mode: off`; the YAML forces it back to `chain`).

## Reproducing the compile

```sh
pip install --user --break-system-packages esphome   # spike used 2026.5.3
ESPHOME_DATA_DIR=/tmp/esphome-spike esphome compile spike/esphome/ct485-spike.yaml
```

The compile result, binary size, friction list, and the assessment derived from
this spike are recorded in `docs/08-firmware-platform-decision.md`.

## Known, deliberate limitations (part of the evidence)

- 3.5 ms inter-frame gap is inferred at `loop()` granularity, not from the UART
  RX-timeout interrupt — fine for sniffing (packets are >= 100 ms apart), not
  for the production TX/token path.
- RX only. No TX, no token handling, no demand path — by design.
