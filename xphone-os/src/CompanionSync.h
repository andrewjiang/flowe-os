#pragma once

// xphone-os — companion resync registry, SCENE-SCOPED.
//
// THE PROBLEM: on a deep-sleep wake the last scene is restored from NVS, but
// every companion store lives in RAM and is wiped by the wake/reboot. Scenes
// request their data in onEnter, which at boot fires a few seconds BEFORE BLE
// reconnects, so that request is sent while disconnected and dropped. Nothing
// re-requested when the link matured, so a restored card scene sat empty until
// the user left and re-entered it.
//
// THE FIX (scope reduction): on the isConnected() false->true edge main.cpp
// requests ONLY the ACTIVE scene's data — the one scene actually on glass —
// instead of resyncing all three card apps every wake. The other card scenes
// re-request their own data in their onEnter, so opening them later syncs them
// lazily; there is no reason to pull data for scenes the user can't see. This
// is now a SINGLE request per wake, so the old staged multi-send sequencer (it
// staggered block/priorities/today because they share one action
// characteristic and clobbered each other) is no longer needed for spacing —
// but the pump()/inProgress() plumbing is kept: it is cheap, harmless, and
// inProgress() feeds the status-bar sync dot.
//
// TO REGISTER A NEW CARD-BACKED APP'S AUTO-RESYNC: add one case to
// requestForSceneId() in CompanionSync.cpp mapping its SceneId to its send.
// This stays the ONE registry place a new card app is wired into wake-resync.

#include "scenes/AppScenes.h"  // SceneId, gCurrentSceneId

namespace CompanionSync {

// Arm a single resync request for scene `id` (Block/Priorities/Today send their
// card request; every other scene is a no-op). The send goes out on the next
// pump(). Re-arming simply replaces any pending request.
void requestForScene(SceneId id);

// Convenience: arm the request for whatever scene is currently on glass
// (reads gCurrentSceneId). This is what the wake edge in main.cpp calls.
void requestActive();

// Main-loop tick: fire the armed request once its (immediate) timer is due.
// Cheap no-op while idle. Never blocks (millis scheduling).
void pump();

// True while a request is armed but not yet sent. Feeds the sync dot.
bool inProgress();

}  // namespace CompanionSync
