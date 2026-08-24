#pragma once

// xphone-os M1/M2 — scene registry / navigation helpers.
//
// The scene instances live as statics in AppScenes.cpp; these helpers are
// how scenes navigate without including each other's headers.

#include <cstddef>
#include <cstdint>

// Shown in Settings and About. XPHONE_VERSION is the human-facing series;
// XPHONE_GIT_REV is the exact build (git describe), so a bug report names
// one commit instead of a marketing number nobody remembered to bump. Both
// come from include/generated/xphone_version.h, rewritten before every
// build by tools/gen_version.py.
#include "generated/xphone_version.h"
constexpr const char* XPHONE_VERSION = XPHONE_MARKETING;
constexpr const char* XPHONE_GIT_REV_STR = XPHONE_GIT_REV;

// M4.2 last-scene restore: a stable id for each restorable scene. Persisted in
// RTC memory at sleep (Sleep.cpp) and dispatched by boot() on wake so the
// device returns to whatever was on glass. Values are explicit so the
// RTC-stored integer is stable across firmware builds.
enum class SceneId : uint32_t {
  Launcher = 0,
  Notifications = 1,
  Settings = 2,
  Block = 3,
  Priorities = 4,
  Today = 5,
  About = 6,
  Reader = 7,
  Workout = 8,
  FileTransfer = 9,
};

// Single source of truth for "what scene is on glass" — set by every show*()
// helper below. Read by Sleep::sleepNow() to persist the restore target.
extern SceneId gCurrentSceneId;

// boot() restore dispatch: switch to the scene named by `id` (calls the
// matching show*() so the scene's onEnter re-requests its data). Unknown ids
// fall back to the launcher.
void showSceneById(SceneId id);

// Short human-readable name for a scene id ("Launcher"/"Block"/…), for the
// About wake diagnostic.
const char* sceneName(SceneId id);

void showLauncher();
void showAbout();
void readerShelfDump();
void showNotifications();
void showSettings();    // M3: real Settings scene (SD update / restart / about)
void showBlock();       // M3: real Block scene (Screen Time shields via BLE)
void showBlockDeepWork();  // Launcher top-right long-press: open Block + start Deep Work
void showPriorities();  // M3: real Priorities scene (to-do snapshot via BLE)
void showToday();       // M3: real Today scene (agenda/reminders/weather card)
void showReader();      // R1 EPUB reader (resumes the last book; book list on BACK)
void showWorkout();     // Workout: set-by-set exercise tracker synced from iPhone
void showFileTransfer();           // R2: Wi-Fi File Transfer scene (Idle menu)
void showFileTransferAutoStart();  // R2: same, but bring Wi-Fi up immediately (BLE transfer.start)
void showFileTransferAutoStartDirect();
// A transfer card while the scene is already up: behave like a fresh entry.
void fileTransferRestartFromCard(bool direct);  // W2: raise the device's own hotspot (BLE transfer.direct)
// R2: BLE "transfer.stop" — ack + restart when the transfer scene is active
// (restart is the clean Wi-Fi teardown); no-op on any other scene.
void stopFileTransferIfActive();

// Bench dev console ("where"): current launcher grid index (0-based,
// row-major, COLS=2 — Today/Notif, Prio/Block, Read/Workout).
int launcherSelection();

// Bench dev console ("where" v2): Reader sub-state one-liner.
void readerWhere(char* out, size_t n);

// M2: main.cpp marshals BLE/ANCS events to redraws with these — a scene is
// only marked dirty when it is the one on glass (e-ink discipline: a
// notification burst never repaints the launcher, a connection change never
// repaints Notifications' list rows for nothing).
void markLauncherDirtyIfActive();
void markNotificationsDirtyIfActive();
// M3: companion card revision changed (Block status card updates land here).
void markBlockDirtyIfActive();
// M3: same revision pump for the Priorities snapshot card.
void markPrioritiesDirtyIfActive();
// M3: same revision pump for the Today snapshot card.
void markTodayDirtyIfActive();
// Workout: revision pump for the workout snapshot card + local +/- bumps.
void markWorkoutDirtyIfActive();

// Total boot-to-first-paint time, set once by main.cpp (shown in About).
extern unsigned long gBootTotalMs;

// M4.2 wake diagnostics — captured ONCE in boot() before anything else can
// change them, rendered on the About scene so we can tell (without a serial
// cable) whether the X3 truly deep-sleeps or cold-boots on power-button wake,
// and whether the last-scene id was captured. gWakeResetReason is a short
// esp_reset_reason() string ("POWERON"/"DEEPSLEEP"/"BROWNOUT"/"SW"/"PANIC"/…);
// gWakeRestoreScene names the restored scene, or "none" on a cold boot.
extern const char* gWakeResetReason;
extern const char* gWakeRestoreScene;
