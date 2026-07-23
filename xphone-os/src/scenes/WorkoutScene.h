#pragma once

// xphone-os — Workout: today's exercise list, synced from the iPhone as a
// "workout.snapshot" companion card and mirrored back set-by-set.
//
// Sets-only model: every exercise is "N sets x <free text>" ("10 x Pushups
// x10", "5 x Bench 135lb"); the device counts completed sets. When
// done == sets the exercise's checkbox fills; when every exercise is done the
// header shows the complete banner.
//
// Controls (redesigned per the workout use case — hands mid-set):
//   front Left/Right  = move selection UP/DOWN (under the soft-key tabs)
//   top-LEFT edge     = -1 set (fat-finger correction; unchecks if it drops
//                       a completed exercise below target)
//   top-RIGHT edge    = +1 set
//   CONFIRM           = +1 set too (biggest easy-to-hit button)
//   BACK              = launcher
//
// Sync model: +/- applies to WORKOUT_STORE immediately (optimistic repaint,
// PARTIAL refresh keeps it instant) and the ABSOLUTE new count is sent as a
// workout.set action after a short quiet window, so five rapid presses are
// one BLE packet, and a retried packet can't double-count. iOS acks by
// re-sending the snapshot card. Offline presses still count locally: the
// store is persisted through sleep (Sleep.cpp NVS card stash) and the phone
// merges with max(done) on reconnect.
//
// renderDormant() is the sleep-frame face used when the device sleeps FROM
// this scene: the workout list replaces the priorities list, everything else
// (calendar line, block line, footer) stays the sleep screen's own chrome.

#include <cstdint>

#include "../Scene.h"

class WorkoutScene : public Scene {
 public:
  void onEnter() override;
  void onExit() override;
  void handleInput(Input& in) override;
  void render(Gfx& gfx) override;
  const char* const* softKeys() const override;

  // Sleep-frame face: compose the workout list into the (already cleared)
  // framebuffer. Returns false when the workout store is empty — the caller
  // falls back to the priorities dormant list / wordmark. Never flushes.
  static bool renderDormant(Gfx& gfx);

  // Flush any coalesced-but-unsent workout.set update NOW (pre-sleep hook —
  // the quiet-window timer dies when the loop stops).
  static void flushPendingSend();

 private:
  void bumpSelected(int delta);
  XpRect listRect() const;

  int _sel = 0;
  int _scroll = 0;
  const char* _localMsg = "";  // static literals only
  uint32_t _seenStoreRevision = 0;
  int16_t _wCache = 0, _hCache = 0;
};
