#pragma once

// xphone-os M4 — power-button deep sleep.
//
// sleepNow() paints a minimal sleep screen (FULL refresh — no RTC on X3/X4,
// so the glass shows a static wordmark, not a clock), tears the radios and
// panel down, arms the power button as the ESP32-C3 deep-sleep GPIO wakeup
// (esp_deep_sleep_enable_gpio_wakeup — the C3 has no ext0/ext1; pattern from
// x4-os lib/hal/HalPowerManager.cpp:63-95) and calls esp_deep_sleep_start().
// Wake is a chip reset through the normal boot path: the SD self-update check
// runs again and the first paint is a FULL refresh that cleanly replaces the
// sleep screen (main.cpp boot()).

#include <cstdint>

class Gfx;
class Input;

// M4 device auto-sleep: ms without any button input before the main loop
// calls Sleep::sleepNow() on its own (an active Block session pins the device
// awake — see main.cpp checkAutoSleep). Override via build_flags
// -DXP_AUTO_SLEEP_MS=...; 0 disables auto-sleep entirely.
#ifndef XP_AUTO_SLEEP_MS
#define XP_AUTO_SLEEP_MS 600000UL
#endif

// M4.2 shorter idle window WHILE A BLOCK IS ACTIVE (default 2 min). The iPhone
// (Screen Time) enforces the block independently, so the device sleeping has
// zero effect on it — an active block used to pin the panel/BLE/CPU on for the
// whole ~2h session and drain the cell. Now an active block sleeps fast; the
// block keeps running on the phone. Override via -DXP_AUTO_SLEEP_BLOCK_MS=...
#ifndef XP_AUTO_SLEEP_BLOCK_MS
#define XP_AUTO_SLEEP_BLOCK_MS 120000UL
#endif

namespace Sleep {

// Never returns: ends in esp_deep_sleep_start(). Call from the main loop only
// (draws with gfx, waits on input for the power-button release).
[[noreturn]] void sleepNow(Gfx& gfx, Input& input);

// M4.2 last-scene restore. sleepNow() persists the on-glass scene id
// (AppScenes.h gCurrentSceneId) in NVS flash (Arduino Preferences), which
// survives ANY reset including the X3's full power-on-reset power-button wake —
// RTC memory did not. boot() calls this once: returns true and writes `sceneId`
// when a saved key is present (and REMOVES the key so the next cold boot with
// no prior sleep goes to the launcher); false when no key is stored. Kept as a
// plain uint32_t so Sleep does not depend on the SceneId enum.
bool consumeRestoreScene(uint32_t& sceneId);

// M4.3 wake-from-active-block. sleepNow() persists a tiny Block snapshot
// (active/onBreak/preset/endsAtLabel/duration/remaining) to the same NVS
// namespace when BLOCK_STATUS is active, and clears those keys when it is not.
// boot() calls this once BEFORE the first render so BLOCK_STATUS holds the
// last-known active state immediately — the Block scene then shows the locked
// screen ("until 10:30 AM") instantly instead of "Syncing...". A fresh
// block-status card after BLE reconnect supersedes the seed. No-op when no
// snapshot is stored (cold boot, or the last block ended before sleep).
void seedPersistedBlock();

}  // namespace Sleep
