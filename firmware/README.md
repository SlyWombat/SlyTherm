# firmware/ — OTA catalog (issue #60, docs/10)

`catalog.json` is the device-facing firmware catalog. Devices fetch it from

```
https://raw.githubusercontent.com/SlyWombat/SlyTherm/main/firmware/catalog.json
```

resolve their compile-time `SLYTHERM_OTA_TARGET` + `SLYTHERM_OTA_HWREV`
against `targets[]`, and download the matching release asset (`appUrl`).
Parser + resolution semantics: `lib/OtaCatalog` (host-tested in
`test/test_ota_catalog`).

**Do not edit by hand on a release.** CI (`tools/release.py`, issue #59)
regenerates and commits this file when a `v*` tag is pushed — versions, URLs,
hashes and signatures must come from the build. `targets` is empty until the
first release is cut.

Note: the repo must be public for devices to fetch this file and the release
assets (prerequisite gate recorded on epic #58).
