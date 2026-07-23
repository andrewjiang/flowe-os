#pragma once

// SD-card firmware self-update for xphone-os.
//
// At boot (right after display init, before anything else) main() calls
// sd_update::checkAndApply(). It mounts the SD card and, if /update.bin
// exists at the SD root, validates it (ESP image header, segment table,
// XOR checksum, SHA256 trailer — same end-to-end pass CrossPoint runs in
// x4-os/src/network/FirmwareFlasher.cpp validateImageFile), streams it into
// the inactive OTA app slot with raw esp_partition writes, renames the file
// to /update.bin.flashed (so the next boot does NOT reflash forever), points
// otadata at the new slot, and restarts.
//
// Deliberate deviation from "esp_ota_begin/write/end": CrossPoint bypasses
// the esp_ota_* API on this hardware because the factory-bootloader-patched
// images fail esp_image_verify with bogus efuse-blk-rev errors (see
// x4-os/src/network/OtaBootSwitch.h header comment). We replicate its proven
// path: raw partition erase/write + a hand-rolled otadata switch.
//
// Failure policy: any error logs over serial, draws an X pattern on the
// e-ink, leaves otadata untouched, and RETURNS so normal boot continues.
// Never aborts, never bricks.

// EInkDisplay is a `using` alias for freeink::FreeInkDisplay (compatibility
// shim in freeink-sdk include/EInkDisplay.h), so it cannot be forward-declared.
#include <EInkDisplay.h>

namespace sd_update {

// Returns only when no update was applied (no SD card, no /update.bin, or a
// failure that left the current firmware in charge). On success it restarts
// the chip and never returns.
void checkAndApply(EInkDisplay& display);

// M3 Settings picker path: validate + flash an arbitrary firmware image at
// `path` (SD must already be mounted — the Settings scene mounts on entry).
// Same machinery as the boot path: full image validation, progress bar,
// inactive-OTA-slot raw flash, otadata switch, esp_restart() on success
// (never returns). If `path` is /update.bin it is retired to
// /update.bin.flashed first, exactly like the boot path, so the next boot
// does not reflash it. On any failure: logs, draws the error X, leaves the
// boot selection untouched, and returns false.
bool flashFromPath(EInkDisplay& display, const char* path);

}  // namespace sd_update
