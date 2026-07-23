#include "TodayScene.h"

#include <cstdio>
#include <cstring>

#include "../Fonts.h"
#include "../TodayStore.h"
#include "../ble/CompanionBleService.h"
#include "AppScenes.h"

namespace {

constexpr int kMarginX = 20;
constexpr int kHeaderH = 46;  // same chrome as Notifications/Priorities/Block

constexpr int kIcon = 22;         // day-divider / section icon box (square)
constexpr int kIconGap = 12;      // icon -> label
constexpr int kCheck = 22;        // reminder checkbox
constexpr int kRemGutter = kCheck + 13;  // reminder title/time indent

// Header (day divider / reminders) spacing: generous pad above, an icon+label
// line vertically centered on the label. kIconVAdjust nudges the icon down onto
// the label's optical center (all-caps text sits high in its line box).
constexpr int kHeadTopPad = 24;
constexpr int kHeadLineH = 28;
constexpr int kIconVAdjust = 4;
constexpr int kDividerRowH = kHeadTopPad + kHeadLineH;
constexpr int kSectionRowH = kHeadTopPad + kHeadLineH;

// Item: time/due line (small) ABOVE the bold title, with breathing room under.
constexpr int kTimeLineH = 22;
constexpr int kTitleLineH = 30;
constexpr int kItemBotPad = 18;
constexpr int kItemRowH = kTimeLineH + kTitleLineH + kItemBotPad;

// Width-clipping copy (same helper as NotificationsScene).
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

// --- 1-bit day/section glyphs, drawn from primitives (top-left x,y, box d) ----

// Crescent: a full disc with a second (paper) disc carved out, offset right/up.
void drawMoon(Gfx& g, int x, int y, int d) {
  g.fillRoundedRect(x, y, d, d, d / 2, true);
  g.fillRoundedRect(x + (d * 2) / 5, y - d / 6, d, d, d / 2, false);
}

// Sun: a small filled disc with four orthogonal rays and four diagonal nubs.
void drawSun(Gfx& g, int x, int y, int d) {
  const int cx = x + d / 2, cy = y + d / 2;
  const int r = d / 5;              // center disc radius
  g.fillRoundedRect(cx - r, cy - r, 2 * r, 2 * r, r, true);
  const int r1 = r + 3, r2 = d / 2;  // ray span
  g.fillRect(cx - 1, cy - r2, 2, r2 - r1, true);   // up
  g.fillRect(cx - 1, cy + r1, 2, r2 - r1, true);   // down
  g.fillRect(cx - r2, cy - 1, r2 - r1, 2, true);   // left
  g.fillRect(cx + r1, cy - 1, r2 - r1, 2, true);   // right
  const int dd = (r1 * 7) / 10;                    // diagonal offset ~ r1/sqrt2
  g.fillRect(cx - dd - 1, cy - dd - 1, 3, 3, true);
  g.fillRect(cx + dd - 1, cy - dd - 1, 3, 3, true);
  g.fillRect(cx - dd - 1, cy + dd - 1, 3, 3, true);
  g.fillRect(cx + dd - 1, cy + dd - 1, 3, 3, true);
}

// Empty rounded checkbox (todo affordance for individual reminders).
void drawCheckbox(Gfx& g, int x, int y, int s) {
  g.drawRoundedRect(x, y, s, s, 3, 2, true);
}

// Bell (Reminders section header) — a filled dome over a flared body with a
// rim, clapper and top nub. Filled to match the moon; not a checkbox (which
// reads as an actionable todo, confusing on a section header).
void drawBell(Gfx& g, int x, int y, int d) {
  const int cx = x + d / 2;
  const int r = (d * 7) / 20;             // dome radius (~0.35 d)
  const int domeCy = y + d / 2 - 1;
  g.fillRoundedRect(cx - r, domeCy - r, 2 * r, 2 * r, r, true);   // dome
  const int baseW = (d * 4) / 5, baseY = domeCy, baseH = (d * 3) / 10;
  g.fillRect(cx - baseW / 2, baseY, baseW, baseH, true);          // flared body
  g.fillRect(cx - baseW / 2 - 2, baseY + baseH, baseW + 4, 2, true);  // rim
  g.fillRect(cx - 2, baseY + baseH + 3, 4, 3, true);             // clapper
  g.fillRect(cx - 2, domeCy - r - 3, 4, 3, true);               // top nub
}

}  // namespace

// buildRows: events first, grouped by day bucket — a DayDivider is emitted each
// time the bucket (item.subtitle, e.g. "TONIGHT"/"TOMORROW", set by the iOS
// producer) changes. Reminders follow under one "Reminders" header. The phone
// owns the sort; we only insert the dividers/header.
int TodayScene::buildRows(Row (&rows)[MAX_ROWS]) const {
  int n = 0;
  TodayStore::Item item;
  char lastBucket[sizeof(item.subtitle)] = {0};
  bool haveBucket = false;

  const int count = static_cast<int>(TODAY_STORE.count());
  for (int i = 0; i < count && n < MAX_ROWS - 1; i++) {
    if (!TODAY_STORE.get(static_cast<std::size_t>(i), item)) continue;
    if (strcmp(item.kind, "reminder") == 0) continue;  // events only in this pass
    if (!haveBucket || strcmp(item.subtitle, lastBucket) != 0) {
      rows[n++] = Row{RowType::DayDivider, static_cast<int8_t>(i)};
      snprintf(lastBucket, sizeof(lastBucket), "%s", item.subtitle);
      haveBucket = true;
    }
    rows[n++] = Row{RowType::Item, static_cast<int8_t>(i)};
  }

  bool remHeader = false;
  for (int i = 0; i < count && n < MAX_ROWS; i++) {
    if (!TODAY_STORE.get(static_cast<std::size_t>(i), item)) continue;
    if (strcmp(item.kind, "reminder") != 0) continue;
    if (!remHeader) {
      if (n >= MAX_ROWS - 1) break;
      rows[n++] = Row{RowType::SectionReminders, -1};
      remHeader = true;
    }
    rows[n++] = Row{RowType::Item, static_cast<int8_t>(i)};
  }
  return n;
}

void TodayScene::onEnter() {
  _scroll = 0;
  requestSync();
}

void TodayScene::requestSync() {
  if (COMPANION_BLE.isConnected() && COMPANION_BLE.sendTodaySyncRequest()) {
    _localMsg = "Requesting today...";
  } else {
    _localMsg = "";
  }
}

const char* const* TodayScene::softKeys() const {
  static constexpr const char* kFull[4] = {"BACK", "SYNC", "UP", "DOWN"};
  static constexpr const char* kEmpty[4] = {"BACK", "SYNC", nullptr, nullptr};
  return TODAY_STORE.hasSnapshot() ? kFull : kEmpty;
}

XpRect TodayScene::contentRect() const {
  if (_hCache <= 0) return XpRect{};
  return XpRect{0, kHeaderH, _wCache, static_cast<int16_t>(_hCache - kHeaderH - Scene::SOFTKEY_BAR_H)};
}

void TodayScene::handleInput(Input& in) {
  if (in.wasPressed(Btn::Back)) {
    showLauncher();
    return;
  }
  if (in.wasPressed(Btn::Confirm)) {  // SYNC: re-request the snapshot
    requestSync();
    markDirty();
    return;
  }
  if ((in.wasPressed(Btn::Up) || in.wasPressed(Btn::Left)) && _scroll > 0) {
    _scroll--;
    markDirty(contentRect());
  }
  if ((in.wasPressed(Btn::Down) || in.wasPressed(Btn::Right)) && _scroll < _maxScrollCache) {
    _scroll++;
    markDirty(contentRect());
  }
}

void TodayScene::render(Gfx& gfx) {
  const int w = gfx.width();
  const int h = gfx.height();
  _wCache = static_cast<int16_t>(w);
  _hCache = static_cast<int16_t>(h);

  const uint32_t storeRevision = TODAY_STORE.revision();
  if (storeRevision != _seenStoreRevision) {
    _seenStoreRevision = storeRevision;
    _localMsg = "";
  }

  // --- Header: title + sync line (or companion status) ----------------------
  gfx.drawText(kFontBold, kMarginX, 8, "Today");
  {
    // A pending sync shows "syncing..." at the top over whatever cached snapshot
    // is on glass (seeded from NVS on wake); it clears to the "Synced HH:MM"
    // line once the fresh card lands (revision bump clears _localMsg). No BLE
    // status fallback — routine rail text ("Paired & encrypted") is noise here.
    const char* detailSrc = _localMsg[0]                  ? "syncing..."
                            : TODAY_STORE.syncLine()[0]   ? TODAY_STORE.syncLine()
                                                          : nullptr;
    if (detailSrc) {
      char detail[96];
      const int detailMax = w - 2 * kMarginX - gfx.textWidth(kFontBold, "Today") - 12;
      truncateToWidth(gfx, kFontRegular, detailSrc, detailMax, detail, sizeof(detail));
      gfx.drawText(kFontRegular, w - kMarginX - gfx.textWidth(kFontRegular, detail), 8, detail);
    }
  }
  gfx.fillRect(0, kHeaderH - 2, w, 2, true);

  // --- Empty state -----------------------------------------------------------
  Row rows[MAX_ROWS];
  const int rowCount = TODAY_STORE.hasSnapshot() ? buildRows(rows) : 0;
  if (rowCount == 0) {
    _scroll = 0;
    _maxScrollCache = 0;
    const int cy = h / 2;
    gfx.drawTextCentered(kFontBold, w / 2, cy - 2 * gfx.lineHeight(kFontBold), "Sync Today");
    const char* line = _localMsg[0]                 ? _localMsg
                       : COMPANION_BLE.isConnected() ? "Syncing..."
                                                     : "Connect Companion to sync.";
    gfx.drawTextCentered(kFontRegular, w / 2, cy - gfx.lineHeight(kFontRegular) / 2, line);
    gfx.drawTextCentered(kFontRegular, w / 2, cy + gfx.lineHeight(kFontRegular) + 6,
                         "Calendar and reminders appear here.");
    return;
  }

  // --- Scroll clamp + overflow bookkeeping -----------------------------------
  const int top = kHeaderH + 6;
  const int bottom = h - Scene::SOFTKEY_BAR_H - 6;
  auto rowH = [](const Row& r) {
    switch (r.type) {
      case RowType::DayDivider: return kDividerRowH;
      case RowType::SectionReminders: return kSectionRowH;
      case RowType::Item:
      default: return kItemRowH;
    }
  };
  int maxScroll = 0;
  {
    int used = 0;
    int first = rowCount - 1;
    for (int i = rowCount - 1; i >= 0; i--) {
      used += rowH(rows[i]);
      if (used > bottom - top) break;
      first = i;
    }
    maxScroll = first;
  }
  if (_scroll > maxScroll) _scroll = maxScroll;
  if (_scroll < 0) _scroll = 0;
  _maxScrollCache = maxScroll;

  const int textW = w - 2 * kMarginX;
  const int smallH = gfx.lineHeight(kFontSmall);
  int y = top;
  TodayStore::Item item;
  for (int i = _scroll; i < rowCount; i++) {
    const Row& r = rows[i];
    if (y + rowH(r) > bottom) break;
    switch (r.type) {
      case RowType::DayDivider: {
        if (!TODAY_STORE.get(static_cast<std::size_t>(r.item), item)) break;
        const char* label = item.subtitle[0] ? item.subtitle : "TODAY";
        const int labelTop = y + kHeadTopPad;
        const int iconY = labelTop + kIconVAdjust;
        if (strcmp(label, "TONIGHT") == 0) {
          drawMoon(gfx, kMarginX, iconY, kIcon);
        } else {
          drawSun(gfx, kMarginX, iconY, kIcon);
        }
        gfx.drawText(kFontBold, kMarginX + kIcon + kIconGap, labelTop, label);
        break;
      }
      case RowType::SectionReminders: {
        const int labelTop = y + kHeadTopPad;
        drawBell(gfx, kMarginX, labelTop + kIconVAdjust, kIcon);
        gfx.drawText(kFontBold, kMarginX + kIcon + kIconGap, labelTop, "REMINDERS");
        break;
      }
      case RowType::Item:
      default: {
        if (!TODAY_STORE.get(static_cast<std::size_t>(r.item), item)) break;
        const bool reminder = strcmp(item.kind, "reminder") == 0;
        const int textX = reminder ? kMarginX + kRemGutter : kMarginX;
        const int lineW = reminder ? textW - kRemGutter : textW;
        // Checkbox on individual reminders, vertically nudged onto the block.
        if (reminder) drawCheckbox(gfx, kMarginX, y + 6, kCheck);

        // Line 1: time (event) or due label (reminder) — small/light, above.
        if (item.time[0]) {
          char t[56];
          truncateToWidth(gfx, kFontSmall, item.time, lineW, t, sizeof(t));
          gfx.drawText(kFontSmall, textX, y, t);
        }
        // Line 2: title — bold, near full width, beneath the time.
        char title[112];
        truncateToWidth(gfx, kFontBold, item.title[0] ? item.title : "Untitled", lineW, title, sizeof(title));
        gfx.drawText(kFontBold, textX, y + kTimeLineH, title);
        (void)smallH;
        break;
      }
    }
    y += rowH(r);
  }

  // Transient sync message, bottom-left above the soft keys.
  if (_localMsg[0] && y <= bottom - gfx.lineHeight(kFontRegular)) {
    char msg[96];
    truncateToWidth(gfx, kFontRegular, _localMsg, textW, msg, sizeof(msg));
    gfx.drawText(kFontRegular, kMarginX, bottom - gfx.lineHeight(kFontRegular), msg);
  }
}
