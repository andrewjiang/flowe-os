#pragma once

// xphone-os M3 — Priorities: the day's to-do list, synced from the iPhone as
// a companion BLE card and mirrored back item-by-item.
//
// Faithful port of CrossPoint's PrioritiesActivity (x4-os
// src/activities/companion/PrioritiesActivity.{h,cpp}) onto the xphone scene
// framework, with the wire protocol kept byte-identical: the iOS app pushes a
// "priorities.snapshot" card whose priorityItems are 4-element arrays
// [id, title, note, done] (PrioritiesManager.swift:124-146, up to 10 items),
// and the device answers with "priorities.sync.request" /
// "priority.toggle" commands — iOS acks a toggle by re-sending a fresh
// snapshot card, so there is no separate ack message.
//
// Controls redesigned for the X3's physical layout, exactly the
// NotificationsScene mapping: front Left/Right (and the top-edge Up/Down
// pair) move the selection UP/DOWN, CONFIRM toggles the selected priority
// done/undone (soft key DONE; SYNC re-requests the snapshot while the list is
// empty), BACK returns to the launcher. Renders only when dirty; main.cpp
// marks the scene dirty on PRIORITIES_STORE revision changes (fresh
// snapshots) and companion card/status revision changes (the header's status
// line) while this scene is on glass.
//
// All list state lives in PRIORITIES_STORE (fixed buffers, filled by the
// companion service for every snapshot — even ones that arrive while another
// scene is on glass), so handleInput()/softKeys() read it directly on the
// 10 ms input tick with zero heap copies; no per-scene caches remain.
//
// renderDormant() is the M4 sleep-frame face: Sleep.cpp draws the same
// priorities list (via the shared row renderer) instead of the wordmark when
// the store holds a snapshot.

#include "../Scene.h"

class PrioritiesScene : public Scene {
 public:
  void onEnter() override;
  void handleInput(Input& in) override;
  void render(Gfx& gfx) override;
  const char* const* softKeys() const override;

  // M4 dormant sleep frame: compose the priorities list + "xphone" moon
  // stamp into the (already cleared) framebuffer. Returns false when the
  // priorities store is empty — the caller keeps the plain wordmark sleep
  // screen. Never flushes; Sleep.cpp owns the FULL refresh.
  static bool renderDormant(Gfx& gfx);

  // M5 sleep-frame chrome, shared with Sleep.cpp's plain wordmark screen so
  // both sleep faces read as one design. Both are centered one-liners drawn at
  // `y`; both return false when they had nothing to draw:
  //  - Block line: lock + "until 5:30 PM (2 today)" while a block is active,
  //    or lock + "2 blocks today" after completions.
  //  - Calendar line: calendar glyph + the next TIMED event from the Today
  //    store ("8:00 PM: Go to the mall", day-prefixed when not today).
  //    All-day events (birthdays etc.) are skipped.
  static bool renderDormantBlockLine(Gfx& gfx, int y);
  static bool renderDormantFooter(Gfx& gfx, int y);

 private:
  void toggleSelected();
  XpRect listRect() const;

  int _sel = 0;     // selected item index
  int _scroll = 0;  // first visible row (window follows _sel in render)
  const char* _localMsg = "Sync priorities from iPhone.";  // static literals only
  uint32_t _seenStoreRevision = 0;  // PRIORITIES_STORE.revision() last consumed by render()
  int _rowsPerPageCache = 1;
  int16_t _wCache = 0, _hCache = 0;  // panel dims cached by render()
};
