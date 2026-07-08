"""version_flag.py — inject the firmware version as build defines (#113).

PlatformIO pre-script (extra_scripts = pre:tools/version_flag.py). Reads the
root VERSION file (single source of truth, semver MAJOR.MINOR.PATCH) plus the
git short sha + dirty state, and defines:

    SLYTHERM_FW_VERSION  "0.3.0"            (clean semver — OTA version compare)
    SLYTHERM_FW_BUILD    "0.3.0+g1a2b3c4"   (+ "-dirty" when the tree is dirty)

Degrades gracefully without git (CI tarball, exported tree): sha = "unknown".
Never fails the build — a missing VERSION file defines "0.0.0" loudly.
"""

import subprocess
from pathlib import Path

Import("env")  # noqa: F821  (PlatformIO SCons construction environment)

root = Path(env["PROJECT_DIR"])  # noqa: F821

try:
    version = (root / "VERSION").read_text(encoding="utf-8").strip()
    if not version:
        raise ValueError("VERSION file is empty")
except Exception as e:  # pragma: no cover - defensive
    print(f"version_flag.py: WARNING: {e}; defaulting to 0.0.0")
    version = "0.0.0"

sha = "unknown"
dirty = ""
try:
    sha = subprocess.check_output(
        ["git", "rev-parse", "--short", "HEAD"], cwd=root, text=True,
        stderr=subprocess.DEVNULL).strip() or "unknown"
    if subprocess.call(["git", "diff", "--quiet", "HEAD"], cwd=root,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) != 0:
        dirty = "-dirty"
except Exception:
    pass

build = f"{version}+g{sha}{dirty}"
print(f"version_flag.py: SLYTHERM_FW_VERSION={version}  SLYTHERM_FW_BUILD={build}")

env.Append(CPPDEFINES=[  # noqa: F821
    ("SLYTHERM_FW_VERSION", env.StringifyMacro(version)),  # noqa: F821
    ("SLYTHERM_FW_BUILD", env.StringifyMacro(build)),  # noqa: F821
])
