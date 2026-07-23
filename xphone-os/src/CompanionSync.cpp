#include "CompanionSync.h"

#include <Arduino.h>  // millis()
#include <cstdint>

#include "ble/CompanionBleService.h"

namespace CompanionSync {
namespace {

// The resync registry. ONE case per card-backed app: map its SceneId to the
// single card request that refills its RAM store. Non-card scenes
// (Notifications is covered by the ANCS backfill; Launcher/Settings/About have
// no companion store) map to kRequestNone and arm nothing. To add a new card
// app: add a case here.
constexpr int kRequestNone = -1;
constexpr int kRequestBlock = 0;
constexpr int kRequestPriorities = 1;
constexpr int kRequestToday = 2;
constexpr int kRequestWorkout = 3;

int requestForSceneId(SceneId id) {
  switch (id) {
    case SceneId::Block:      return kRequestBlock;
    case SceneId::Priorities: return kRequestPriorities;
    case SceneId::Today:      return kRequestToday;
    case SceneId::Workout:    return kRequestWorkout;
    default:                  return kRequestNone;  // Notifications/Launcher/Settings/About
  }
}

// Issue the card request. Returns false when the link is down (no send
// happened) — the retry backstop in main.cpp re-arms in that case.
bool sendRequest(int index) {
  switch (index) {
    case kRequestBlock:      return COMPANION_BLE.sendBlockStatus();
    case kRequestPriorities: return COMPANION_BLE.sendPrioritiesSyncRequest();
    case kRequestToday:      return COMPANION_BLE.sendTodaySyncRequest();
    case kRequestWorkout:    return COMPANION_BLE.sendWorkoutSyncRequest();
    default:                 return false;
  }
}

// Single armed request. kRequestNone means idle/disarmed. Touched only from the
// main loop (requestForScene/pump both run there), so no synchronization.
int pendingIndex = kRequestNone;
uint32_t pendingAtMs = 0;

}  // namespace

void requestForScene(SceneId id) {
  pendingIndex = requestForSceneId(id);
  pendingAtMs = millis();  // fire on the next pump()
}

void requestActive() { requestForScene(gCurrentSceneId); }

bool inProgress() { return pendingIndex != kRequestNone; }

void pump() {
  if (pendingIndex == kRequestNone) return;                        // idle
  if (static_cast<int32_t>(millis() - pendingAtMs) < 0) return;    // not due (rollover-safe)

  // Fire once, then disarm regardless of result: a single request has no
  // sibling to clobber, so there is nothing to stagger. If the send failed
  // (link dropped), the retry backstop in main.cpp notices and re-arms.
  sendRequest(pendingIndex);
  pendingIndex = kRequestNone;
}

}  // namespace CompanionSync
