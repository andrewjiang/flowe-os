#include "PrioritiesScene.h"

#include <cstdio>
#include <cstring>

#include "../BlockStatusStore.h"
#include "../Fonts.h"
#include "../PrioritiesStore.h"
#include "../SyncIndicator.h"
#include "../TodayStore.h"
#include "../ble/CompanionBleService.h"
#include "AppScenes.h"

namespace {

constexpr int kMarginX = 20;
constexpr int kHeaderH = 46;  // same chrome as NotificationsScene/BlockScene
constexpr int kCheckboxSize = 32;

// Width-clipping copy (same helper as NotificationsScene) — titles and notes
// come from the phone and can exceed a row.
void truncateToWidth(Gfx& gfx, const XpFont& font, const char* src, int maxWidth, char* dst, size_t dstSize) {
  snprintf(dst, dstSize, "%s", src ? src : "");
  if (gfx.textWidth(font, dst) <= maxWidth) return;
  size_t len = strlen(dst);
  while (len > 0) {
    do {
      len--;
    } while (len > 0 && (static_cast<uint8_t>(dst[len]) & 0xC0) == 0x80);
    dst[len] = '\0';
    char probe[160];
    snprintf(probe, sizeof(probe), "%s...", dst);
    if (gfx.textWidth(font, probe) <= maxWidth) {
      snprintf(dst, dstSize, "%s", probe);
      return;
    }
  }
}

// The phone-side parser occasionally emits a literal placeholder instead of
// an empty note ("none", "n/a", "-"); treat those as no note so the row
// renders as a single centered title line.
bool noteIsReal(const char* note) {
  if (!note || !note[0]) return false;
  auto matches = [note](const char* placeholder) {
    const char* a = note;
    const char* b = placeholder;
    while (*a && *b) {
      const char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
      if (ca != *b) return false;
      ++a;
      ++b;
    }
    return *a == '\0' && *b == '\0';
  };
  return !matches("none") && !matches("n/a") && !matches("-");
}

// CrossPoint's 32x32 rounded checkbox (PrioritiesActivity.cpp drawCheckbox:
// 28-33, border 3 when selected else 2). The tick is a real two-stroke
// diagonal check via Gfx::drawLine — short down-right stroke into the
// notch, long up-right stroke out of it, 3px stamp, centered inside the
// border.
void drawCheckbox(Gfx& gfx, const int x, const int y, const bool checked, const int thickness) {
  gfx.drawRoundedRect(x, y, kCheckboxSize, kCheckboxSize, 8, thickness, true);
  if (!checked) return;
  gfx.drawLine(x + 8, y + 16, x + 13, y + 22, 3, true);   // down-right into the notch
  gfx.drawLine(x + 13, y + 22, x + 24, y + 10, 3, true);  // long up-right stroke
}

// Crescent moon for the dormant stamp: full disc, then a white disc offset
// up-right carves it (paper is white, so the carve just erases).
void drawMoon(Gfx& gfx, const int x, const int y, const int d) {
  gfx.fillRoundedRect(x, y, d, d, d / 2, true);
  gfx.fillRoundedRect(x + d / 3, y - d / 5, d, d, d / 2, false);
}

// Small padlock glyph for the dormant BLOCK stamp — the same primitives as
// BlockScene::drawLock (rounded shackle + filled body + carved keyhole) but
// sized to stand next to stamp text (~d px tall). Always locked (a block is
// active whenever this is drawn). `d` is the glyph height budget.
void drawLockGlyph(Gfx& gfx, const int x, const int y, const int d) {
  // Taller-than-wide proportions (body w = 3/4 d) so the padlock reads as a
  // padlock next to a full text line instead of a squashed square.
  const int bodyW = (d * 3) / 4;
  const int bodyH = (d * 9) / 16;            // body ~56% of the height
  const int bodyX = x + (d - bodyW) / 2;
  const int bodyY = y + d - bodyH;           // body sits at the bottom of the budget
  const int shackleW = (bodyW * 3) / 5;      // shackle narrower than the body
  const int shackleX = x + (d - shackleW) / 2;
  const int shackleH = d - bodyH + (bodyH / 3);  // dips into the body so the arc reads closed
  gfx.drawRoundedRect(shackleX, y, shackleW, shackleH, shackleW / 2, 2, true);
  gfx.fillRoundedRect(bodyX, bodyY, bodyW, bodyH, 3, true);
  const int khW = d / 5;                     // keyhole carved white
  gfx.fillRoundedRect(x + (d - khW) / 2, bodyY + bodyH / 4, khW, khW, khW / 2, false);
  gfx.fillRect(x + d / 2 - 1, bodyY + bodyH / 4 + khW / 2, 2, bodyH / 3, false);
}

// Calendar glyph for the dormant next-event footer: page slightly taller than
// wide (w = 7/8 d), filled header band, two binding tabs, four date dots.
// `d` is the HEIGHT budget; returns nothing, occupies w x d at (x, y).
void drawCalendarGlyph(Gfx& gfx, const int x, const int y, const int d) {
  const int w = (d * 7) / 8;
  const int bodyY = y + 4;
  const int bodyH = d - 4;                              // page fills the height
  gfx.drawRoundedRect(x, bodyY, w, bodyH, 4, 2, true);
  gfx.fillRect(x + 2, bodyY + 2, w - 4, (d * 5) / 16, true);  // header band
  gfx.fillRect(x + w / 4 - 1, y, 3, 8, true);           // left binding tab
  gfx.fillRect(x + (3 * w) / 4 - 2, y, 3, 8, true);     // right binding tab
  const int dotTop = bodyY + (d * 5) / 16 + 5;          // date dots, 2x2 grid
  const int dotGapX = w / 3;
  const int dotGapY = (bodyY + bodyH - 5) - dotTop;
  gfx.fillRect(x + w / 4, dotTop, 3, 3, true);
  gfx.fillRect(x + w / 4 + dotGapX, dotTop, 3, 3, true);
  if (dotGapY >= 6) {
    gfx.fillRect(x + w / 4, dotTop + dotGapY / 2, 3, 3, true);
    gfx.fillRect(x + w / 4 + dotGapX, dotTop + dotGapY / 2, 3, 3, true);
  }
}

}  // namespace

void PrioritiesScene::onEnter() {
  _sel = 0;
  _scroll = 0;
  // Same entry behavior as PrioritiesActivity::onEnter (minus radio bring-up
  // — main.cpp owns BLE begin/advertising): ask the phone for a fresh
  // snapshot so the list is not stale from a previous session.
  if (COMPANION_BLE.isConnected() && COMPANION_BLE.sendPrioritiesSyncRequest()) {
    _localMsg = "Requesting priorities...";
  } else {
    // No transient in flight — leave the line empty so render() derives it live
    // from the connection ("Syncing..." vs "Connect Companion") and it can't go
    // stale after the link comes up post-wake.
    _localMsg = "";
  }
}

const char* const* PrioritiesScene::softKeys() const {
  static constexpr const char* kList[4] = {"BACK", "DONE", SoftKey::Left, SoftKey::Right};
  static constexpr const char* kEmpty[4] = {"BACK", "SYNC", nullptr, nullptr};
  return PRIORITIES_STORE.count() > 0 ? kList : kEmpty;
}

XpRect PrioritiesScene::listRect() const {
  if (_hCache <= 0) return XpRect{};  // no layout yet -> full-panel fallback
  return XpRect{0, kHeaderH, _wCache, static_cast<int16_t>(_hCache - kHeaderH - Scene::SOFTKEY_BAR_H)};
}

void PrioritiesScene::toggleSelected() {
  PrioritiesStore::Item item;
  if (_sel < 0 || !PRIORITIES_STORE.get(static_cast<std::size_t>(_sel), item)) return;
  if (item.id[0] == '\0') return;  // phone sent no id — nothing to address
  const bool desired = !item.done;
  // Optimistic flip + durable pending overlay: the checkbox changes at once and
  // survives a stale snapshot, so a priority can be checked off with the phone
  // away (matching Workout's local counting and Notifications' local clear).
  // The toggle is sent now if the link is up, and re-sent on reconnect
  // (main.cpp); the phone's confirming snapshot then clears the pending entry.
  PRIORITIES_STORE.setDoneLocal(static_cast<std::size_t>(_sel), desired);
  const bool sent = COMPANION_BLE.sendPriorityToggle(item.id, desired);
  _localMsg = sent ? "Updating priority..." : "Saved. Syncs when connected.";
  // setDoneLocal bumped the revision; adopt it as seen so render() does not
  // mistake our own local flip for a fresh phone snapshot and wipe the message.
  _seenStoreRevision = PRIORITIES_STORE.revision();
  markDirty();
}

void PrioritiesScene::handleInput(Input& in) {
  // Header transfer-arrow flash: bold on a new sent/received transient, back
  // to the thin idle pair ~1s later. Header-window repaint only.
  if (SyncIndicator::tick(COMPANION_BLE.getRevision(), millis(),
                          [] { return COMPANION_BLE.getStatusMessage(); })) {
    markDirty(XpRect{0, 0, _wCache, kHeaderH});
  }

  if (in.wasPressed(Btn::Back)) {
    showLauncher();
    return;
  }

  // The store is main-loop-only fixed state — count() on the 10 ms input
  // tick is a plain member read, no heap copies.
  const int count = static_cast<int>(PRIORITIES_STORE.count());

  if (in.wasPressed(Btn::Confirm)) {
    if (count > 0) {
      toggleSelected();  // DONE: mirror the checkbox to the phone
    } else {
      // SYNC: re-request the snapshot (PrioritiesActivity does the same on
      // Confirm with an empty list). Empty line when offline so render()
      // derives the live Syncing/Connect message.
      _localMsg = COMPANION_BLE.sendPrioritiesSyncRequest() ? "Requesting priorities..." : "";
      markDirty();
    }
    return;
  }

  // Selection: top-edge Up/Down pair AND the front Left/Right buttons — the
  // latter sit directly under the soft-key bar's UP/DOWN tabs (same mapping
  // as NotificationsScene).
  if ((in.wasPressed(Btn::Up) || in.wasPressed(Btn::Left)) && _sel > 0) {
    _sel--;
    markDirty(listRect());
  }
  if ((in.wasPressed(Btn::Down) || in.wasPressed(Btn::Right)) && _sel < count - 1) {
    _sel++;
    markDirty(listRect());
  }
}

void PrioritiesScene::render(Gfx& gfx) {
  const int w = gfx.width();
  const int h = gfx.height();
  _wCache = static_cast<int16_t>(w);
  _hCache = static_cast<int16_t>(h);

  // A FRESH snapshot (the store's revision moves only when one lands) is the
  // phone's answer to any in-flight sync/toggle — clear the transient message.
  const uint32_t storeRevision = PRIORITIES_STORE.revision();
  if (storeRevision != _seenStoreRevision) {
    _seenStoreRevision = storeRevision;
    _localMsg = "";
  }

  const int count = static_cast<int>(PRIORITIES_STORE.count());
  if (_sel > count - 1) _sel = count > 0 ? count - 1 : 0;

  // --- Header: title + companion status (same chrome as BlockScene) --------
  gfx.drawText(kFontBold, kMarginX, 8, "Priorities");
  // Transfer transients render as the paired up/down arrows; routine BLE
  // status ("Paired & encrypted", "Connected", advertising lines) stays OFF
  // the header — it read as noise. Connect problems surface in the empty
  // state and the About scene (same treatment as Block/Workout/Today).
  const std::string msg = COMPANION_BLE.getStatusMessage();
  SyncIndicator::draw(gfx, w - kMarginX, 8, gfx.lineHeight(kFontRegular), msg.c_str());
  gfx.fillRect(0, kHeaderH - 2, w, 2, true);

  // --- Empty state (PrioritiesActivity renderEmpty) -------------------------
  if (count == 0) {
    const int cy = h / 2;
    gfx.drawTextCentered(kFontBold, w / 2, cy - 2 * gfx.lineHeight(kFontBold), "Today's priorities");
    // No snapshot yet: an in-flight transient ("Requesting priorities...")
    // wins; otherwise "Syncing..." while the link is up (main.cpp's auto-resync
    // has re-requested and the phone is about to answer), else the connect hint.
    const char* line = _localMsg[0]                 ? _localMsg
                       : COMPANION_BLE.isConnected() ? "Syncing..."
                                                     : "Connect Companion to sync.";
    gfx.drawTextCentered(kFontRegular, w / 2, cy - gfx.lineHeight(kFontRegular) / 2, line);
    gfx.drawTextCentered(kFontSmall, w / 2, cy + gfx.lineHeight(kFontRegular) + 6, "Sync from Companion to refresh.");
    return;
  }

  // --- Sub-header: "Today" + progress, then the sync/message line ----------
  int active = 0, done = 0;
  PRIORITIES_STORE.tally(active, done);
  const int subY = kHeaderH + 10;
  gfx.drawText(kFontBold, kMarginX, subY, "Today");
  char progress[32];
  snprintf(progress, sizeof(progress), "%d active / %d done", active, done);
  gfx.drawText(kFontRegular, w - kMarginX - gfx.textWidth(kFontRegular, progress), subY, progress);

  const int syncY = subY + gfx.lineHeight(kFontBold) + 2;
  // Transients only ("Updating priority..."). No idle hint, and NOT the
  // store's syncLine — the card body is the same counts as the line above.
  if (_localMsg[0]) {
    char sync[96];
    truncateToWidth(gfx, kFontSmall, _localMsg, w - 2 * kMarginX - 96, sync, sizeof(sync));
    gfx.drawText(kFontSmall, kMarginX, syncY, sync);
  }

  // --- Rows (PrioritiesActivity renderList, X3 metrics) ---------------------
  const int rowH = gfx.lineHeight(kFontBold) + gfx.lineHeight(kFontSmall) + 22;
  const int listTop = syncY + gfx.lineHeight(kFontSmall) + 12;
  const int avail = h - Scene::SOFTKEY_BAR_H - 8 - listTop;
  int perPage = avail / rowH;
  if (perPage < 1) perPage = 1;
  _rowsPerPageCache = perPage;

  // Scroll window follows the selection.
  if (_sel < _scroll) _scroll = _sel;
  if (_sel >= _scroll + perPage) _scroll = _sel - perPage + 1;
  const int maxScroll = count > perPage ? count - perPage : 0;
  if (_scroll > maxScroll) _scroll = maxScroll;
  if (_scroll < 0) _scroll = 0;

  if (count > perPage) {  // range indicator, right-aligned on the sync line
    char range[24];
    const int last = _scroll + perPage < count ? _scroll + perPage : count;
    snprintf(range, sizeof(range), "%d-%d of %d", _scroll + 1, last, count);
    gfx.drawText(kFontSmall, w - kMarginX - gfx.textWidth(kFontSmall, range), syncY, range);
  }

  const int rowW = w - 2 * kMarginX;
  int y = listTop;
  for (int i = _scroll; i < count && i - _scroll < perPage; i++) {
    PrioritiesStore::Item item;
    if (!PRIORITIES_STORE.get(static_cast<std::size_t>(i), item)) break;
    const bool selected = i == _sel;
    const int cardH = rowH - 8;

    if (selected) {
      gfx.drawRoundedRect(kMarginX, y, rowW, cardH, 14, 3, true);
    } else {
      gfx.fillRect(kMarginX + 12, y + cardH + 3, rowW - 24, 1, true);  // separator
    }

    drawCheckbox(gfx, kMarginX + 14, y + (cardH - kCheckboxSize) / 2, item.done, selected ? 3 : 2);

    const int textX = kMarginX + 14 + kCheckboxSize + 16;
    const int textW = w - kMarginX - 14 - textX;
    const XpFont& titleFont = selected ? kFontBold : kFontRegular;
    // A row without a note is a single-line row: the title alone is centered
    // in the card, level with the checkbox — no placeholder text underneath.
    const char* noteSrc = noteIsReal(item.note) ? item.note : "";
    const int textBlockH = gfx.lineHeight(titleFont) + (noteSrc[0] ? gfx.lineHeight(kFontSmall) : 0);
    const int textTop = y + (cardH - textBlockH) / 2;
    char title[96];
    truncateToWidth(gfx, titleFont, item.title, textW, title, sizeof(title));
    gfx.drawText(titleFont, textX, textTop, title);

    if (noteSrc[0]) {
      char note[128];
      truncateToWidth(gfx, kFontSmall, noteSrc, textW, note, sizeof(note));
      gfx.drawText(kFontSmall, textX, textTop + gfx.lineHeight(titleFont), note);
    }
    y += rowH;
  }
}

// M4 dormant sleep frame — the priorities list held on glass all night.
// Static: reads the priorities store directly, shares the checkbox/truncation
// helpers with render(). Calm composition: centered title + rule, tallies,
// single-line rows (notes omitted — undone bold, done regular + ticked), and
// a bottom "moon + xphone" stamp with the wake hint. No RTC on X3/X4, so no
// "synced Xm ago" line.
bool PrioritiesScene::renderDormant(Gfx& gfx) {
  const int count = static_cast<int>(PRIORITIES_STORE.count());
  if (count == 0) return false;

  const int w = gfx.width();
  const int h = gfx.height();
  const int cx = w / 2;

  // Title + short rule (same chrome family as the wordmark sleep screen).
  const int titleY = 52;
  gfx.drawTextCentered(kFontBold, cx, titleY, "Today's priorities");
  constexpr int kRuleW = 56;
  gfx.fillRect(cx - kRuleW / 2, titleY + gfx.lineHeight(kFontBold) + 10, kRuleW, 2, true);

  // M5 sleep redesign v2: no progress line, no corner chrome — the header sits
  // high and the list runs tight so the bottom keeps room for a calendar line
  // + a block line (both centered) above the wake hint.
  // Rows: tighter default spacing, squeezing further when the list is long so
  // all 10 priorities fit (floor keeps the 32px checkbox breathing).
  const int listTop = 100;  // fixed — the title floats in the space above it
  const int stampTop = h - 176;  // bottom bound: calendar + block lines + hint
  const int avail = stampTop - listTop;
  int rowH = 48;  // checkbox 32 + breathing room
  if (count * rowH > avail) {
    rowH = avail / count;
    if (rowH < 38) rowH = 38;
  }
  int rows = avail / rowH;
  if (rows > count) rows = count;
  if (rows < 1) rows = 1;
  const bool overflow = rows < count;

  int y = listTop + (avail - rows * rowH - (overflow ? gfx.lineHeight(kFontSmall) + 6 : 0)) / 2;
  const int boxX = kMarginX + 28;
  const int textX = boxX + kCheckboxSize + 16;
  for (int i = 0; i < rows; i++) {
    PrioritiesStore::Item item;
    if (!PRIORITIES_STORE.get(static_cast<std::size_t>(i), item)) break;
    const XpFont& font = item.done ? kFontRegular : kFontBold;  // what's left leads
    drawCheckbox(gfx, boxX, y + (rowH - kCheckboxSize) / 2, item.done, 2);
    char title[96];
    truncateToWidth(gfx, font, item.title, w - textX - kMarginX, title, sizeof(title));
    gfx.drawText(font, textX, y + (rowH - gfx.lineHeight(font)) / 2, title);
    y += rowH;
  }
  if (overflow) {
    char more[24];
    snprintf(more, sizeof(more), "+%d more", count - rows);
    gfx.drawTextCentered(kFontSmall, cx, y + 4, more);
  }

  // Bottom stamp: a centered icon + label group, wake hint below (same
  // position as the plain sleep screen's hint). During an active block the
  // stamp becomes a padlock + "Block active until <time>" (falling back to a
  // minutes-left line when the phone omits the end-time label); otherwise it
  // stays the calm crescent + "xphone" wordmark.
  // Footer stack (centered): calendar line, block line beneath it, wake hint
  // last. Either line slides into the lower slot when the other is absent.
  int slotY = h - 108;
  const bool blockDrawn = renderDormantBlockLine(gfx, slotY);
  if (blockDrawn) slotY = h - 148;
  const bool calDrawn = renderDormantFooter(gfx, slotY);
  if (!blockDrawn && !calDrawn) {
    // Nothing to say: the calm crescent + wordmark.
    const int gap = 12;
    const char* kWordmark = "xphone";
    const int moonD = 22;
    const int gx = cx - (moonD + gap + gfx.textWidth(kFontBold, kWordmark)) / 2;
    drawMoon(gfx, gx, h - 108 + (gfx.lineHeight(kFontBold) - moonD) / 2, moonD);
    gfx.drawText(kFontBold, gx + moonD + gap, h - 108, kWordmark);
  }
  gfx.drawTextCentered(kFontSmall, cx, h - 56, "press power to wake");
  return true;
}

// Centered block line: lock + "until 5:30 PM (2 today)" while a block is
// active (count omitted at zero), or lock + "2 blocks today" after
// completions. Returns false when there is nothing to say.
bool PrioritiesScene::renderDormantBlockLine(Gfx& gfx, const int y) {
  const BlockStatusStore::Status block = BLOCK_STATUS.get();
  char label[48];
  if (block.active) {
    char when[24];
    if (block.endsAtLabel[0]) {
      snprintf(when, sizeof(when), "Until %s", block.endsAtLabel);
    } else {
      snprintf(when, sizeof(when), "%d min left", block.remainingMinutes);
    }
    snprintf(label, sizeof(label), "%s | Today: %d", when, block.blocksToday);
  } else if (block.blocksToday > 0) {
    snprintf(label, sizeof(label), "Today: %d", block.blocksToday);
  } else {
    return false;
  }
  constexpr int kLockD = 24;
  constexpr int kGap = 10;
  const int groupW = kLockD + kGap + gfx.textWidth(kFontRegular, label);
  const int gx = gfx.width() / 2 - groupW / 2;
  drawLockGlyph(gfx, gx, y + (gfx.lineHeight(kFontRegular) - kLockD) / 2, kLockD);
  gfx.drawText(kFontRegular, gx + kLockD + kGap, y, label);
  return true;
}

// Footer: calendar glyph + the next calendar event from the Today store
// ("8:00 PM: Go to the mall"; "Tomorrow 9:00 AM: Standup" when not today).
// The phone sends the rolling now->+24h agenda sorted by start, so the first
// non-reminder item IS the next event. Returns false when none is known.
bool PrioritiesScene::renderDormantFooter(Gfx& gfx, const int y) {
  // Next TIMED event only: skip reminders AND all-day entries (birthdays etc.
  // arrive with time "All day") — the footer answers "what's next on the
  // clock", and an all-day event has no clock.
  TodayStore::Item item;
  bool found = false;
  for (std::size_t i = 0; i < TODAY_STORE.count(); i++) {
    if (!TODAY_STORE.get(i, item)) continue;
    if (strcmp(item.kind, "reminder") == 0) continue;
    if (!item.time[0] || strcmp(item.time, "All day") == 0) continue;
    found = true;
    break;
  }
  if (!found || !item.title[0]) return false;

  // No day prefix: the Today producer only sends the rolling now->+24h window,
  // so "8:00 AM" is unambiguous — and the saved width goes to the event title.
  char label[128];
  snprintf(label, sizeof(label), "%s: %s", item.time, item.title);

  const int cx = gfx.width() / 2;
  constexpr int kCalD = 28;
  constexpr int kGap = 12;
  char clipped[112];
  truncateToWidth(gfx, kFontRegular, label, gfx.width() - 2 * kMarginX - kCalD - kGap, clipped, sizeof(clipped));
  const int groupW = kCalD + kGap + gfx.textWidth(kFontRegular, clipped);
  const int gx = cx - groupW / 2;
  drawCalendarGlyph(gfx, gx, y + (gfx.lineHeight(kFontRegular) - kCalD) / 2, kCalD);
  gfx.drawText(kFontRegular, gx + kCalD + kGap, y, clipped);
  return true;
}
