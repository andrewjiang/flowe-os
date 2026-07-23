#pragma once

// Launcher icon pack selection — packs live in art/LauncherIcons.h; the active
// index is persisted in NVS so Settings A/B survives sleep/restart.

#include <cstdint>

namespace IconStyle {

// Load from NVS once (safe to call repeatedly). Returns the active pack index
// clamped to [0, XPhoneIconPackCount).
uint8_t get();

// Persist and activate a pack. Clamped. Main-loop only.
void set(uint8_t packIndex);

// Bitmap for launcher app slot `appIndex` (0..APP_COUNT-1) from the active pack.
// Never null when packs are complete.
const uint8_t* iconForApp(int appIndex);

// Display name of the active pack (static literal from LauncherIcons.h).
const char* activeName();

// Display name of pack `i` (nullptr if out of range).
const char* packName(uint8_t i);

uint8_t packCount();

}  // namespace IconStyle
