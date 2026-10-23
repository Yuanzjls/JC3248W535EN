# Keep the firmware's version string current. ESP-IDF takes it from `git describe --tags --dirty`
# only when CMake configures the project, and PlatformIO doesn't reconfigure for a new commit or
# tag - so an OTA build could report an old version. When the commit / tag changed since the last
# build, drop the CMake cache so this build reconfigures. That rebuilds everything (~2 min), so it is
# keyed on the commit only, not on "-dirty" (which flips with every edit).
import subprocess
from pathlib import Path

Import("env")

build_dir = Path(env.subst("$BUILD_DIR"))
try:
    version = subprocess.run(
        ["git", "describe", "--always", "--tags"],
        cwd=env.subst("$PROJECT_DIR"),
        capture_output=True,
        text=True,
    ).stdout.strip()
except OSError:
    version = ""

stamp = build_dir / "version.stamp"
if version and (not stamp.exists() or stamp.read_text() != version):
    cache = build_dir / "CMakeCache.txt"
    if cache.exists():
        cache.unlink()
    build_dir.mkdir(parents=True, exist_ok=True)
    stamp.write_text(version)
    print("Firmware version: %s" % version)
