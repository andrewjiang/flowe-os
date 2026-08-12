"""Grant CompanionBleService access to BLEService's private destructor.

Upstream BLEServer has no destructor: BLEDevice::deinit deletes the server
but orphans every BLEService/BLECharacteristic created through it — measured
1.76 KB leaked per BLE init/deinit cycle (X4 bench, 2026-08-11), and the
reader suspends/resumes the radio once per book session. CompanionBleService
frees the orphaned characteristics itself (public destructors), but
BLEService's destructor is friend-only; this pre-build patch adds one friend
line to the framework copy so shutdownRadio can free the service too.

Safe because the platform is PINNED (platform-espressif32 55.03.37 in
platformio.ini), so the file below never changes underneath us. Idempotent:
re-runs detect the marker and do nothing. If a framework upgrade ever moves
the anchor, this raises and fails the build loudly rather than rotting.
"""

from pathlib import Path

Import("env")  # noqa: F821 - provided by SCons/PlatformIO

FRIEND_LINE = "friend class CompanionBleService;"
ANCHOR = "friend class BLEDevice;"

header = (
    Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32"))  # noqa: F821
    / "libraries" / "BLE" / "src" / "BLEService.h"
)

text = header.read_text()
if FRIEND_LINE not in text:
    if ANCHOR not in text:
        raise RuntimeError(
            f"patch_ble_service_friend: anchor '{ANCHOR}' not found in {header} — "
            "framework version changed? Re-verify the leak and this patch."
        )
    header.write_text(text.replace(ANCHOR, f"{ANCHOR}\n  {FRIEND_LINE}", 1))
    print(f"patch_ble_service_friend: patched {header}")
