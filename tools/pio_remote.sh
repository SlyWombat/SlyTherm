#!/usr/bin/env bash
# Wrapper for all env:remote_p4 PlatformIO commands.
#
# Forces two env vars so a bare `pio ...` can never touch the shared
# ~/.platformio cache or grind through /mnt/c's slow DrvFS I/O:
#   PLATFORMIO_CORE_DIR  - isolated package/toolchain cache, native disk
#                          (NOT /tmp: the full P4 toolchain is ~7GB+ and
#                          this box's /tmp is a small tmpfs).
#   PLATFORMIO_BUILD_DIR - per-project build output (.pio/build equivalent),
#                          native /tmp (fast, and small - tens of MB).
#
# Usage: tools/pio_remote.sh run -e remote_p4
#        tools/pio_remote.sh run -e remote_p4 -t upload --upload-port COM6

set -euo pipefail

export PLATFORMIO_CORE_DIR="/home/sly/.platformio-remote-p4-core"
export PLATFORMIO_BUILD_DIR="/tmp/pio-remote-p4"

exec pio "$@"
