#include "Scene.h"

#include <Arduino.h>

#include "Fonts.h"
#include "scenes/AppScenes.h"

SceneManager SCENES;
RefreshStats gRefreshStats;

const char* const* Scene::softKeys() const {
  static constexpr const char* kDefault[4] = {"BACK", nullptr, nullptr, nullptr};
  return kDefault;
}

namespace {
// M2.1 soft-key bar: 4 small rounded-TOP tabs anchored to the bottom edge,
// one per physical bottom-front button. Drawn by the SceneManager after
// every scene render so per-scene chrome can't drift; labels are static per
// scene in M2, so the bar repaints exactly when the scene repaints (no extra
// e-ink traffic).
constexpr int kTabH = 28;       // visible tab height (reduced with the 10pt labels;
                                // SOFTKEY_BAR_H still reserves 44)
constexpr int kTabRadius = 10;  // top-corner radius
// M3 design pass: the bar spans the middle ~84% of the panel (8% margin per
// side — 42px on X3, 38px on X4) so the four tabs line up with the physical
// front buttons instead of stretching edge to edge.
constexpr int kBarMarginPct = 8;
constexpr int kTabGap = 10;
// Landscape stacks four tabs down a 480/528 px edge instead of across it, so
// the same 10 px gap costs height the stacked letters need. 6 px keeps the
// tabs visually separate and buys the 5-letter labels (BOOKS) their pitch.
constexpr int kTabGapV = 6;
// Stacked-letter pitch = cap height + this. Measured from the font's own 'H',
// never from lineAdvance (24 px against a 15 px cap — prose leading, far too
// loose stacked). ONE pitch for every landscape label so the bar reads as one
// system; a label too long to fit at this pitch is ABBREVIATED by the scene,
// never crushed.
constexpr int kStackGap = 3;
// The longest label the fixed pitch can hold. Scenes must abbreviate past it
// (SELECT -> OK); drawSoftKeyTabVertical tightens as a last resort and says so.
constexpr int kStackMaxLetters = 5;
// Arrow mark length. Portrait tabs are 28 px tall, so 20 is near the ceiling;
// a landscape tab is ~100 px tall and 38 wide, where 20 looked lost — 26 fills
// it without the head (2 * 26 * 5/12 = 21 px wide) touching the sides.
constexpr int kArrowLen = 20;
constexpr int kArrowLenV = 26;

// The four arrow marks, drawn from primitives: a shaft plus a solid
// triangular head. dir is a SoftKey:: sentinel byte (0x11..0x14).
void drawArrow(Gfx& gfx, int cx, int cy, int len, char dir, bool ink) {
  const bool horiz = (dir == '\x11' || dir == '\x12');
  const bool toward0 = (dir == '\x11' || dir == '\x13');  // points at -x / -y
  const int half = len / 2;
  const int head = (len * 5) / 12;
  constexpr int kShaft = 3;
  if (horiz) gfx.fillRect(cx - half, cy - kShaft / 2, len, kShaft, ink);
  else gfx.fillRect(cx - kShaft / 2, cy - half, kShaft, len, ink);
  for (int i = 0; i < head; i++) {
    // One row/column per step, widening away from the tip.
    if (horiz) {
      const int x = toward0 ? cx - half + i : cx + half - 1 - i;
      gfx.fillRect(x, cy - i, 1, 2 * i + 1, ink);
    } else {
      const int y = toward0 ? cy - half + i : cy + half - 1 - i;
      gfx.fillRect(cx - i, y, 2 * i + 1, 1, ink);
    }
  }
}

// Small settings gear from 1-bit primitives: eight teeth around a solid body
// with a carved centre hole. Centred on (cx, cy); `ink` inverts for the
// pressed (black tab) rendering.
void drawGear(Gfx& gfx, int cx, int cy, int r, bool ink = true) {
  const int t = 4;                 // tooth size
  const int o = (r * 7) / 10;      // diagonal offset (~ r / sqrt2)
  gfx.fillRect(cx - t / 2, cy - r - t / 2, t, t, ink);       // N
  gfx.fillRect(cx - t / 2, cy + r - t / 2, t, t, ink);       // S
  gfx.fillRect(cx - r - t / 2, cy - t / 2, t, t, ink);       // W
  gfx.fillRect(cx + r - t / 2, cy - t / 2, t, t, ink);       // E
  gfx.fillRect(cx - o - t / 2, cy - o - t / 2, t, t, ink);   // NW
  gfx.fillRect(cx + o - t / 2, cy - o - t / 2, t, t, ink);   // NE
  gfx.fillRect(cx - o - t / 2, cy + o - t / 2, t, t, ink);   // SW
  gfx.fillRect(cx + o - t / 2, cy + o - t / 2, t, t, ink);   // SE
  const int br = r - 1;                                      // body disc
  gfx.fillRoundedRect(cx - br, cy - br, 2 * br, 2 * br, br, ink);
  const int hr = (r * 2) / 5;                                // centre hole
  gfx.fillRoundedRect(cx - hr, cy - hr, 2 * hr, 2 * hr, hr, !ink);
}

// Tab geometry for one soft-key slot (icon tabs are half width, right-aligned
// in their slot). Shared by the bar painter and the press-feedback flash.
struct TabGeom {
  int x, y, w, h;
};
TabGeom softKeyTabGeom(Gfx& gfx, int slot, bool isIcon) {
  const int w = gfx.width();
  const int marginX = (w * kBarMarginPct) / 100;
  const int tabW = (w - 2 * marginX - 3 * kTabGap) / 4;
  const int tabY = gfx.height() - kTabH;
  int x = marginX + slot * (tabW + kTabGap);
  int tw = tabW;
  if (isIcon) {
    tw = tabW / 2;
    x += tabW - tw;  // right edge stays put
  }
  return {x, tabY, tw, kTabH};
}

// Paint ONE tab (normal or pressed/inverted) into the framebuffer. The
// rounded rect extends kTabRadius px past the panel's bottom edge — drawPixel
// clips it, which squares off the bottom and leaves only the top corners
// rounded. Pressed = solid black tab with white glyphs (E2 press feedback).
void drawSoftKeyTab(Gfx& gfx, int slot, const char* label, bool longPress, bool isIcon, bool pressed) {
  const TabGeom g = softKeyTabGeom(gfx, slot, isIcon);
  gfx.fillRect(g.x, g.y, g.w, g.h, false);  // clear (restore path repaints over the fill)
  if (pressed) {
    gfx.fillRoundedRect(g.x, g.y, g.w, kTabH + kTabRadius, kTabRadius, true);
  } else {
    gfx.drawRoundedRect(g.x, g.y, g.w, kTabH + kTabRadius, kTabRadius, 1, true);
  }
  const bool ink = !pressed;  // glyph colour: black on white, white on black
  if (isIcon) {
    drawGear(gfx, g.x + g.w / 2, g.y + kTabH / 2 + 1, 8, ink);
  } else if (SoftKey::isArrow(label)) {
    drawArrow(gfx, g.x + g.w / 2, g.y + kTabH / 2, kArrowLen, label[0], ink);
  } else if (label && label[0]) {
    const int textY = g.y + (kTabH - gfx.lineHeight(kFontSmall)) / 2 + 1;
    gfx.drawTextCentered(kFontSmall, g.x + g.w / 2, textY, label, ink);
  }
  // M3: subtle hold hint — a 3px dot at the tab's top edge marks a slot that
  // also has a long-press action.
  if (longPress) {
    constexpr int kDot = 3;
    gfx.fillRoundedRect(g.x + (g.w - kDot) / 2, g.y + 4, kDot, kDot, 1, ink);
  }
}

// Landscape: the four tabs run down the panel's button edge, and the label
// reads DOWNWARD one letter per line ("D O W N" stacked) rather than lying on
// its side — sideways words are legible in principle and unreadable at a
// glance, which is the only kind of reading a button label gets.
//
// Slot 0 sits at the BOTTOM: rotating the panel maps the portrait bar's
// leftmost key to the bottom of this edge, and the hardware does not move.
void drawSoftKeyTabVertical(Gfx& gfx, int slot, const char* label, bool longPress, bool isIcon) {
  const int w = gfx.width();
  const int h = gfx.height();
  const int colW = Scene::SOFTKEY_BAR_H - 6;
  const int marginY = (h * kBarMarginPct) / 100;
  const int slotH = (h - 2 * marginY - 3 * kTabGapV) / 4;
  const int x = w - colW;
  const int y = h - marginY - (slot + 1) * slotH - slot * kTabGapV;

  gfx.fillRect(x, y, colW, slotH, false);
  // Extend past the right edge so drawPixel clips it: only the LEFT corners
  // round, mirroring how the portrait bar keeps only its top corners.
  gfx.drawRoundedRect(x, y, colW + kTabRadius, slotH, kTabRadius, 1, true);

  if (isIcon) {
    drawGear(gfx, x + colW / 2, y + slotH / 2, 8, true);
    return;
  }
  if (SoftKey::isArrow(label)) {
    drawArrow(gfx, x + colW / 2, y + slotH / 2, kArrowLenV, label[0], true);
    return;
  }
  if (!label || !label[0]) return;

  // ONE pitch for every label in the bar, derived from the font's own cap
  // height. A fixed pitch is what makes SELECT and GO look like the same
  // system — the old code used lineHeight and then squeezed only the long
  // labels, so every tab had its own spacing.
  int n = 0;
  for (const char* p = label; *p; p++) n++;
  const int capH = gfx.capHeight(kFontSmall);
  int pitch = capH + kStackGap;
  // Ink runs from the first cap's top to the last cap's bottom — the trailing
  // gap is not ink, so it must not be counted when centring.
  const int avail = slotH - 8;
  if ((n - 1) * pitch + capH > avail) {
    // The scene should have abbreviated (kStackMaxLetters). Tighten to a hard
    // floor rather than draw off the end, and say so on serial once seen.
    pitch = n > 1 ? (avail - capH) / (n - 1) : capH;
    if (pitch < capH + 1) pitch = capH + 1;
    Serial.printf("[xphone-os] softkey: '%s' (%d) over %d letters — pitch %d\n", label, n,
                  kStackMaxLetters, pitch);
  }
  int top = y + (slotH - ((n - 1) * pitch + capH)) / 2;  // top pixel of cap 1
  const int capOff = gfx.capTopOffset(kFontSmall);
  for (const char* p = label; *p; p++, top += pitch) {
    const char one[2] = {*p, 0};
    gfx.drawTextCentered(kFontSmall, x + colW / 2, top - capOff, one);
  }
  if (longPress) {
    constexpr int kDot = 3;
    gfx.fillRoundedRect(x + 4, y + (slotH - kDot) / 2, kDot, kDot, 1, true);
  }
}

void drawSoftKeyBar(Gfx& gfx, const char* const* labels, const uint8_t longPressSlots,
                    const uint8_t iconMask) {
  if (!labels) return;
  const bool landscape = gfx.orientation() == Gfx::Orient::Landscape;
  for (int i = 0; i < 4; i++) {
    const char* label = labels[i];
    const bool isIcon = iconMask & (1u << i);
    if (!isIcon && (!label || label[0] == '\0')) continue;  // hidden tab
    if (landscape) drawSoftKeyTabVertical(gfx, i, label, longPressSlots & (1u << i), isIcon);
    else drawSoftKeyTab(gfx, i, label, longPressSlots & (1u << i), isIcon, /*pressed=*/false);
  }
}
}  // namespace

void SceneManager::loop(Input& in, Gfx& gfx) {
  if (!_active) return;
  if (in.wasLongPressed(Btn::Back)) {
    // M3 OS-wide convention: long-press BACK jumps home from ANY scene
    // (scenes cannot override this in this iteration). The event is consumed
    // here — handleInput is skipped this tick, and wasLongPressed is a
    // one-tick flag, so the (possibly new) scene never sees it. A no-op on
    // the launcher itself (switchTo returns early on the active scene).
    showLauncher();
  } else {
    _active->handleInput(in);
  }
  renderIfDirty(gfx);
}
// (E2 press-invert feedback was tried and REMOVED: a release landing while the
// press-flash was still mid-waveform skipped its restore — tabs stuck black.
// The driver's displayWindowFlash stays available if a stateless use appears.)

void SceneManager::renderNow() {
  if (_flushGfx) renderIfDirty(*_flushGfx);
}

void SceneManager::switchTo(Scene& s) {
  if (_active == &s) return;
  if (_active) _active->onExit();
  _active = &s;
  _needFull = true;  // clean slate on scene change, no ghosting
  s.markDirty();
  s.onEnter();
}

namespace {
// M2.1a refresh discipline.
//
// Ghost-scrub cadence (kScrubAfterRefreshes, declared in Scene.h): CrossPoint
// runs a HALF scrub every N differential page turns (x4-os
// ReaderUtils.h:63-70 displayWithRefreshCycle; N is the user-facing "refresh
// frequency" setting {1,5,10,15,30}, default 15,
// CrossPointSettings.h:136-142/236). Partial windows are more ghost-prone
// than full-page FAST turns (nothing outside the window is ever re-driven),
// so 10 — inside CrossPoint's menu, conservative side of its default.
//
// Partial window only for dirty rects up to this fraction of the panel;
// larger regions take the plain full-panel FAST path (per-tier policy).
constexpr int kPartialMaxAreaPct = 50;
// The X3 driver force-full-syncs its first TWO displays after begin() for
// panel conditioning (Uc8253X3Driver.cpp:143 _initialFullSyncsRemaining = 2);
// partial flushes bypass display(), so hold them back until two full-panel
// flushes have run through it.
constexpr uint8_t kConditioningFlushes = 2;
}  // namespace

void SceneManager::renderIfDirty(Gfx& gfx) {
  if (!_active || !_active->isDirty()) return;
  // M5 Phase 2: a flush is still driving the panel — defer. The scene's dirty
  // state persists and keeps accumulating, so the next compose (right after
  // the worker goes idle) shows the NEWEST state. Input/BLE keep pumping on
  // the loop for the whole waveform instead of blocking inside gfx.flush().
  if (_flushInFlight) return;

  // M4 power model: the panel controller stays powered for the whole awake
  // session (idle-sleep removed — the only panel deepSleep left is inside
  // Sleep::sleepNow(), and wake from that is a full reboot), so no runtime
  // wake/re-init path exists here anymore. _bootFlushes only ever counts up
  // from the boot begin().

  // Capture the dirty info before render/clearDirty.
  const bool dirtyAll = _active->dirtyAll();
  const XpRect rect = _active->dirtyRect();

  const unsigned long t0 = millis();
  // Scenes always compose the COMPLETE frame — the dirty rect only narrows
  // the glass refresh window, so the framebuffer stays a full frame (which
  // the partial paths stream their window rows from).
  gfx.clear();
  _active->render(gfx);
  // The tabs sit against the PHYSICAL button edge in both orientations —
  // rotating the panel does not move the buttons. drawSoftKeyBar picks the
  // horizontal or vertical layout from the current orientation.
  drawSoftKeyBar(gfx, _active->softKeys(), _active->longPressSlots(), _active->softKeyIconMask());
  const unsigned long t1 = millis();

  const bool rectUsable = !dirtyAll && !rect.empty() &&
                          (static_cast<long>(rect.w) * rect.h * 100 <=
                           static_cast<long>(gfx.width()) * gfx.height() * kPartialMaxAreaPct);
  // Tier decision (counters updated here on the loop; the worker only drives
  // the panel). M5 Phase 2 policy change: scene switches use FAST (~450 ms)
  // instead of FULL (~3.2 s) once boot conditioning is done — the periodic
  // HALF scrub bounds the accumulated ghosting (experiment: eyes on glass).
  FlushReq req;
  if (_needFull && _bootFlushes < kConditioningFlushes) {
    req = FlushReq::Full;  // boot conditioning: the X3 driver needs 2 full syncs
    _sinceScrub = 0;
    _bootFlushes++;
  } else if (_sinceScrub >= kScrubAfterRefreshes) {
    req = FlushReq::Half;  // periodic ghost scrub (CrossPoint cadence)
    _sinceScrub = 0;
    if (_bootFlushes < kConditioningFlushes) _bootFlushes++;
  } else if (rectUsable && !_needFull && _bootFlushes >= kConditioningFlushes) {
    req = FlushReq::Window;
    _sinceScrub++;  // FAST fallback is differential too — it accrues ghosting
  } else {
    req = FlushReq::Fast;  // interactions AND scene switches
    _sinceScrub++;
    if (_bootFlushes < kConditioningFlushes) _bootFlushes++;
  }

  _active->clearDirty();
  _needFull = false;
  _composeMs = t1 - t0;
  gRefreshStats.drawMs = _composeMs;
  gRefreshStats.sinceScrub = _sinceScrub;
  ensureFlushTask(gfx);
  dispatchFlush(req, rect);
}

// --- M5 Phase 2: flush worker ------------------------------------------------
// The panel flush is CPU-cheap but wall-clock long (the driver busy-waits the
// waveform with delay(1) yields and holds no locks — verified in EpdBus), so a
// same-priority worker task carries it while the loop keeps sampling input and
// pumping BLE. Exactly one flush is ever in flight; the framebuffer is not
// touched by the loop while _flushInFlight (renderIfDirty defers composing).

void SceneManager::ensureFlushTask(Gfx& gfx) {
  _flushGfx = &gfx;
  if (_flushTask) return;
  // Static stack + TCB (BSS), not xTaskCreate's heap pair: this task lives
  // forever, and its heap-era stack was the ~4.5 KB boot-time wall bounding
  // the reader's post-suspend free plain from below — measured one block
  // shy of the contiguous 32 KB inflate window (FRAGMAP 2026-08-06). In
  // BSS it can't split the heap. IDF's StackType_t is uint8_t, so the
  // element count is the byte count.
  static StaticTask_t tcb;
  alignas(8) static StackType_t stack[4096];
  _flushTask = xTaskCreateStatic(&SceneManager::flushTrampoline, "xp_flush", sizeof(stack), this, 1, stack, &tcb);
}

void SceneManager::dispatchFlush(const FlushReq req, const XpRect& rect) {
  _flushReq = req;
  _flushRect = rect;
  _flushInFlight = true;
  xTaskNotifyGive(_flushTask);
}

void SceneManager::waitFlushIdle() const {
  while (_flushInFlight) delay(2);
}

void SceneManager::flushTrampoline(void* self) { static_cast<SceneManager*>(self)->flushWorkLoop(); }

void SceneManager::flushWorkLoop() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    Gfx& gfx = *_flushGfx;
    const XpRect rect = _flushRect;
    const unsigned long t1 = millis();
    const char* tier = "FAST";
    switch (_flushReq) {
      case FlushReq::Full:
        gfx.flush(EInkDisplay::FULL_REFRESH);
        tier = "FULL";
        break;
      case FlushReq::Half:
        gfx.flush(EInkDisplay::HALF_REFRESH);
        tier = "HALF";
        break;
      case FlushReq::Window: {
        const Gfx::FlushTier t = gfx.flushWindow(rect.x, rect.y, rect.w, rect.h);
        tier = (t == Gfx::FlushTier::Partial) ? "PARTIAL" : "FAST";
        break;
      }
      case FlushReq::Flash:
        gfx.flushWindowFlash(rect.x, rect.y, rect.w, rect.h);
        tier = "FLASH";
        break;
      case FlushReq::Fast:
      default:
        gfx.flush(EInkDisplay::FAST_REFRESH);
        break;
    }
    const unsigned long t2 = millis();
    gRefreshStats.refreshMs = t2 - t1;
    gRefreshStats.tier = tier;
    Serial.printf("[xphone-os] draw=%lums refresh=%lums tier=%s sinceScrub=%u rect=%d,%d %dx%d\n",
                  gRefreshStats.drawMs, gRefreshStats.refreshMs, tier, gRefreshStats.sinceScrub,
                  rect.x, rect.y, rect.w, rect.h);
    _flushInFlight = false;  // release AFTER the panel is fully idle
  }
}
