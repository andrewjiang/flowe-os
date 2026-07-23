#pragma once

// xphone-os M2/M3 — Notifications: scrollable newest-first list from the
// fixed-buffer NotificationStore (ANCS-fed), plus an M3 detail sub-view
// (internal state, not a stacked scene — same pattern as SettingsScene's
// menu/picker/confirm views).
//
// List view: front Left/Right and top-edge Up/Down move a selection cursor
// (3px rounded border around the row — selection moves repaint just the two
// affected rows), CONFIRM opens the selected notification's detail view,
// LONG-PRESS CONFIRM clears the whole inbox (the OPEN tab carries the
// long-press dot), BACK returns to the launcher.
//
// Header SYNC button (_sel == -1): pressing UP from the first row moves the
// cursor onto a small SYNC pill in the header's top-right (where the old
// "n-m" scroll range indicator lived); the CONFIRM soft-key relabels
// OPEN -> SYNC and pressing it forces a manual ANCS resync (the automatic
// onEnter resync sometimes lands before the link is ready — this is the
// user-visible retry). The pill flashes inverted for ~1.2 s as feedback.
// With an empty inbox the cursor parks on SYNC, so it is always reachable.
//
// Detail view: full app id line, word-wrapped bold title, word-wrapped
// message (Gfx::drawTextWrapped, "..." on the last line when clipped),
// "n of m" position in the header detail slot. PREV/NEXT step between
// notifications (clamped at the ends, matching the list's no-wrap UX),
// CONFIRM removes THIS notification (NotificationStore::removeAt), BACK
// returns to the list with the selection preserved.
//
// Renders only when dirty; the main loop marks it dirty when the store
// revision changes and this scene is visible (no redraws from BLE paths).
// Both views re-clamp the selection against the live store every
// handleInput/render tick, so entries arriving/expiring while the detail
// view is up can never cause an out-of-bounds read.

#include "../Scene.h"

class NotificationsScene : public Scene {
 public:
  void onEnter() override;
  void handleInput(Input& in) override;
  void render(Gfx& gfx) override;
  const char* const* softKeys() const override;      // list: BACK/OPEN/UP/DOWN, detail: BACK/CLEAR/PREV/NEXT
  uint8_t longPressSlots() const override;           // list: CONFIRM holds (clear all)

 private:
  enum class View : uint8_t { List, Detail };

  int rowsPerPage(Gfx& gfx) const;
  // M2.1a: logical rect of the scrollable list region (below the header,
  // above the soft-key bar), from dims cached by render(). Empty (=> full
  // panel fallback) before the first render.
  XpRect listRect() const;
  // Logical rect of one on-screen row slot (0.._rowsPerPageCache-1),
  // including the selection border slop. Empty before the first render.
  XpRect rowRect(int visibleIndex) const;
  // Logical rect of the header band (holds the SYNC pill) — repainted when
  // the cursor crosses between the pill and row 0, or on sync feedback.
  XpRect headerRect() const;
  void moveSelection(int delta);
  void renderList(Gfx& gfx);
  void renderDetail(Gfx& gfx, int count);

  View _view = View::List;
  int _sel = 0;     // selected entry (newest-first index); -1 = header SYNC pill
  int _scroll = 0;  // first visible list row (window follows _sel)
  int _rowsPerPageCache = 1;
  int16_t _wCache = 0, _hCache = 0;  // panel dims cached by render()
  // Sync-press feedback: pill renders inverted until this deadline (0 = idle).
  // Expiry is polled on the input tick (handleInput), matching BlockScene's
  // transient pattern — no timers, no BLE-path redraws.
  uint32_t _syncFlashUntilMs = 0;
};
