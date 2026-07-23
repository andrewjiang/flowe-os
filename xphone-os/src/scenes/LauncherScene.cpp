#include "LauncherScene.h"

#include <BatteryMonitor.h>

#include <cstdio>
#include <cstring>

#include "../BatteryGauge.h"
#include "../Fonts.h"
#include "../IconStyle.h"
#include "../StatusBar.h"
#include "../art/LauncherIcons.h"
#include "../ble/CompanionBleService.h"
#include "AppScenes.h"

namespace {

// Six focus apps in a 3×2 grid. Settings opens from BACK; About is in Settings.
// Icon bitmaps come from IconStyle (Settings → Icon style packs).
constexpr const char* kApps[LauncherScene::APP_COUNT] = {
    "Notifications", "Read", "Today", "Priorities", "Block", "Workout",
};

// 1bpp blitter for the ported artwork (format per LauncherIcons.h header;
// a cleared bit is ink, only ink pixels are drawn so paper stays white).
//
// Masters are always XPhoneLauncherIconSize wide. `size` is the on-screen
// footprint — when it differs from the master we nearest-neighbor sample
// (same math as Settings' icon-style preview). Using `size` as the row
// stride was a bug: drawing at 88% of 104 made rowBytes 12 instead of 13
// and scrambled the bitmap into TV-static.
void drawIcon(Gfx& gfx, const uint8_t* bitmap, const int x, const int y, const int size) {
  const int srcSize = XPhoneLauncherIconSize;
  const int rowBytes = (srcSize + 7) / 8;
  for (int row = 0; row < size; ++row) {
    const int sy = (size == srcSize) ? row : (row * srcSize) / size;
    for (int col = 0; col < size; ++col) {
      const int sx = (size == srcSize) ? col : (col * srcSize) / size;
      const uint8_t byte = bitmap[sy * rowBytes + (sx >> 3)];
      if (((byte >> (7 - (sx & 7))) & 1) == 0) gfx.drawPixel(x + col, y + row, true);
    }
  }
}

// Layout (both panels are native landscape; everything derives from gfx dims).
constexpr int kMargin = 16;       // outer margin
constexpr int kStatusH = 40;      // status bar height incl. separator
constexpr int kGap = 28;          // gap between cells (row + column)
constexpr int kSelRadius = 12;    // tile rounded-corner radius
constexpr int kSelThick = 3;      // selection border thickness
constexpr int kBoxInset = 8;      // rounded box inset inside the cell (near-full tile)
constexpr int kTilePad = 12;      // inner padding between the box edge and icon/label
constexpr int kMaxTileSide = 186; // cap so 5 tiles don't fill the panel — smaller
                                  // boxes with comfortable margins (approved size)

int gridRows() { return (LauncherScene::APP_COUNT + LauncherScene::COLS - 1) / LauncherScene::COLS; }

// Lazily constructed so BoardConfig::ACTIVE is definitely set (no static-init
// order dependence). X3 reads the BQ27220 fuel gauge over I2C
// (FREEINK_BATTERY_I2C_GAUGE, SDA20/SCL0 from BoardConfig); X4 falls through
// to the ADC divider path in the same binary.
BatteryMonitor& battery() {
  static BatteryMonitor mon;
  return mon;
}

}  // namespace

void LauncherScene::moveSelection(const int dCol, const int dRow) {
  int sel = _sel;
  // Front Left/Right = linear PREV/NEXT with wrap (Workout↔Notifications).
  // Up/Down move by row and still clamp (no wrap) so a short last row feels
  // predictable.
  if (dCol != 0) {
    sel = (sel + dCol) % APP_COUNT;
    if (sel < 0) sel += APP_COUNT;
  }
  if (dRow < 0 && sel >= COLS) sel -= COLS;
  if (dRow > 0) {
    if (sel + COLS < APP_COUNT) {
      sel += COLS;
    } else if (sel / COLS < (APP_COUNT - 1) / COLS) {
      sel = APP_COUNT - 1;  // clamp into a short last row
    }
  }
  if (sel != _sel) {
    const int prev = _sel;
    _sel = sel;
    // M2.1a: a selection move repaints only the two affected cells (the
    // launcher's soft-key labels never change, the status bar is untouched by
    // selection). Before the first render the layout cache is empty and
    // cellRect() returns an empty rect — markDirty(empty) falls back to
    // full-panel, so this is safe in every state.
    XpRect dirty = cellRect(prev);
    dirty.unionWith(cellRect(_sel));
    markDirty(dirty);
  }
}

XpRect LauncherScene::cellRect(const int i) const {
  if (_side <= 0) return XpRect{};  // no layout yet -> full-panel fallback
  constexpr int16_t kSlop = 6;      // border rounding + label overhang past the tile edge
  const int col = i % COLS;
  const int row = i / COLS;
  const int x = _gridX + col * (_side + kGap);
  const int y = _gridY + row * (_cellH + kGap);
  return XpRect{static_cast<int16_t>(x - kSlop), static_cast<int16_t>(y - kSlop),
                static_cast<int16_t>(_side + 2 * kSlop), static_cast<int16_t>(_cellH + 2 * kSlop)};
}

void LauncherScene::handleInput(Input& in) {
  // Quick action: long-press the top-RIGHT button (Btn::Down) to start a
  // Deep Work block without opening the app. Checked before the tap handlers;
  // the Input state machine suppresses the tap-on-release once a long-press
  // fires, so Btn::Down won't also move the selection this press.
  if (in.wasLongPressed(Btn::Down)) {
    showBlockDeepWork();
    return;
  }
  if (in.wasPressed(Btn::Left)) moveSelection(-1, 0);
  if (in.wasPressed(Btn::Right)) moveSelection(+1, 0);
  if (in.wasPressed(Btn::Up)) moveSelection(0, -1);
  if (in.wasPressed(Btn::Down)) moveSelection(0, +1);
  if (in.wasPressed(Btn::Confirm)) {
    const char* app = kApps[_sel];
    if (strcmp(app, "Block") == 0) {
      showBlock();
    } else if (strcmp(app, "Priorities") == 0) {
      showPriorities();
    } else if (strcmp(app, "Today") == 0) {
      showToday();
    } else if (strcmp(app, "Notifications") == 0) {
      showNotifications();
    } else if (strcmp(app, "Read") == 0) {
      showReader();
    } else if (strcmp(app, "Workout") == 0) {
      showWorkout();
    }
  }
  // BACK soft-key (short tap) opens Settings. SceneManager intercepts the
  // LONG-press BACK for the OS-wide go-home (a no-op on the launcher), so only
  // a short tap reaches here.
  if (in.wasPressed(Btn::Back)) showSettings();
}

const char* const* LauncherScene::softKeys() const {
  // Slot 0 (BACK button) opens Settings on the launcher; About lives inside it.
  static constexpr const char* kKeys[4] = {"SETTINGS", "OPEN", "PREV", "NEXT"};
  return kKeys;
}

void LauncherScene::render(Gfx& gfx) {
  const int w = gfx.width();
  const int h = gfx.height();

  // --- Status bar: Flowe lockup left, battery icon + percent right ----------
  // Brand mark from primitives (brand/assets/mark-reference.png): a half sun
  // sitting on the horizon with shrinking water-ripple bars beneath, then the
  // lowercase "flowe" wordmark beside it.
  {
    constexpr int kSunD = 22;
    const int sunX = kMargin;
    const int sunTop = 6;
    const int horizonY = sunTop + kSunD / 2;
    gfx.fillRoundedRect(sunX, sunTop, kSunD, kSunD, kSunD / 2, true);
    gfx.fillRect(sunX - 2, horizonY, kSunD + 4, kSunD / 2 + 2, false);  // carve below the horizon
    const int sunCx = sunX + kSunD / 2;
    gfx.fillRect(sunCx - (kSunD + 4) / 2, horizonY + 3, kSunD + 4, 2, true);   // ripples
    gfx.fillRect(sunCx - (kSunD - 8) / 2, horizonY + 8, kSunD - 8, 2, true);
    gfx.fillRect(sunCx - (kSunD - 16) / 2, horizonY + 13, kSunD - 16, 2, true);
    gfx.drawText(kFontBold, sunX + kSunD + 10, 4, "flowe");
  }
  // CrossPoint-style indicator (StatusBar.h): 15x12 body + nub, proportional
  // fill, percent in kFontSmall to the left. Unknown reads draw "--%" and an
  // empty body so the layout stays stable. Charging bolt when the BQ27220
  // average current is positive (into the battery) — X4's ADC path has no
  // gauge, readAvgCurrentMa returns false there and the bolt is simply off.
  const int barH = kStatusH - 2;  // content height above the separator
  uint16_t pct = 0;
  const bool havePct = battery().readPercentageChecked(pct);
  int16_t avgMa = 0;
  const bool charging = BatteryGauge::readAvgCurrentMa(avgMa) && avgMa > 0;
  const int battLeft = StatusBar::drawBattery(gfx, w - kMargin, barH,
                                              havePct ? static_cast<int>(pct) : -1, charging);

  // M2: BLE status dot left of the battery cluster — solid filled dot when
  // the iPhone is connected, hollow circle while advertising. Nothing before
  // the radio starts (BLE begins after the first launcher paint; see main.cpp).
  if (COMPANION_BLE.isStarted()) {
    const int d = 12;  // circle drawn as a fully-rounded rect
    const int dotX = battLeft - d - 10;
    const int dotY = (barH - d) / 2;
    if (COMPANION_BLE.isConnected()) {
      gfx.fillRoundedRect(dotX, dotY, d, d, d / 2, true);
    } else {
      gfx.drawRoundedRect(dotX, dotY, d, d, d / 2, 2, true);
    }
  }
  gfx.fillRect(0, kStatusH - 2, w, 2, true);  // separator

  // --- App grid: square tiles with the LABEL INSIDE the rounded box ---------
  // Each cell is a plain square (no separate label row) so three rows fit with
  // room to spare. The tile side is the smaller of what the width and height
  // budgets allow, capped at kMaxTileSide so five tiles read as comfortable
  // cards rather than filling the panel; the whole block is then centered both
  // ways between the status bar and the soft-key bar.
  const int rows = gridRows();
  const int availTop = kStatusH;
  const int availH = h - Scene::SOFTKEY_BAR_H - availTop;
  const int sideFromW = (w - 2 * kMargin - (COLS - 1) * kGap) / COLS;
  const int sideFromH = (availH - (rows - 1) * kGap) / rows;  // cellH == side now
  int side = sideFromW < sideFromH ? sideFromW : sideFromH;
  if (side > kMaxTileSide) side = kMaxTileSide;
  const int cellH = side;  // label lives inside the tile

  const int gridBlockW = COLS * side + (COLS - 1) * kGap;
  const int gridH = rows * cellH + (rows - 1) * kGap;
  const int gridX = (w - gridBlockW) / 2;  // center horizontally
  int gridY = availTop + (availH - gridH) / 2;
  if (gridY < availTop + 4) gridY = availTop + 4;  // never collide with chrome

  // M2.1a: cache the layout for cellRect() (selection-move dirty rects).
  _gridX = static_cast<int16_t>(gridX);
  _gridY = static_cast<int16_t>(gridY);
  _side = static_cast<int16_t>(side);
  _cellH = static_cast<int16_t>(cellH);

  // Draw at the native master size. Padding lives in the art itself; do not
  // downscale here (a previous 88% draw used the wrong stride and scrambled
  // every icon into static — Settings preview looked fine because it samples
  // correctly from the 104px source).
  const int iconSize = XPhoneLauncherIconSize;
  const int labelLineH = gfx.lineHeight(kFontBold);

  for (int i = 0; i < APP_COUNT; i++) {
    const int col = i % COLS;
    const int row = i / COLS;
    const int cx = gridX + col * (side + kGap);
    const int cy = gridY + row * (cellH + kGap);

    // Rounded box = the near-full tile (small inset). Thin outline for every
    // app, thick when selected. Icon and label both live inside it.
    // Selection chrome only — unselected tiles have no outline so the icons
    // read as a calm grid of glyphs rather than a wall of boxes.
    const int boxX = cx + kBoxInset;
    const int boxY = cy + kBoxInset;
    const int boxSide = side - 2 * kBoxInset;
    if (i == _sel) {
      gfx.drawRoundedRect(boxX, boxY, boxSide, boxSide, kSelRadius, kSelThick, true);
    }

    // Icon centered horizontally, sitting in the region above the label. The
    // label is pinned near the bottom inside the box; the icon is vertically
    // centered in whatever space remains above it.
    const int iconAreaH = boxSide - 2 * kTilePad - labelLineH;
    int iconY = boxY + kTilePad + (iconAreaH > iconSize ? (iconAreaH - iconSize) / 2 : 0);
    const int iconX = cx + (side - iconSize) / 2;
    if (const uint8_t* bmp = IconStyle::iconForApp(i)) {
      drawIcon(gfx, bmp, iconX, iconY, iconSize);
    }

    // Label inside the box, near the bottom edge.
    const XpFont& f = (i == _sel) ? kFontBold : kFontRegular;
    const int labelY = boxY + boxSide - kTilePad - labelLineH;
    gfx.drawTextCentered(f, cx + side / 2, labelY, kApps[i]);
  }
}
