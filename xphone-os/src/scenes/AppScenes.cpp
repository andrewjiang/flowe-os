#include "AppScenes.h"

#include "AboutScene.h"
#include "BlockScene.h"
#include "FileTransferScene.h"
#include "LauncherScene.h"
#include "NotificationsScene.h"
#include "PrioritiesScene.h"
#include "ReaderScene.h"
#include "SettingsScene.h"
#include "TodayScene.h"
#include "WorkoutScene.h"

unsigned long gBootTotalMs = 0;

// M4.2 last-scene restore: updated by every show*() helper below.
SceneId gCurrentSceneId = SceneId::Launcher;

// M4.2 wake diagnostics — set once by main.cpp boot(); defaults hold until then.
const char* gWakeResetReason = "?";
const char* gWakeRestoreScene = "none";

namespace {
// All scenes are static instances — fixed allocation, zero heap churn.
LauncherScene gLauncher;
AboutScene gAbout;
NotificationsScene gNotifications;
SettingsScene gSettings;
BlockScene gBlock;
PrioritiesScene gPriorities;
TodayScene gToday;
WorkoutScene gWorkout;
ReaderScene gReader;
FileTransferScene gFileTransfer;
}  // namespace

void showLauncher() {
  gCurrentSceneId = SceneId::Launcher;
  SCENES.switchTo(gLauncher);
}

void showAbout() {
  gCurrentSceneId = SceneId::About;
  SCENES.switchTo(gAbout);
}

void showNotifications() {
  gCurrentSceneId = SceneId::Notifications;
  SCENES.switchTo(gNotifications);
}

void showSettings() {
  gCurrentSceneId = SceneId::Settings;
  SCENES.switchTo(gSettings);
}

void showBlock() {
  gCurrentSceneId = SceneId::Block;
  SCENES.switchTo(gBlock);
}

void showBlockDeepWork() {
  gCurrentSceneId = SceneId::Block;
  SCENES.switchTo(gBlock);  // runs onEnter (fresh status request)
  gBlock.startDeepWork();   // then fire block.start(deep_work) immediately
}

void showPriorities() {
  gCurrentSceneId = SceneId::Priorities;
  SCENES.switchTo(gPriorities);
}

void showToday() {
  gCurrentSceneId = SceneId::Today;
  SCENES.switchTo(gToday);
}

void showReader() {
  gCurrentSceneId = SceneId::Reader;
  SCENES.switchTo(gReader);
}

void showWorkout() {
  gCurrentSceneId = SceneId::Workout;
  SCENES.switchTo(gWorkout);
}

void showFileTransfer() {
  gCurrentSceneId = SceneId::FileTransfer;
  SCENES.switchTo(gFileTransfer);
}

void showFileTransferAutoStart() {
  gCurrentSceneId = SceneId::FileTransfer;
  SCENES.switchTo(gFileTransfer);  // onEnter resets to Idle
  gFileTransfer.autoStart();       // then bring the radio up (STA or hotspot)
}

void stopFileTransferIfActive() {
  if (SCENES.active() == &gFileTransfer) gFileTransfer.stopAndRestart();
}

// boot() restore dispatch. Each show*() re-runs the scene's onEnter, which
// re-requests its companion data (Block/Priorities/Today/Notifications all do),
// so a restored scene refreshes itself. Sub-view state (Settings picker,
// Notifications detail, Block modes/break) resets to the scene default — fine.
void showSceneById(SceneId id) {
  switch (id) {
    case SceneId::Notifications: showNotifications(); break;
    case SceneId::Settings:      showSettings();      break;
    case SceneId::Block:         showBlock();         break;
    case SceneId::Priorities:    showPriorities();    break;
    case SceneId::Today:         showToday();         break;
    case SceneId::About:         showAbout();         break;
    case SceneId::Reader:        showReader();        break;
    case SceneId::Workout:       showWorkout();       break;
    // FileTransfer deliberately NOT restored: waking straight into a scene
    // that would show a stale Idle menu (the radio never survives sleep)
    // helps nobody — fall through to the launcher.
    case SceneId::FileTransfer:
    case SceneId::Launcher:
    default:                     showLauncher();      break;
  }
}

const char* sceneName(SceneId id) {
  switch (id) {
    case SceneId::Notifications: return "Notifications";
    case SceneId::Settings:      return "Settings";
    case SceneId::Block:         return "Block";
    case SceneId::Priorities:    return "Priorities";
    case SceneId::Today:         return "Today";
    case SceneId::About:         return "About";
    case SceneId::Reader:        return "Reader";
    case SceneId::Workout:       return "Workout";
    case SceneId::FileTransfer:  return "Transfer";
    case SceneId::Launcher:      return "Launcher";
    default:                     return "Launcher";
  }
}

void markLauncherDirtyIfActive() {
  if (SCENES.active() == &gLauncher) gLauncher.markDirty();
}

void markNotificationsDirtyIfActive() {
  if (SCENES.active() == &gNotifications) gNotifications.markDirty();
}

void markBlockDirtyIfActive() {
  if (SCENES.active() == &gBlock) gBlock.markDirty();
}

void markPrioritiesDirtyIfActive() {
  if (SCENES.active() == &gPriorities) gPriorities.markDirty();
}

void markTodayDirtyIfActive() {
  if (SCENES.active() == &gToday) gToday.markDirty();
}

void markWorkoutDirtyIfActive() {
  if (SCENES.active() == &gWorkout) gWorkout.markDirty();
}
