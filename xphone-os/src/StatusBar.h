#pragma once

// Status-bar battery indicator — CrossPoint's design ported to Gfx primitives.
//
// Geometry is pixel-for-pixel x4-os BaseTheme (read-only reference):
//   - icon 15x12 + 1px terminal nub column (BaseTheme.h BaseMetrics
//     batteryWidth=15/batteryHeight=12; BaseTheme.cpp drawBatteryOutline).
//   - proportional fill inset 2px: maxFillWidth = w-5, fillHeight = h-4,
//     +1 rounding so >=1px is always filled (BaseTheme.cpp fillBatteryIcon).
//   - percentage in the small font, 4px left of the icon
//     (batteryPercentSpacing, drawBatteryRight arrangement).
//   - charging: inverted lightning bolt inside the fill, minimum 8px fill so
//     the bolt stays visible (drawBatteryLightningBolt).
// Gfx has no drawLine, so the 1px strokes are 1px fillRects. Header-only so
// any scene can reuse it without touching the build.

#include <cstdio>

#include "Fonts.h"
#include "Gfx.h"

// Sync-activity queries (defined in main.cpp). xphoneSyncActive() is true while
// a wake resync/ANCS backfill is outstanding; xphoneSyncBusy() is true only
// while something is actually transmitting. See the sync-dot block below.
bool xphoneSyncActive();
bool xphoneSyncBusy();

namespace StatusBar {

constexpr int kBattW = 15;   // body width incl. terminal end line
constexpr int kBattH = 12;   // body height
constexpr int kNubW = 1;     // terminal nub column right of the body
constexpr int kTextGap = 4;  // gap between percentage text and icon

// Draws the battery cluster right-aligned so the nub ends at rightX:
// [pct% in kFontSmall] gap [battery icon]. pct < 0 means unknown ("--%",
// empty icon). barH is the status-bar content height (vertical centering).
// Returns the leftmost x of the cluster so callers can stack more
// indicators to its left.
inline int drawBattery(Gfx& gfx, const int rightX, const int barH, const int pct, const bool charging) {
  const int x = rightX - kBattW - kNubW;
  const int y = (barH - kBattH) / 2;

  // Body outline + nub (CrossPoint drawBatteryOutline).
  gfx.fillRect(x + 1, y, kBattW - 3, 1, true);               // top
  gfx.fillRect(x + 1, y + kBattH - 1, kBattW - 3, 1, true);  // bottom
  gfx.fillRect(x, y + 1, 1, kBattH - 2, true);               // left
  gfx.fillRect(x + kBattW - 2, y + 1, 1, kBattH - 2, true);  // terminal end
  gfx.drawPixel(x + kBattW - 1, y + 3, true);                // nub corners
  gfx.drawPixel(x + kBattW - 1, y + kBattH - 4, true);
  gfx.fillRect(x + kBattW, y + 4, 1, kBattH - 8, true);      // nub

  // Proportional fill + optional bolt (CrossPoint fillBatteryIcon). Unknown
  // charge (pct < 0) leaves the body empty.
  if (pct >= 0) {
    constexpr int kMaxFill = kBattW - 5;    // 10
    constexpr int kFillH = kBattH - 4;      // 8
    constexpr int kMinFillForBolt = 8;      // bolt is 5px wide at +4 offset
    int fill = pct * kMaxFill / 100 + 1;    // +1: always show >=1px
    if (fill > kMaxFill) fill = kMaxFill;
    if (charging && fill < kMinFillForBolt) fill = kMinFillForBolt;
    gfx.fillRect(x + 2, y + 2, fill, kFillH, true);

    if (charging) {
      // Inverted (white-on-fill) bolt, rows verbatim from CrossPoint
      // drawBatteryLightningBolt at (x+4, y+2).
      const int bx = x + 4, by = y + 2;
      gfx.fillRect(bx + 4, by + 0, 2, 1, false);
      gfx.fillRect(bx + 3, by + 1, 2, 1, false);
      gfx.fillRect(bx + 2, by + 2, 4, 1, false);
      gfx.fillRect(bx + 3, by + 3, 2, 1, false);
      gfx.fillRect(bx + 2, by + 4, 2, 1, false);
      gfx.fillRect(bx + 1, by + 5, 4, 1, false);
      gfx.fillRect(bx + 2, by + 6, 2, 1, false);
      gfx.fillRect(bx + 1, by + 7, 2, 1, false);
    }
  }

  // Percentage text, small font, left of the icon.
  char buf[8];
  if (pct >= 0) {
    snprintf(buf, sizeof(buf), "%d%%", pct);
  } else {
    snprintf(buf, sizeof(buf), "--%%");
  }
  const int tw = gfx.textWidth(kFontSmall, buf);
  const int tx = x - kTextGap - tw;
  int ty = (barH - gfx.lineHeight(kFontSmall)) / 2;
  if (ty < 0) ty = 0;
  gfx.drawText(kFontSmall, tx, ty, buf);

  // Sync dot: a small indicator just LEFT of the percentage text, PRESENT only
  // while a companion sync (wake resync or ANCS backfill) is in progress and
  // GONE when idle. It is drawn FILLED while a request/fetch is actually in
  // flight and HOLLOW while waiting between attempts — so it visibly changes on
  // the repaints the sync already triggers, giving a subtle "blink" feel with
  // NO periodic panel refresh (the X3 has composite partial refresh disabled;
  // an animation timer would flash the whole panel every tick). The dot appears
  // and clears on the sync active<->idle edge, which main.cpp turns into a
  // one-shot launcher repaint (pumpCompanionEvents) — never a periodic one.
  if (xphoneSyncActive()) {
    constexpr int kDot = 5;    // small square-ish dot
    constexpr int kDotGap = 4;  // gap left of the percentage text
    const int dx = tx - kDotGap - kDot;
    const int dy = (barH - kDot) / 2;
    if (xphoneSyncBusy()) {
      gfx.fillRoundedRect(dx, dy, kDot, kDot, kDot / 2, true);
    } else {
      gfx.drawRoundedRect(dx, dy, kDot, kDot, kDot / 2, 1, true);
    }
  }
  return tx;
}

}  // namespace StatusBar
