#include "BlockScene.h"

#include <cstdio>
#include <cstring>

#include "../BlockStatusStore.h"
#include "../Fonts.h"
#include "../SyncIndicator.h"
#include "../art/BlockArtwork.h"
#include "../ble/CompanionBleService.h"
#include "AppScenes.h"

namespace {

// CrossPoint's preset table (BlockActivity.cpp blockModes) — the preset ids
// ride the wire as presetId/preset, so they must stay identical for
// BlockManager.swift's preset lookup. Target-count subtitles are gone: the
// phone now blocks all apps except a kept allowlist, so per-preset target
// counts no longer exist.
struct BlockMode {
  const char* id;
  const char* title;
  int minutes;
};
constexpr BlockMode kModes[BlockScene::MODE_COUNT] = {
    {"deep_work", "Deep Work", 30},
    {"reading", "Reading", 45},
    {"evening", "Evening", 60},
    {"workout", "Workout", 60},
};

// CrossPoint's break chooser verbatim (BlockActivity.cpp breakChoices).
struct BreakChoice {
  const char* title;
  const char* subtitle;
};
constexpr BreakChoice kBreaks[BlockScene::BREAK_COUNT] = {
    {"Keep blocking", "Return to countdown"},
    {"5 min break", "Short reset"},
    {"Stop now", "End block"},
};

// CrossPoint's duration bounds/step (BlockActivity.cpp:39-41). CrossPoint has
// a single step tier — ±10 min per tap, no hold tier — so taps match exactly
// and long-press adds nothing.
constexpr int kMinBlockMinutes = 10;
constexpr int kMaxBlockMinutes = 180;
constexpr int kBlockMinuteStep = 10;

// Layout (logical portrait; X3 528x792, X4 480x800).
constexpr int kMarginX = 20;
constexpr int kHeaderH = 46;  // same chrome as NotificationsScene
// −/+ side tabs: soft-key tab styling (thin border, rounded), anchored to the
// LEFT/RIGHT screen edges in the upper third so they read as extensions of
// the two TOP-edge physical buttons. Drawn 1 radius past the edge so
// drawPixel clips the outer side square — only the inner corners stay
// rounded, exactly the soft-key bar trick (Scene.cpp drawSoftKeyBar).
constexpr int kSideTabW = 26;   // visible width
constexpr int kSideTabH = 141;  // +10% over 128: ~3px added up, ~10px down
constexpr int kSideTabY = 119;
constexpr int kSideTabRadius = 10;
constexpr int kHeroTopY = 234;  // active-view padlock top
constexpr int kTitleY = 70;     // active-view title under the header

// Transient commands resolve from the phone's pushed block-status card; this
// timeout falls through to the optimistic state if that card never lands.
constexpr uint32_t kTransientTimeoutMs = 4000;

int clampInt(const int v, const int lo, const int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Status helpers. The legacy-card heuristics (state strings, "min left"
// title parsing — BlockActivity.cpp:43-153) live in
// BlockStatusStore::updateFromCard now; the scene reads the ~64 B fixed
// snapshot instead of copying the 3.6 KB generic card per repaint.
bool isBlockCard(const BlockStatusStore::Status& st) { return st.fromCard; }

bool isActiveBlockCard(const BlockStatusStore::Status& st) { return st.fromCard && st.active; }

bool isBreakCard(const BlockStatusStore::Status& st) { return st.fromCard && st.onBreak; }

const char* presetTitle(const BlockStatusStore::Status& st, const char* fallback) {
  return st.preset[0] ? st.preset : fallback;
}

int cardDuration(const BlockStatusStore::Status& st, const int fallback) {
  return st.durationMinutes > 0 ? st.durationMinutes : fallback;
}

int cardRemaining(const BlockStatusStore::Status& st) { return st.remainingMinutes; }

// Width-clipping copy (same helper as NotificationsScene) for the header
// status line, which comes from the BLE service as a free-form message.
void truncateToWidth(Gfx& gfx, const XpFont& font, const char* src, int maxWidth, char* dst, size_t dstSize) {
  snprintf(dst, dstSize, "%s", src ? src : "");
  if (gfx.textWidth(font, dst) <= maxWidth) return;
  size_t len = strlen(dst);
  while (len > 0) {
    do {
      len--;
    } while (len > 0 && (static_cast<uint8_t>(dst[len]) & 0xC0) == 0x80);
    dst[len] = '\0';
    char probe[96];
    snprintf(probe, sizeof(probe), "%s...", dst);
    if (gfx.textWidth(font, probe) <= maxWidth) {
      snprintf(dst, dstSize, "%s", probe);
      return;
    }
  }
}

// 1bpp blitter for the generated artwork (format per BlockArtwork.h header;
// same loop as BlockActivity.cpp drawArtwork:73 — a cleared bit is ink, only
// ink pixels are drawn so the paper stays the framebuffer's white).
void drawArtwork(Gfx& gfx, const uint8_t* bitmap, const int x, const int y, const int width, const int height) {
  const int rowBytes = (width + 7) / 8;
  for (int row = 0; row < height; ++row) {
    for (int col = 0; col < width; ++col) {
      const uint8_t byte = bitmap[row * rowBytes + (col >> 3)];
      if (((byte >> (7 - (col & 7))) & 1) == 0) gfx.drawPixel(x + col, y + row, true);
    }
  }
}

// Per-preset 48x48 icons, CrossPoint's iconForMode (BlockActivity.cpp:121):
// focus target / open book / moon; workout has no bitmap — CrossPoint draws a
// dumbbell from diagonal lines (drawDumbbellIcon, BlockActivity.cpp:107).
const uint8_t* iconForMode(const int index) {
  switch (index) {
    case 0:
      return BlockIconFocusTarget;
    case 1:
      return BlockIconOpenBook;
    case 2:
      return BlockIconMoon;
    default:
      return nullptr;
  }
}

// Axis-aligned dumbbell for the workout preset (Gfx has no diagonal lines, so
// CrossPoint's slanted version is squared up: bar + two plates per side).
void drawDumbbellIcon(Gfx& gfx, const int x, const int y) {
  gfx.fillRect(x + 12, y + 21, 24, 6, true);  // bar
  gfx.fillRect(x + 4, y + 15, 6, 18, true);   // outer plates
  gfx.fillRect(x + 38, y + 15, 6, 18, true);
  gfx.fillRect(x + 12, y + 10, 6, 28, true);  // inner plates
  gfx.fillRect(x + 30, y + 10, 6, 28, true);
}

void drawModeIcon(Gfx& gfx, const int modeIndex, const int x, const int y) {
  const uint8_t* icon = iconForMode(modeIndex);
  if (icon) {
    drawArtwork(gfx, icon, x, y, BlockIconWidth, BlockIconHeight);
  } else {
    drawDumbbellIcon(gfx, x, y);
  }
}

}  // namespace

void BlockScene::onEnter() {
  _view = View::Main;
  // Same entry behavior as BlockActivity::onEnter (minus radio bring-up —
  // main.cpp owns BLE begin/advertising): ask the phone for a fresh Block
  // status so the card is not stale from a previous session.
  if (COMPANION_BLE.isConnected()) {
    _localMsg = COMPANION_BLE.sendBlockStatus() ? "Requesting Block status..." : "Connect Companion to sync.";
  } else {
    _localMsg = "Connect Companion to sync.";
  }
  _zeroConfirmSent = false;
}

const char* const* BlockScene::softKeys() const {
  static constexpr const char* kMainReady[4] = {"BACK", "START", "MODE", nullptr};
  // Active: no direct STOP — stopping goes through the break chooser ("Stop
  // now"), exactly CrossPoint's flow. BREAK stays on the same physical
  // button (front-LEFT, slot 2) the user already knows as MODE when idle.
  static constexpr const char* kMainActive[4] = {"BACK", nullptr, "BREAK", nullptr};
  static constexpr const char* kList[4] = {"BACK", "SELECT", SoftKey::Left, SoftKey::Right};
  if (_view == View::Main) return _activeCache ? kMainActive : kMainReady;
  return kList;
}

void BlockScene::adjustDuration(const int delta) {
  const int next = clampInt(_durationMin + delta, kMinBlockMinutes, kMaxBlockMinutes);
  if (next == _durationMin) return;
  _durationMin = next;
  _durationCustomized = true;
  markDirty();
}

void BlockScene::setPending(const Pending p) {
  _pending = p;
  _pendingAtMs = millis();
}

// Local countdown anchor. Called with the phone's remainingMinutes on every
// fresh active card (phone authoritative) or with the selected duration on an
// optimistic start.
void BlockScene::latchEnd(const int minutes) {
  _endMs = millis() + static_cast<uint32_t>(minutes) * 60000UL;
  _endValid = true;
  _zeroConfirmSent = false;
}

// Rollover-safe: unsigned subtraction reinterpreted as a signed window works
// across the 49.7-day millis() wrap. Remaining minutes round UP so the display
// matches the phone's ceil-style "N min left".
int BlockScene::remainingNowMin(const uint32_t now) const {
  const int32_t diffMs = static_cast<int32_t>(_endMs - now);
  if (diffMs <= 0) return 0;
  return static_cast<int>((diffMs + 59999L) / 60000L);
}

void BlockScene::startBlock() {
  const BlockMode& mode = kModes[clampInt(_modeSel, 0, MODE_COUNT - 1)];
  if (COMPANION_BLE.sendBlockStart(static_cast<uint16_t>(_durationMin), mode.id)) {
    _localMsg = "Starting selected mode...";
    setPending(Pending::Start);
  } else {
    _localMsg = "Connect Companion to start.";
  }
  markDirty();
}

void BlockScene::startDeepWork() {
  // deep_work is kModes[0]; use its default duration for a deterministic
  // quick-start regardless of any duration the user left set earlier.
  _view = View::Main;
  _modeSel = 0;
  _durationMin = kModes[0].minutes;
  _durationCustomized = false;
  startBlock();
}

void BlockScene::confirmBreakChoice() {
  if (_breakSel == 0) {
    _localMsg = "Still blocking.";
  } else if (_breakSel == 1) {
    if (COMPANION_BLE.sendBlockBreak(5)) {
      _localMsg = "Requesting 5 minute break...";
      setPending(Pending::Break);
    } else {
      _localMsg = "Connect Companion to pause.";
    }
  } else {
    if (COMPANION_BLE.sendBlockStop()) {
      _localMsg = "Stopping Block...";
      setPending(Pending::Stop);
    } else {
      _localMsg = "Connect Companion to stop.";
    }
  }
  _view = View::Main;
  markDirty();
}

// Per-tick (10 ms main loop) bookkeeping — runs only while this scene is on
// glass, from handleInput(). Three jobs:
//  1. Transient timeout: a pending command unconfirmed after ~4s falls
//     through to the optimistic state so the UI is never stuck.
//  2. Minute tick: mark dirty when the locally computed remaining-minutes
//     value changes (one FAST/PARTIAL repaint per minute, no BLE traffic).
//  3. Local zero: confirm with one block.status, then optimistically show
//     ready if the confirmation card doesn't land either.
void BlockScene::tickTransients(const uint32_t now) {
  if (_pending != Pending::None && static_cast<int32_t>(now - _pendingAtMs) >= static_cast<int32_t>(kTransientTimeoutMs)) {
    switch (_pending) {
      case Pending::Start:
        _optimisticActive = true;
        _optimisticReady = false;
        latchEnd(_durationMin);
        _localMsg = "Blocking (awaiting phone).";
        break;
      case Pending::Stop:
        _optimisticReady = true;
        _optimisticActive = false;
        _endValid = false;
        _localMsg = "Block stopped (awaiting phone).";
        break;
      case Pending::Break:
        _localMsg = "Break requested.";
        break;
      case Pending::None:
        break;
    }
    _pending = Pending::None;
    markDirty();
  }

  if (!_activeCache || !_endValid) return;

  const int remaining = remainingNowMin(now);
  if (remaining != _shownRemaining) markDirty();  // once per minute

  if (remaining > 0) return;
  if (!_zeroConfirmSent) {
    _zeroConfirmSent = true;
    _zeroConfirmAtMs = now;
    if (!COMPANION_BLE.sendBlockStatus()) _zeroConfirmAtMs = now - kTransientTimeoutMs;  // offline: no reply coming
  } else if (static_cast<int32_t>(now - _zeroConfirmAtMs) >= static_cast<int32_t>(kTransientTimeoutMs)) {
    // Phone never confirmed the natural end — show ready optimistically; the
    // next fresh card corrects us either way.
    _optimisticReady = true;
    _optimisticActive = false;
    _endValid = false;
    _zeroConfirmSent = false;
    _localMsg = "Block finished.";
    markDirty();
  }
}

void BlockScene::handleInput(Input& in) {
  tickTransients(millis());  // scene tick: timeouts + local minute countdown

  // Header transfer-arrow flash: bold on a new sent/received transient, back
  // to the thin idle pair ~1s later. Header-window repaint only.
  if (SyncIndicator::tick(COMPANION_BLE.getRevision(), millis(),
                          [] { return COMPANION_BLE.getStatusMessage(); })) {
    markDirty(XpRect{0, 0, _wCache, kHeaderH});
  }

  if (in.wasPressed(Btn::Back)) {
    if (_view != View::Main) {
      _view = View::Main;
      markDirty();
    } else {
      showLauncher();
    }
    return;
  }

  switch (_view) {
    case View::Main:
      if (in.wasPressed(Btn::Confirm)) {
        // Active: CONFIRM is unbound (soft key slot blank) — stopping goes
        // through the BREAK chooser's "Stop now", CrossPoint's flow.
        if (!_activeCache) startBlock();
        return;
      }
      if (in.wasPressed(Btn::Left)) {  // MODE (idle) / BREAK chooser (active)
        if (_activeCache) {
          _breakSel = 0;
          _view = View::Break;
        } else {
          _view = View::Modes;
        }
        markDirty();
        return;
      }
      // Duration: top-LEFT (Btn::Up) = −, top-RIGHT (Btn::Down) = + — the
      // −/+ edge tabs. Only while idle, as in CrossPoint (duration is the
      // phone's to report once a block runs).
      if (!_activeCache) {
        if (in.wasPressed(Btn::Up)) adjustDuration(-kBlockMinuteStep);
        if (in.wasPressed(Btn::Down)) adjustDuration(+kBlockMinuteStep);
      }
      break;

    case View::Modes:
      if (in.wasPressed(Btn::Confirm)) {  // SELECT: adopt preset, back to main
        _durationMin = kModes[clampInt(_modeSel, 0, MODE_COUNT - 1)].minutes;
        _durationCustomized = false;
        _view = View::Main;
        markDirty();
        return;
      }
      // Front LEFT = selection UP, front RIGHT = selection DOWN; the
      // top-edge pair mirrors the same axis.
      if ((in.wasPressed(Btn::Left) || in.wasPressed(Btn::Up)) && _modeSel > 0) {
        _modeSel--;
        markDirty();
      }
      if ((in.wasPressed(Btn::Right) || in.wasPressed(Btn::Down)) && _modeSel < MODE_COUNT - 1) {
        _modeSel++;
        markDirty();
      }
      break;

    case View::Break:
      if (in.wasPressed(Btn::Confirm)) {
        confirmBreakChoice();
        return;
      }
      if ((in.wasPressed(Btn::Left) || in.wasPressed(Btn::Up)) && _breakSel > 0) {
        _breakSel--;
        markDirty();
      }
      if ((in.wasPressed(Btn::Right) || in.wasPressed(Btn::Down)) && _breakSel < BREAK_COUNT - 1) {
        _breakSel++;
        markDirty();
      }
      break;
  }
}

void BlockScene::renderHeader(Gfx& gfx) const {
  const int w = gfx.width();
  gfx.drawText(kFontBold, kMarginX, 8, "Block");
  // Transfer transients (command sent / card received) render as the paired
  // up/down arrows; routine BLE status stays OFF the header (same treatment
  // as Priorities/Workout/Today) — the scene's own message line carries
  // connect hints.
  const std::string msg = COMPANION_BLE.getStatusMessage();
  SyncIndicator::draw(gfx, w - kMarginX, 8, gfx.lineHeight(kFontRegular), msg.c_str());
  gfx.fillRect(0, kHeaderH - 2, w, 2, true);
}

// Padlock hero from axis-aligned primitives (Gfx has no diagonals; CrossPoint
// draws its hero from bitmap artwork). locked = filled body (block active),
// unlocked = outline body (idle). ~150 px tall, centered on cx.
void BlockScene::drawLock(Gfx& gfx, const int cx, const int topY, const bool locked) const {
  // Shackle: rounded border, lower half hidden behind the body.
  gfx.drawRoundedRect(cx - 34, topY, 68, 76, 30, 8, true);
  const int bodyY = topY + 52;
  if (locked) {
    gfx.fillRoundedRect(cx - 55, bodyY, 110, 86, 14, true);
    gfx.fillRoundedRect(cx - 9, bodyY + 22, 18, 18, 9, false);  // keyhole
    gfx.fillRect(cx - 4, bodyY + 36, 8, 24, false);
  } else {
    gfx.drawRoundedRect(cx - 55, bodyY, 110, 86, 14, 4, true);  // wipes shackle overlap: draw body last
    gfx.fillRoundedRect(cx - 9, bodyY + 22, 18, 18, 9, true);
    gfx.fillRect(cx - 4, bodyY + 36, 8, 24, true);
  }
}

void BlockScene::renderReady(Gfx& gfx, const BlockStatusStore::Status& card) const {
  const int w = gfx.width();
  const int h = gfx.height();
  const int cx = w / 2;
  const BlockMode& mode = kModes[clampInt(_modeSel, 0, MODE_COUNT - 1)];
  const char* title = card.ready ? presetTitle(card, mode.title) : mode.title;

  // −/+ duration tabs on the screen edges (top-edge physical buttons).
  gfx.drawRoundedRect(-kSideTabRadius, kSideTabY, kSideTabW + kSideTabRadius, kSideTabH, kSideTabRadius, 1, true);
  gfx.drawTextCentered(kFontBold, kSideTabW / 2, kSideTabY + (kSideTabH - gfx.lineHeight(kFontBold)) / 2, "-");
  gfx.drawRoundedRect(w - kSideTabW, kSideTabY, kSideTabW + kSideTabRadius, kSideTabH, kSideTabRadius, 1, true);
  gfx.drawTextCentered(kFontBold, w - kSideTabW / 2, kSideTabY + (kSideTabH - gfx.lineHeight(kFontBold)) / 2, "+");

  // Tabs + mode panel read as ONE horizontal band at kSideTabY: the central
  // component sits between the -/+ edge tabs, matched to their height
  // (taller + narrower than the old bottom panel). CrossPoint panel content
  // (BlockActivity.cpp:309-331) squeezed: icon, title/subtitle, divider,
  // bare "Nm" readout (no "duration" word, no +/- rail, no Start CTA).
  const int panelX = kSideTabW + 14;
  const int panelW = w - 2 * panelX;
  const int panelH = kSideTabH;
  const int panelY = kSideTabY;
  gfx.drawRoundedRect(panelX, panelY, panelW, panelH, 18, 3, true);
  drawModeIcon(gfx, clampInt(_modeSel, 0, MODE_COUNT - 1), panelX + 18, panelY + (panelH - BlockIconHeight) / 2);
  // Single centered title line — the old target-count subtitle is gone (the
  // duration readout to the right of the divider already covers minutes).
  const int textX = panelX + 78;
  gfx.drawText(kFontBold, textX, panelY + (panelH - gfx.lineHeight(kFontBold)) / 2, title);
  const int divX = panelX + panelW - 96;
  gfx.fillRect(divX, panelY + 18, 2, panelH - 36, true);
  char duration[16];
  snprintf(duration, sizeof(duration), "%dm", _durationMin);
  gfx.drawText(kFontBold, divX + 16, panelY + (panelH - gfx.lineHeight(kFontBold)) / 2, duration);

  const int rowBottom = kSideTabY + kSideTabH;
  gfx.drawTextCentered(kFontRegular, cx, rowBottom + 12, _localMsg);

  // Quiet completion stats above the field art — today's count, the streak of
  // consecutive completed blocks, and the all-time total (shown once you've
  // finished at least one). Motivation to keep the streak alive.
  int heroBandTop = rowBottom + 12 + gfx.lineHeight(kFontRegular) + 6;
  const BlockStatusStore::Status stats = BLOCK_STATUS.get();
  if (stats.total > 0 || stats.blocksToday > 0) {
    char line[56];
    snprintf(line, sizeof(line), "Today %d      Streak %d", stats.blocksToday, stats.streak);
    gfx.drawTextCentered(kFontSmall, cx, heroBandTop, line);
    heroBandTop += gfx.lineHeight(kFontSmall) + 10;
  }

  // The field illustration owns everything below the band, centered both
  // ways (regenerated bigger per panel: X3 480x330, X4 432x297 -- see
  // src/art/BlockArtwork.h).
  const int heroBandH = h - Scene::SOFTKEY_BAR_H - heroBandTop;
  const int heroY = heroBandTop + (heroBandH - BlockHeroHeight) / 2;
  drawArtwork(gfx, BlockHeroField, (w - BlockHeroWidth) / 2, heroY, BlockHeroWidth, BlockHeroHeight);
}

void BlockScene::renderActive(Gfx& gfx, const BlockStatusStore::Status& card) {
  const int w = gfx.width();
  const int h = gfx.height();
  const int cx = w / 2;

  // The snapshot carries both provenances (fromCard distinguishes them), so
  // the old fresh-card/seeded duality collapses: preset and onBreak read the
  // same either way.
  const char* title = presetTitle(card, "Deep Work");

  // A LIVE countdown exists only once we've latched an end from a real card
  // (or the card still carries remainingMinutes). A pure wake-from-sleep
  // seed has NO elapsed-time info (no RTC on X3/X4), so we must show the phone's
  // ABSOLUTE end label ("until 10:30 AM") instead of a stale minute number.
  const bool haveLive = _endValid || (card.fromCard && card.remainingMinutes > 0);
  const bool onBreak = card.onBreak;

  gfx.drawTextCentered(kFontBold, cx, kTitleY, title);

  // Countdown panel (CrossPoint renderActive: bordered panel, remaining
  // number + label, progress bar underneath).
  const int panelW = w - 2 * (kMarginX + 30);
  const int panelX = kMarginX + 30;
  const int panelH = 122;
  const int panelY = h - Scene::SOFTKEY_BAR_H - 68 - panelH - 46;

  // WORK-sign hero centered between the title and the countdown panel.
  const int heroTop = kTitleY + 56;
  drawArtwork(gfx, BlockHeroWork, (w - BlockWorkWidth) / 2,
              heroTop + (panelY - heroTop - BlockWorkHeight) / 2, BlockWorkWidth, BlockWorkHeight);
  gfx.drawRoundedRect(panelX, panelY, panelW, panelH, 18, 3, true);

  if (haveLive) {
    // Live minute countdown, corrected by every fresh card.
    const int remaining = _endValid ? remainingNowMin(millis()) : cardRemaining(card);
    _shownRemaining = remaining;  // minute tick repaints when this goes stale
    const int fallbackDuration = _durationMin > remaining ? _durationMin : remaining;
    const int duration = cardDuration(card, fallbackDuration > 0 ? fallbackDuration : 30);
    char number[16];
    snprintf(number, sizeof(number), "%d", remaining);
    gfx.drawTextCentered(kFontBold, cx, panelY + 20, number);
    gfx.drawTextCentered(kFontBold, cx, panelY + 52, onBreak ? "min break" : "min left");
    const int barW = panelW - 40;
    const int progress = duration > 0 ? clampInt(((duration - remaining) * barW) / duration, 0, barW) : barW / 2;
    if (!onBreak && progress > 0) gfx.fillRoundedRect(panelX + 20, panelY + 94, progress, 8, 4, true);
    gfx.drawRoundedRect(panelX + 20, panelY + 94, barW, 8, 4, 2, true);
    gfx.drawTextCentered(kFontRegular, cx, panelY + panelH + 24, _localMsg);
  } else {
    // Seeded (wake-from-sleep) locked view: absolute end time, NEVER a stale
    // countdown. "until 10:30 AM" plus a subtle "updating..." affordance
    // instead of the full-screen "Syncing..." — the live countdown fills in
    // when the phone reconnects and pushes a fresh block-status card.
    _shownRemaining = -1;  // no live minute on glass yet
    char endLabel[32];
    if (card.endsAtLabel[0]) {
      snprintf(endLabel, sizeof(endLabel), "until %s", card.endsAtLabel);
    } else {
      snprintf(endLabel, sizeof(endLabel), "%s", onBreak ? "on a break" : "active");
    }
    gfx.drawTextCentered(kFontBold, cx, panelY + 34, endLabel);
    gfx.drawTextCentered(kFontRegular, cx, panelY + 78, "updating...");
  }
}

void BlockScene::renderModes(Gfx& gfx) const {
  const int w = gfx.width();
  const int cx = w / 2;
  gfx.drawTextCentered(kFontRegular, cx, kHeaderH + 14, "Choose a preset");

  const int rowH = 76;
  const int rowGap = 12;
  const int x = kMarginX;
  const int rowW = w - 2 * kMarginX;
  int y = kHeaderH + 52;
  for (int i = 0; i < MODE_COUNT; i++) {
    const bool selected = i == _modeSel;
    gfx.drawRoundedRect(x, y, rowW, rowH, 16, selected ? 4 : 1, true);
    // Per-preset icon + text at x+92, CrossPoint's renderModes row layout
    // (BlockActivity.cpp:384-392).
    drawModeIcon(gfx, i, x + 24, y + (rowH - BlockIconHeight) / 2);
    gfx.drawText(kFontBold, x + 92, y + 14, kModes[i].title);
    char subtitle[16];
    snprintf(subtitle, sizeof(subtitle), "%d min", kModes[i].minutes);
    gfx.drawText(kFontRegular, x + 92, y + 42, subtitle);
    if (selected) gfx.fillRoundedRect(x + rowW - 46, y + (rowH - 22) / 2, 22, 22, 11, true);
    y += rowH + rowGap;
  }
}

void BlockScene::renderBreak(Gfx& gfx) const {
  const int w = gfx.width();
  const int cx = w / 2;
  gfx.drawTextCentered(kFontBold, cx, kHeaderH + 20, "Take a break?");

  const int rowH = 62;
  const int rowGap = 18;
  const int x = kMarginX;
  const int rowW = w - 2 * kMarginX;
  int y = kHeaderH + 76;
  for (int i = 0; i < BREAK_COUNT; i++) {
    const bool selected = i == _breakSel;
    if (selected) {
      gfx.fillRoundedRect(x, y, rowW, rowH, 14, true);
    } else {
      gfx.drawRoundedRect(x, y, rowW, rowH, 14, 2, true);
    }
    gfx.drawText(kFontBold, x + 24, y + 8, kBreaks[i].title, !selected);
    gfx.drawText(kFontRegular, x + 24, y + 32, kBreaks[i].subtitle, !selected);
    y += rowH + rowGap;
  }
}

void BlockScene::render(Gfx& gfx) {
  _wCache = static_cast<int16_t>(gfx.width());  // header dirty rect (arrow flash)
  // Renders happen only on dirty — entry, input, a store revision change
  // marshalled by main.cpp, or the local minute tick. The store snapshot is
  // block-status-only by construction (updateFromCard runs only for
  // block-status cards), so the old "resync burst clobbers the shared card
  // slot" hazard is structurally gone.
  const BlockStatusStore::Status card = BLOCK_STATUS.get();

  // A FRESH block-status card (fromCard, new revision) is the phone's
  // authoritative answer: it resolves any pending transient, cancels
  // optimism, and re-anchors the local countdown. Seed/count revisions also
  // pass through here; the fromCard guard keeps them from acting as cards.
  const uint32_t blockRevision = BLOCK_STATUS.revision();
  if (blockRevision != _seenCardRevision) {
    _seenCardRevision = blockRevision;
    if (isBlockCard(card)) {
      const bool nowActive = isActiveBlockCard(card);
      _optimisticActive = false;
      _optimisticReady = false;
      switch (_pending) {
        case Pending::Start:
          _localMsg = nowActive ? "" : "Phone did not start the block.";
          break;
        case Pending::Stop:
          _localMsg = nowActive ? "Phone is still blocking." : "Block stopped.";
          break;
        case Pending::Break:
          _localMsg = isBreakCard(card) ? "On a break." : "";
          break;
        case Pending::None:
          // No in-flight command, but the fresh card still supersedes any
          // stale status line — notably onEnter's "Requesting Block
          // status...", which otherwise persists over an active block.
          _localMsg = isBreakCard(card) ? "On a break." : (nowActive ? "" : "Start a phone block.");
          break;
      }
      _pending = Pending::None;
      if (nowActive) {
        const int rem = cardRemaining(card);
        if (rem > 0) latchEnd(rem);  // phone-corrected countdown anchor
      } else {
        _endValid = false;
        _zeroConfirmSent = false;
      }
    }
  }

  // M4.3 wake-from-active-block: with NO fresh card yet (RAM wiped by deep
  // sleep), fall back to the NVS-seeded snapshot so the locked view shows
  // immediately. A real block-status card (isBlockCard) always wins — if it
  // says ready/stopped, the seed is dropped (block ended during sleep).
  const bool seedActive = !isBlockCard(card) && !_optimisticActive && !_optimisticReady && card.active;
  _activeCache = _optimisticReady ? false : (isActiveBlockCard(card) || _optimisticActive || seedActive);

  // Adopt the phone's configured duration while the user hasn't touched −/+
  // (BlockActivity::loop does the same on each revision change).
  if (!_durationCustomized && card.ready && card.durationMinutes > 0) {
    _durationMin = clampInt(card.durationMinutes, kMinBlockMinutes, kMaxBlockMinutes);
  }

  // No block-status card has landed yet (a fresh cold-boot wake into Block has
  // an empty RAM store) and nothing is in flight or optimistic: label the wait
  // honestly instead of leaving onEnter's stale line. While the link is up,
  // main.cpp's auto-resync has already re-requested block.status, so show
  // "Syncing..." until the card lands (then active/ready renders); while it is
  // down, keep the existing connect hint. A real card, an in-flight command,
  // or an optimistic state all take precedence.
  if (!isBlockCard(card) && !_optimisticActive && !_optimisticReady && !seedActive && _pending == Pending::None) {
    _localMsg = COMPANION_BLE.isConnected() ? "Syncing..." : "Connect Companion to sync.";
  }

  renderHeader(gfx);
  if (_view == View::Modes) {
    renderModes(gfx);
  } else if (_view == View::Break) {
    renderBreak(gfx);
  } else if (_activeCache) {
    renderActive(gfx, card);
  } else {
    renderReady(gfx, card);
  }
}
