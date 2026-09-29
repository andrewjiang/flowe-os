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

#include <cstddef>
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
// Two-stage idle policy (Andrew, 2026-09-05): idle -> NAP (sleep screen on,
// chip napping, Bluetooth kept, any press wakes at once) -> OFF (deep sleep).
#ifndef XP_AUTO_NAP_MS
#define XP_AUTO_NAP_MS 300000UL         // 5 min idle -> nap
#endif
#ifndef XP_AUTO_NAP_BLOCK_MS
#define XP_AUTO_NAP_BLOCK_MS 120000UL   // 2 min while a block runs
#endif
#ifndef XP_AUTO_OFF_MS
#define XP_AUTO_OFF_MS 3600000UL        // 60 min idle -> off (never while charging)
#endif

namespace Sleep {
// Sleep policy in Settings (Andrew, 2026-09-06): idle minutes before the nap
// and before OFF. 0 = never. Stored in NVS ("sleep": napMin, offMin); the
// defaults are the 5 / 60 of the 5 September policy. A running block keeps
// its shorter nap window when that is shorter than the setting.
uint16_t napAfterMin();
uint16_t offAfterMin();
void setNapAfterMin(uint16_t min);
void setOffAfterMin(uint16_t min);
uint16_t cycleNapAfter(int delta);  // through the Settings choices; returns the new value
uint16_t cycleOffAfter(int delta);
void formatMinutes(char* out, size_t n, uint16_t min);  // "Never", "5 min", "1 h", "2 h 30"
// The Settings choices, for the picker screen (2026-09-06): a list with the
// current value marked, like every other list on the device.
int napChoiceCount();
uint16_t napChoiceAt(int i);
int offChoiceCount();
uint16_t offChoiceAt(int i);

// Settings > Sleep > Sleep screen (Andrew, 2026-09-06: a custom sleep screen,
// the person chooses). 0 = Priorities (the proven poster; Workout when napped
// from Workout), 1 = Last screen (the home hero the device slept from, from
// the home-apps lane; needs the Widget home). NVS "sleep": face.
enum class Face : uint8_t { Priorities = 0, LastScreen = 1 };
Face face();
void setFace(Face f);
Face cycleFace(int delta);
const char* faceName(Face f);

// Nap-screen background image (bottom-right/bottom-left buttons): a
// user-supplied /background.bmp (BackgroundImage.h) shown
// full-screen instead of the configured Face while napping. NOT a
// Face value and NOT gated behind Settings — the two buttons toggle it
// directly. Persisted in NVS ("sleep": bg) like the Face choice, so the LAST
// selection is what the device's next sleep (nap or OFF) starts on.
bool napShowingBackground();
// Full-screen "working on it" frame. The FIRST time a given background is
// shown it is read off the SD card and dithered whole, which takes seconds;
// without this the glass just sits on the old poster and the button looks
// dead. Call before drawSleepScreenNow() when BackgroundImage::cached() is
// false; the cached path is a straight blit and needs nothing.
void drawNapBackgroundLoading(Gfx& gfx);
// Turns the override on/off and persists it. Returns false (state left
// unchanged) when turning ON with no background image available, so a
// Right-press with no /background.bmp present is a silent no-op.
bool setNapBackgroundOverride(bool on);

// Bench A/B for the OFF poster tier: 0 = policy (FULL), 1 = HALF, 2 = FULL.
extern uint8_t gPosterTierOverride;
// P1.2 (efficiency test plan 2026-09-02): the X3's QMI8658 motion sensor is
// never used, but its oscillator runs from power-on until deep sleep. Put it
// to sleep once at boot. No-op when the sensor is not found.
void imuSleepAtBoot();

// Never returns: ends in esp_deep_sleep_start(). Call from the main loop only
// (draws with gfx, waits on input for the power-button release).
[[noreturn]] void sleepNow(Gfx& gfx, Input& input);

// Write the companion stores (Today, Priorities, Workout, notifications) to
// NVS now, so a restart re-seeds the CURRENT lists. sleepNow does this on the
// way to OFF; the quiet restart (main.cpp quietRestartToScene) must do it too,
// or the boot re-seeds whatever the last power-off left (2026-09-17).
void persistStoresForRestart();

// Ask a connected phone for fresh Priorities and Today snapshots and wait a
// short moment for each (about 300 ms; returns at once when the phone is not
// connected). sleepNow calls it before the OFF poster; the nap entry calls it
// before the nap poster for the same reason: the poster is only as fresh as
// the store, and a restart since the last push may have left it stale.
void requestFreshCardsNow();

/// The sleep screen, composed at full CPU speed. Used by
/// the nap (screen off, link kept) and by sleepNow on the way to deep sleep.
/// `napping`: the device is only resting (Bluetooth on, a press brings the
/// screen back at once). Both states use a light background. Deep sleep has
/// a larger moon and bold "asleep" footer. Refresh tiers stay state-specific.
void drawSleepScreenNow(Gfx& gfx, bool napping);

// Live nap poster (Andrew, 2026-09-06): while the device naps, a fresh
// priorities, workout, or Today snapshot from the phone redraws the poster
// with one FAST differential refresh (X4 0.6 s, no flash). Composes the nap
// poster again and flushes it only when the frame changed. Returns true
// when the glass was refreshed.
bool refreshNapPoster(Gfx& gfx);

// M4.2 last-scene restore. sleepNow() persists the on-glass scene id
// (AppScenes.h gCurrentSceneId) in NVS flash (Arduino Preferences), which
// survives ANY reset including the X3's full power-on-reset power-button wake —
// RTC memory did not. boot() calls this once: returns true and writes `sceneId`
// when a saved key is present (and REMOVES the key so the next cold boot with
// no prior sleep goes to the launcher); false when no key is stored. Kept as a
// plain uint32_t so Sleep does not depend on the SceneId enum.
bool consumeRestoreScene(uint32_t& sceneId);
// Arm the same key by hand for a deliberate restart (the reader's memory
// recovery, 2026-09-06): the next boot lands on `sceneId`.
void armRestoreScene(uint32_t sceneId);

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
