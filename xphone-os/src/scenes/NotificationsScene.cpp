#include "NotificationsScene.h"

#include <cstdio>
#include <cstring>

#include "../Fonts.h"
#include "../NotificationStore.h"
#include "../ble/CompanionAncsClient.h"  // appDisplayName(bundle id -> "Messages")
#include "AppScenes.h"

namespace {
constexpr int kMarginX = 20;
constexpr int kHeaderH = 46;
constexpr int kListTopPad = 8;  // gap between the header rule and row 0

// Copy `src` into `dst`, chopping whole UTF-8 sequences and appending "..."
// until it fits in `maxWidth` pixels. Fixed buffers + snprintf only.
void truncateToWidth(Gfx& gfx, const XpFont& font, const char* src, int maxWidth, char* dst, size_t dstSize) {
  snprintf(dst, dstSize, "%s", src ? src : "");
  if (gfx.textWidth(font, dst) <= maxWidth) return;

  size_t len = strlen(dst);
  while (len > 0) {
    // Strip one UTF-8 sequence (back over continuation bytes 0b10xxxxxx).
    do {
      len--;
    } while (len > 0 && (static_cast<uint8_t>(dst[len]) & 0xC0) == 0x80);
    dst[len] = '\0';
    char probe[192];
    snprintf(probe, sizeof(probe), "%s...", dst);
    if (gfx.textWidth(font, probe) <= maxWidth) {
      snprintf(dst, dstSize, "%s", probe);
      return;
    }
  }
}

// One list row: bold title line + regular message line + padding/separator.
int rowHeight(Gfx& gfx) { return gfx.lineHeight(kFontBold) + gfx.lineHeight(kFontRegular) + 14; }

// Header SYNC pill: a small rounded button in the header's top-right. Sized
// for the 10pt soft-key font so it reads as a *button*, not a row — the
// cursor lands on it from row 0 (UP) and the CONFIRM tab relabels to SYNC.
constexpr int kSyncPillH = 26;
constexpr uint32_t kSyncFlashMs = 1200;  // pressed-state invert duration
}  // namespace

void NotificationsScene::onEnter() {
  _view = View::List;
  // Default to the first notification (the common case: read, not sync);
  // an empty inbox parks the cursor on the header SYNC pill instead.
  _sel = NOTIFICATION_STORE.count() > 0 ? 0 : -1;
  _scroll = 0;
  _syncFlashUntilMs = 0;
  // Open-triggers-sync, matching the card scenes' onEnter: show what the
  // store already has instantly, and kick a fresh ANCS replay so anything
  // missed mid-session (dropped burst, timed-out fetch) lands within a few
  // seconds — rows repaint live via the store-revision pump in main.cpp.
  COMPANION_ANCS.requestResync();
}

const char* const* NotificationsScene::softKeys() const {
  // List: CONFIRM opens the selected row's detail view; its long-press (dot
  // on the tab) clears the whole inbox — CLEAR-the-lot moved off the tap so
  // one stray press can no longer wipe the list. With the header SYNC pill
  // selected (_sel == -1) the CONFIRM tab relabels to SYNC — the bar repaints
  // with every scene repaint, so the label follows the cursor for free.
  static constexpr const char* kList[4] = {"BACK", "OPEN", SoftKey::Left, SoftKey::Right};
  static constexpr const char* kListSync[4] = {"BACK", "SYNC", SoftKey::Left, SoftKey::Right};
  static constexpr const char* kListSyncEmpty[4] = {"BACK", "SYNC", nullptr, nullptr};
  static constexpr const char* kDetail[4] = {"BACK", "CLEAR", SoftKey::Left, SoftKey::Right};
  if (_view == View::Detail) return kDetail;
  if (_sel < 0) return NOTIFICATION_STORE.count() > 0 ? kListSync : kListSyncEmpty;
  return kList;
}

uint8_t NotificationsScene::longPressSlots() const {
  // Bit 1 marks the list's OPEN tab while there is something to clear
  // (long-press CONFIRM = clear all). No dot on the SYNC pill's tab — sync
  // has no long-press action, and none on BACK (see Scene::longPressSlots).
  if (_view == View::List && _sel >= 0 && NOTIFICATION_STORE.count() > 0) return 0x02;
  return 0;
}

int NotificationsScene::rowsPerPage(Gfx& gfx) const {
  const int avail = gfx.height() - kHeaderH - Scene::SOFTKEY_BAR_H - kListTopPad;
  const int rows = avail / rowHeight(gfx);
  return rows > 0 ? rows : 1;
}

XpRect NotificationsScene::listRect() const {
  if (_hCache <= 0) return XpRect{};  // no layout yet -> full-panel fallback
  return XpRect{0, kHeaderH, _wCache, static_cast<int16_t>(_hCache - kHeaderH - Scene::SOFTKEY_BAR_H)};
}

XpRect NotificationsScene::rowRect(const int visibleIndex) const {
  if (_hCache <= 0 || visibleIndex < 0 || visibleIndex >= _rowsPerPageCache) return XpRect{};
  // Row body is 2 text lines + 6px pad (64px); the selection border draws
  // 4px above/around it — 8px slop on every side keeps the rect honest.
  const int rowH = 29 + 29 + 14;  // matches rowHeight() (advanceY is 29 for both 12pt fonts)
  const int y = kHeaderH + kListTopPad + visibleIndex * rowH;
  return XpRect{0, static_cast<int16_t>(y - 8), _wCache, static_cast<int16_t>(rowH + 8)};
}

XpRect NotificationsScene::headerRect() const {
  if (_hCache <= 0) return XpRect{};  // no layout yet -> full-panel fallback
  return XpRect{0, 0, _wCache, kHeaderH};
}

void NotificationsScene::moveSelection(const int delta) {
  const int prev = _sel;
  _sel += delta;
  const int oldScroll = _scroll;
  if (_sel < _scroll) _scroll = _sel;
  if (_sel >= _scroll + _rowsPerPageCache) _scroll = _sel - _rowsPerPageCache + 1;
  if (_scroll != oldScroll) {
    markDirty(listRect());  // rows shifted — repaint the whole list region
    return;
  }
  // Same page: repaint only the two affected rows (the header SYNC pill
  // doesn't depend on _sel while the cursor stays among the rows).
  XpRect dirty = rowRect(prev - _scroll);
  dirty.unionWith(rowRect(_sel - _scroll));
  markDirty(dirty);
}

void NotificationsScene::handleInput(Input& in) {
  const int count = static_cast<int>(NOTIFICATION_STORE.count());
  // Entries may have arrived/expired since the last tick (revision pump) —
  // re-clamp before any index is used. -1 (header SYNC pill) is a valid
  // cursor position in the list view; an emptied store forces it.
  if (_sel > count - 1) _sel = count > 0 ? count - 1 : -1;
  if (_scroll > _sel && _sel >= 0) _scroll = _sel;
  if (_scroll < 0) _scroll = 0;

  // Sync-press feedback expiry (input tick, like BlockScene's transients).
  if (_syncFlashUntilMs != 0 && static_cast<int32_t>(millis() - _syncFlashUntilMs) >= 0) {
    _syncFlashUntilMs = 0;
    if (_view == View::List) markDirty(headerRect());
  }

  if (_view == View::Detail) {
    if (count == 0) {  // store emptied under us -> fall back to the (empty) list
      _view = View::List;
      _sel = -1;  // nothing left to select — park on the SYNC pill
      _scroll = 0;
      markDirty();
      return;
    }
    if (in.wasPressed(Btn::Back)) {  // back to the list, selection preserved
      _view = View::List;
      markDirty();
      return;
    }
    // PREV/NEXT: front Left/Right (under the PREV/NEXT tabs) and the top-edge
    // pair. Clamped at the ends — the list doesn't wrap, so neither does this.
    if ((in.wasPressed(Btn::Up) || in.wasPressed(Btn::Left)) && _sel > 0) {
      _sel--;
      markDirty();  // whole content + header position indicator change
      return;
    }
    if ((in.wasPressed(Btn::Down) || in.wasPressed(Btn::Right)) && _sel < count - 1) {
      _sel++;
      markDirty();
      return;
    }
    if (in.wasPressed(Btn::Confirm)) {  // CLEAR: remove THIS notification
      NotificationStore::Entry cleared;
      if (NOTIFICATION_STORE.get(static_cast<size_t>(_sel), cleared) &&
          NOTIFICATION_STORE.removeAt(static_cast<size_t>(_sel))) {
        // The local row disappears immediately. A dated date+title tombstone
        // then suppresses/retries any future replay with a fresh session UID.
        // Undated notifications intentionally skip the tombstone (title alone
        // is not a safe identity) but still get this immediate action attempt.
        NOTIFICATION_STORE.recordTombstone(cleared.sortKey, cleared.title);
        COMPANION_ANCS.dismissNotification(cleared.uid, cleared.categoryId, cleared.flags,
                                           cleared.sessionId);
        const int left = count - 1;
        if (left == 0) {  // inbox now empty -> back to the (empty) list
          _view = View::List;
          _sel = -1;  // park on the SYNC pill
          _scroll = 0;
        } else if (_sel > left - 1) {
          _sel = left - 1;  // removed the oldest -> show the previous one
        }
        // else: _sel now addresses the next (older) notification — stay.
        markDirty();
      }
    }
    return;
  }

  // --- List view -------------------------------------------------------------
  if (in.wasPressed(Btn::Back)) {
    showLauncher();
    return;
  }
  // CLEAR ALL only while a ROW is selected — the SYNC pill's tab has no
  // long-press action (and no dot), so a held press there stays a no-op.
  if (in.wasLongPressed(Btn::Confirm) && count > 0 && _sel >= 0) {
    NOTIFICATION_STORE.clearAll();
    _sel = -1;  // list is empty now — park on the SYNC pill
    _scroll = 0;
    markDirty();
    return;
  }
  if (in.wasPressed(Btn::Confirm)) {
    if (_sel < 0) {  // SYNC: force a fresh ANCS replay on demand
      COMPANION_ANCS.requestResync();
      _syncFlashUntilMs = millis() + kSyncFlashMs;
      if (_syncFlashUntilMs == 0) _syncFlashUntilMs = 1;  // 0 means idle
      markDirty(headerRect());
      return;
    }
    if (count > 0) {  // OPEN the selected row
      _view = View::Detail;
      markDirty();
    }
    return;
  }
  // Selection: top-edge Up/Down pair AND the front Left/Right buttons — the
  // latter sit directly under the soft-key bar's UP/DOWN tabs. UP from the
  // first row steps onto the header SYNC pill (-1); DOWN steps back off it.
  if (in.wasPressed(Btn::Up) || in.wasPressed(Btn::Left)) {
    if (_sel > 0) {
      moveSelection(-1);
    } else if (_sel == 0) {
      _sel = -1;
      // Full repaint, not header+row0: the CONFIRM soft-key relabels
      // OPEN -> SYNC at the panel's bottom edge, outside any small window.
      markDirty();
    }
  }
  if (in.wasPressed(Btn::Down) || in.wasPressed(Btn::Right)) {
    if (_sel < 0 && count > 0) {
      _sel = 0;
      _scroll = 0;   // pill sits above the list — re-anchor to the top
      markDirty();   // soft-key relabels back to OPEN (see UP above)
    } else if (_sel >= 0 && _sel < count - 1) {
      moveSelection(+1);
    }
  }
}

void NotificationsScene::render(Gfx& gfx) {
  const int count = static_cast<int>(NOTIFICATION_STORE.count());
  _wCache = static_cast<int16_t>(gfx.width());
  _hCache = static_cast<int16_t>(gfx.height());
  _rowsPerPageCache = rowsPerPage(gfx);
  // Same revision-safety clamps as handleInput (render may run first).
  if (_sel > count - 1) _sel = count > 0 ? count - 1 : -1;
  if (count == 0 && _view == View::Detail) _view = View::List;
  if (_sel >= 0 && _sel < _scroll) _scroll = _sel;
  if (_sel >= _scroll + _rowsPerPageCache) _scroll = _sel - _rowsPerPageCache + 1;
  if (_scroll < 0) _scroll = 0;

  if (_view == View::Detail) {
    renderDetail(gfx, count);
  } else {
    renderList(gfx);
  }
}

void NotificationsScene::renderList(Gfx& gfx) {
  const int w = gfx.width();
  const int count = static_cast<int>(NOTIFICATION_STORE.count());
  const int perPage = _rowsPerPageCache;

  // Header.
  char line[192];
  snprintf(line, sizeof(line), "Notifications (%d)", count);
  gfx.drawText(kFontBold, kMarginX, 8, line);

  // SYNC pill (top-right, where the old "n-m" scroll range indicator lived —
  // the count in the title already tells the story). Three states: idle
  // (thin outline), cursor-on (thick 3px border, matching the row cursor),
  // and just-pressed (inverted for kSyncFlashMs — "the press registered"
  // feedback while the replay lands in the background).
  {
    const bool selected = _sel < 0;
    const bool flashing = _syncFlashUntilMs != 0;
    const char* label = flashing ? "SYNCING" : "SYNC";
    const int textW = gfx.textWidth(kFontSmall, label);
    const int pillW = textW + 24;
    const int pillX = w - kMarginX - pillW;
    const int pillY = (kHeaderH - 2 - kSyncPillH) / 2;  // centred above the rule
    const int textY = pillY + (kSyncPillH - gfx.lineHeight(kFontSmall)) / 2 + 1;
    if (flashing) {
      gfx.fillRoundedRect(pillX, pillY, pillW, kSyncPillH, kSyncPillH / 2, true);
      gfx.drawTextCentered(kFontSmall, pillX + pillW / 2, textY, label, false);
    } else {
      gfx.drawRoundedRect(pillX, pillY, pillW, kSyncPillH, kSyncPillH / 2, selected ? 3 : 1, true);
      gfx.drawTextCentered(kFontSmall, pillX + pillW / 2, textY, label, true);
    }
  }
  gfx.fillRect(0, kHeaderH - 2, w, 2, true);

  if (count == 0) {
    gfx.drawTextCentered(kFontBold, w / 2, gfx.height() / 2 - gfx.lineHeight(kFontBold), "No notifications");
    gfx.drawTextCentered(kFontRegular, w / 2, gfx.height() / 2 + 6, "Notifications from iPhone land here");
    return;
  }

  const int textW = w - 2 * kMarginX;
  const int rowH = rowHeight(gfx);
  int y = kHeaderH + kListTopPad;

  NotificationStore::Entry entry;
  for (int i = 0; i < perPage; i++) {
    if (!NOTIFICATION_STORE.get(static_cast<size_t>(_scroll + i), entry)) break;

    // Selection cursor: 3px rounded border around the row's two text lines
    // (text at kMarginX=20 sits 12px inside the border at x=8 — high-contrast
    // per the e-ink rules, and moving it repaints just two rows).
    if (_scroll + i == _sel) {
      const int borderH = gfx.lineHeight(kFontBold) + gfx.lineHeight(kFontRegular) + 10;
      gfx.drawRoundedRect(8, y - 4, w - 16, borderH, 10, 3, true);
    }

    // Line 1 (bold): title, right-aligned app NAME sharing the line. The store
    // keeps the raw bundle id (stable key); resolve it to the friendly iOS name
    // ("Messages") at render — a name landing mid-list just repaints on the next
    // getAppNameRevision() tick (see main.cpp).
    char appName[32];
    COMPANION_ANCS.appDisplayName(entry.appId, appName, sizeof(appName));
    char app[48];
    truncateToWidth(gfx, kFontRegular, appName, textW / 3, app, sizeof(app));
    const int appW = gfx.textWidth(kFontRegular, app);
    char title[96];
    truncateToWidth(gfx, kFontBold, entry.title, textW - appW - 12, title, sizeof(title));
    gfx.drawText(kFontBold, kMarginX, y, title);
    gfx.drawText(kFontRegular, w - kMarginX - appW, y, app);
    y += gfx.lineHeight(kFontBold);

    // Line 2 (regular): message.
    char msg[160];
    truncateToWidth(gfx, kFontRegular, entry.message, textW, msg, sizeof(msg));
    gfx.drawText(kFontRegular, kMarginX, y, msg);
    y += gfx.lineHeight(kFontRegular) + 6;

    if (_scroll + i != _sel) gfx.fillRect(kMarginX, y, textW, 1, true);  // separator
    y += 8;
    if (y + rowH > gfx.height() - Scene::SOFTKEY_BAR_H) break;  // keep clear of chrome
  }
}

void NotificationsScene::renderDetail(Gfx& gfx, const int count) {
  const int w = gfx.width();
  NotificationStore::Entry entry;
  if (!NOTIFICATION_STORE.get(static_cast<size_t>(_sel), entry)) {  // belt-and-braces
    _view = View::List;
    renderList(gfx);
    return;
  }

  // Header: title + "n of m" position in the detail slot.
  gfx.drawText(kFontBold, kMarginX, 8, "Notification");
  char pos[24];
  snprintf(pos, sizeof(pos), "%d of %d", _sel + 1, count);
  gfx.drawText(kFontRegular, w - kMarginX - gfx.textWidth(kFontRegular, pos), 8, pos);
  gfx.fillRect(0, kHeaderH - 2, w, 2, true);

  const int textW = w - 2 * kMarginX;
  const int bottom = gfx.height() - Scene::SOFTKEY_BAR_H - 6;
  int y = kHeaderH + 10;

  // App name line (regular, one line) + short rule under it.
  char appName[32];
  COMPANION_ANCS.appDisplayName(entry.appId, appName, sizeof(appName));
  char app[48];
  truncateToWidth(gfx, kFontRegular, appName, textW, app, sizeof(app));
  gfx.drawText(kFontRegular, kMarginX, y, app);
  y += gfx.lineHeight(kFontRegular) + 4;
  gfx.fillRect(kMarginX, y, textW, 1, true);
  y += 10;

  // Title: bold, word-wrapped (store field is 56 bytes -> <= 3 lines here).
  const int titleLines =
      gfx.drawTextWrapped(kFontBold, kMarginX, y, entry.title[0] ? entry.title : "(no title)", textW, 3);
  y += (titleLines > 0 ? titleLines : 1) * gfx.lineHeight(kFontBold) + 8;

  // Message: regular, word-wrapped into whatever fits above the soft keys;
  // drawTextWrapped ends the last line with "..." when clipped.
  if (entry.message[0]) {
    const int maxLines = (bottom - y) / gfx.lineHeight(kFontRegular);
    if (maxLines > 0) gfx.drawTextWrapped(kFontRegular, kMarginX, y, entry.message, textW, maxLines);
  }
}
