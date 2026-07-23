#include "WorkoutScene.h"

#include <Arduino.h>

#include <cstdio>
#include <cstring>

#include "../Fonts.h"
#include "../SyncIndicator.h"
#include "../WorkoutStore.h"
#include "../ble/CompanionBleService.h"
#include "AppScenes.h"

namespace {

constexpr int kMarginX = 20;
constexpr int kHeaderH = 46;  // same chrome as PrioritiesScene
constexpr int kCheckboxSize = 32;
constexpr int kMaxPips = 12;  // sets <= 12 draw a pip bar; beyond that, numeric only

// Coalesced workout.set send: five rapid + presses become one packet carrying
// the final absolute count. File-scope (not members) so Sleep.cpp can flush
// through the static hook without a scene pointer. Main-loop only.
int s_pendingIdx = -1;
uint32_t s_pendingDueMs = 0;
constexpr uint32_t kSendQuietMs = 400;

void sendPendingNow() {
  if (s_pendingIdx < 0) return;
  WorkoutStore::Item item;
  if (WORKOUT_STORE.get(static_cast<std::size_t>(s_pendingIdx), item) && item.id[0] != '\0') {
    COMPANION_BLE.sendWorkoutSet(item.id, item.done);  // absolute — idempotent
  }
  s_pendingIdx = -1;
}

// Width-clipping copy (same helper as PrioritiesScene).
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

// Same rounded checkbox as PrioritiesScene.
void drawCheckbox(Gfx& gfx, const int x, const int y, const bool checked, const int thickness) {
  gfx.drawRoundedRect(x, y, kCheckboxSize, kCheckboxSize, 8, thickness, true);
  if (!checked) return;
  gfx.drawLine(x + 8, y + 16, x + 13, y + 22, 3, true);
  gfx.drawLine(x + 13, y + 22, x + 24, y + 10, 3, true);
}

// Set-progress pip bar: `done` filled squares then hollow ones, 12x12 px on
// a 18 px pitch. Only drawn when sets <= kMaxPips (numeric covers the rest).
void drawPips(Gfx& gfx, const int x, const int y, const int sets, const int done) {
  for (int i = 0; i < sets; i++) {
    const int px = x + i * 18;
    if (i < done) {
      gfx.fillRect(px, y, 12, 12, true);
    } else {
      gfx.drawRoundedRect(px, y, 12, 12, 2, 2, true);
    }
  }
}

// "3/10" progress fragment.
void formatCount(char* buf, size_t n, const int done, const int sets) {
  snprintf(buf, n, "%d/%d", done, sets);
}

}  // namespace

void WorkoutScene::onEnter() {
  _sel = 0;
  _scroll = 0;
  if (COMPANION_BLE.isConnected() && COMPANION_BLE.sendWorkoutSyncRequest()) {
    _localMsg = "Requesting workout...";
  } else {
    _localMsg = "";
  }
}

void WorkoutScene::onExit() {
  sendPendingNow();  // don't strand a coalesced update when leaving the scene
}

void WorkoutScene::flushPendingSend() { sendPendingNow(); }

const char* const* WorkoutScene::softKeys() const {
  static constexpr const char* kList[4] = {"BACK", "+SET", "UP", "DOWN"};
  static constexpr const char* kEmpty[4] = {"BACK", "SYNC", nullptr, nullptr};
  return WORKOUT_STORE.count() > 0 ? kList : kEmpty;
}

XpRect WorkoutScene::listRect() const {
  if (_hCache <= 0) return XpRect{};
  return XpRect{0, kHeaderH, _wCache, static_cast<int16_t>(_hCache - kHeaderH - Scene::SOFTKEY_BAR_H)};
}

void WorkoutScene::bumpSelected(const int delta) {
  const int next = WORKOUT_STORE.bumpDone(static_cast<std::size_t>(_sel), delta);
  if (next < 0) return;  // clamp edge or bad index — nothing changed
  // Different exercise pending? Flush it first so its count isn't lost.
  if (s_pendingIdx >= 0 && s_pendingIdx != _sel) sendPendingNow();
  s_pendingIdx = _sel;
  s_pendingDueMs = millis() + kSendQuietMs;
  _localMsg = "";
  markDirty(listRect());
}

void WorkoutScene::handleInput(Input& in) {
  // Quiet-window expiry for the coalesced send (runs on the input tick).
  if (s_pendingIdx >= 0 && static_cast<int32_t>(millis() - s_pendingDueMs) >= 0) {
    sendPendingNow();
  }

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

  const int count = static_cast<int>(WORKOUT_STORE.count());

  if (in.wasPressed(Btn::Confirm)) {
    if (count > 0) {
      bumpSelected(+1);  // +SET: the big center button counts a set too
    } else {
      _localMsg = COMPANION_BLE.sendWorkoutSyncRequest() ? "Requesting workout..." : "";
      markDirty();
    }
    return;
  }

  // Edge pair: top-left = -1 set, top-right = +1 set (the workout gesture).
  if (in.wasPressed(Btn::Up) && count > 0) {
    bumpSelected(-1);
    return;
  }
  if (in.wasPressed(Btn::Down) && count > 0) {
    bumpSelected(+1);
    return;
  }

  // Front Left/Right (under the UP/DOWN soft-key tabs): move the selection.
  if (in.wasPressed(Btn::Left) && _sel > 0) {
    _sel--;
    markDirty(listRect());
  }
  if (in.wasPressed(Btn::Right) && _sel < count - 1) {
    _sel++;
    markDirty(listRect());
  }
}

void WorkoutScene::render(Gfx& gfx) {
  const int w = gfx.width();
  const int h = gfx.height();
  _wCache = static_cast<int16_t>(w);
  _hCache = static_cast<int16_t>(h);

  // A fresh snapshot answers any in-flight sync — clear the transient line
  // (without this, onEnter's "Requesting workout..." sat forever).
  const uint32_t storeRevision = WORKOUT_STORE.revision();
  if (storeRevision != _seenStoreRevision) {
    _seenStoreRevision = storeRevision;
    _localMsg = "";
  }

  const int count = static_cast<int>(WORKOUT_STORE.count());
  if (_sel > count - 1) _sel = count > 0 ? count - 1 : 0;

  // --- Header ---------------------------------------------------------------
  gfx.drawText(kFontBold, kMarginX, 8, "Workout");
  // Transfer transients render as the paired up/down arrows; routine BLE
  // status stays OFF the header (same treatment as Block/Priorities/Today).
  const std::string msg = COMPANION_BLE.getStatusMessage();
  SyncIndicator::draw(gfx, w - kMarginX, 8, gfx.lineHeight(kFontRegular), msg.c_str());
  gfx.fillRect(0, kHeaderH - 2, w, 2, true);

  // --- Empty state ------------------------------------------------------------
  if (count == 0) {
    const int cy = h / 2;
    gfx.drawTextCentered(kFontBold, w / 2, cy - 2 * gfx.lineHeight(kFontBold), "Today's workout");
    const char* line = _localMsg[0]                  ? _localMsg
                       : COMPANION_BLE.isConnected() ? "Syncing..."
                                                     : "Connect Companion to sync.";
    gfx.drawTextCentered(kFontRegular, w / 2, cy - gfx.lineHeight(kFontRegular) / 2, line);
    gfx.drawTextCentered(kFontSmall, w / 2, cy + gfx.lineHeight(kFontRegular) + 6,
                         "Set today's workout in the companion app.");
    return;
  }

  // --- Sub-header: progress + hint -------------------------------------------
  int doneEx = 0, total = 0;
  WORKOUT_STORE.tally(doneEx, total);
  const int subY = kHeaderH + 10;
  const bool complete = doneEx == total;
  gfx.drawText(kFontBold, kMarginX, subY, complete ? "Workout complete!" : "Today");
  char progress[32];
  snprintf(progress, sizeof(progress), "%d / %d done", doneEx, total);
  gfx.drawText(kFontRegular, w - kMarginX - gfx.textWidth(kFontRegular, progress), subY, progress);

  const int syncY = subY + gfx.lineHeight(kFontBold) + 2;
  // Transients win; otherwise the set-counting instruction (the edge buttons
  // are not discoverable without it).
  const char* syncLine = _localMsg[0] ? _localMsg : "Press left (-) and right (+) to count sets.";
  char sync[96];
  truncateToWidth(gfx, kFontSmall, syncLine, w - 2 * kMarginX - 96, sync, sizeof(sync));
  gfx.drawText(kFontSmall, kMarginX, syncY, sync);

  // --- Rows -------------------------------------------------------------------
  const int rowH = gfx.lineHeight(kFontBold) + gfx.lineHeight(kFontSmall) + 22;
  const int listTop = syncY + gfx.lineHeight(kFontSmall) + 12;
  const int avail = h - Scene::SOFTKEY_BAR_H - 8 - listTop;
  int perPage = avail / rowH;
  if (perPage < 1) perPage = 1;

  if (_sel < _scroll) _scroll = _sel;
  if (_sel >= _scroll + perPage) _scroll = _sel - perPage + 1;
  const int maxScroll = count > perPage ? count - perPage : 0;
  if (_scroll > maxScroll) _scroll = maxScroll;
  if (_scroll < 0) _scroll = 0;

  if (count > perPage) {
    char range[24];
    const int last = _scroll + perPage < count ? _scroll + perPage : count;
    snprintf(range, sizeof(range), "%d-%d of %d", _scroll + 1, last, count);
    gfx.drawText(kFontSmall, w - kMarginX - gfx.textWidth(kFontSmall, range), syncY, range);
  }

  const int rowW = w - 2 * kMarginX;
  int y = listTop;
  for (int i = _scroll; i < count && i - _scroll < perPage; i++) {
    WorkoutStore::Item item;
    if (!WORKOUT_STORE.get(static_cast<std::size_t>(i), item)) break;
    const bool selected = i == _sel;
    const bool exDone = item.sets > 0 && item.done >= item.sets;
    const int cardH = rowH - 8;

    if (selected) {
      gfx.drawRoundedRect(kMarginX, y, rowW, cardH, 14, 3, true);
    } else {
      gfx.fillRect(kMarginX + 12, y + cardH + 3, rowW - 24, 1, true);
    }

    drawCheckbox(gfx, kMarginX + 14, y + (cardH - kCheckboxSize) / 2, exDone, selected ? 3 : 2);

    const int textX = kMarginX + 14 + kCheckboxSize + 16;
    // Reserve the right side for the "3/10" counter.
    char countStr[16];
    formatCount(countStr, sizeof(countStr), item.done, item.sets);
    const XpFont& countFont = selected ? kFontBold : kFontRegular;
    const int countW = gfx.textWidth(countFont, countStr);
    const int countX = w - kMarginX - 14 - countW;
    const int textW = countX - 12 - textX;

    const XpFont& titleFont = selected ? kFontBold : kFontRegular;
    const int textTop = y + (cardH - gfx.lineHeight(titleFont) - gfx.lineHeight(kFontSmall)) / 2;
    char title[96];
    truncateToWidth(gfx, titleFont, item.name, textW, title, sizeof(title));
    gfx.drawText(titleFont, textX, textTop, title);

    gfx.drawText(countFont, countX, textTop, countStr);

    // Second line: pip bar for small set counts, "N sets" caption otherwise.
    const int line2Y = textTop + gfx.lineHeight(titleFont);
    if (item.sets > 0 && item.sets <= kMaxPips) {
      drawPips(gfx, textX, line2Y + (gfx.lineHeight(kFontSmall) - 12) / 2, item.sets, item.done);
    } else {
      char caption[32];
      snprintf(caption, sizeof(caption), exDone ? "done" : "%d sets", item.sets);
      gfx.drawText(kFontSmall, textX, line2Y, caption);
    }
    y += rowH;
  }
}

// --- Dormant sleep face --------------------------------------------------------

bool WorkoutScene::renderDormant(Gfx& gfx) {
  const int count = static_cast<int>(WORKOUT_STORE.count());
  if (count == 0) return false;

  const int w = gfx.width();
  const int h = gfx.height();
  const int cx = w / 2;

  // EXACTLY the priorities sleep face's chrome and rhythm — centered title +
  // short rule, fixed listTop, rows squeezing when long, bottom kept clear for
  // the footer stack Sleep.cpp draws — only the words and the per-row content
  // (name + set count) differ. The sleep screen is ONE design with swappable
  // list content, not the workout app screen.
  const int titleY = 52;
  gfx.drawTextCentered(kFontBold, cx, titleY, "Today's workout");
  constexpr int kRuleW = 56;
  gfx.fillRect(cx - kRuleW / 2, titleY + gfx.lineHeight(kFontBold) + 10, kRuleW, 2, true);

  const int listTop = 100;           // fixed — the title floats above it
  const int stampTop = h - 176;      // bottom bound: calendar + block + hint
  const int avail = stampTop - listTop;
  int rowH = 48;                     // checkbox 32 + breathing room
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
    WorkoutStore::Item item;
    if (!WORKOUT_STORE.get(static_cast<std::size_t>(i), item)) break;
    const bool exDone = item.sets > 0 && item.done >= item.sets;
    const XpFont& font = exDone ? kFontRegular : kFontBold;  // what's left leads
    drawCheckbox(gfx, boxX, y + (rowH - kCheckboxSize) / 2, exDone, 2);

    char countStr[16];
    formatCount(countStr, sizeof(countStr), item.done, item.sets);
    const int countW = gfx.textWidth(font, countStr);
    const int countX = w - kMarginX - 28 - countW;

    char title[96];
    truncateToWidth(gfx, font, item.name, countX - 12 - textX, title, sizeof(title));
    const int textY = y + (rowH - gfx.lineHeight(font)) / 2;
    gfx.drawText(font, textX, textY, title);
    gfx.drawText(font, countX, textY, countStr);
    y += rowH;
  }
  if (overflow) {
    char more[24];
    snprintf(more, sizeof(more), "+%d more", count - rows);
    gfx.drawTextCentered(kFontSmall, cx, y + 4, more);
  }
  return true;
}
