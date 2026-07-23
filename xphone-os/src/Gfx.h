#pragma once

// xphone-os M1 — minimal framebuffer drawer + bitmap text.
//
// Bespoke text path (option "b" from the M1 plan): the full x4-os GfxRenderer
// drags lib/hal wholesale (HalPowerManager.cpp includes WiFi.h, Bitmap.h pulls
// HalStorage/SdFat, plus Logging/Utf8/MiniBidi/uzlib), so instead we blit the
// SDK's own uncompressed EpdFontData-format builtin fonts (x4-os
// lib/EpdFont/builtinFonts, consumed read-only via src/fonts/ symlinks) with
// ~100 lines of code. Glyph packing verified against GfxRenderer.cpp
// renderCharImpl (1bpp, MSB-first, pixelPosition = glyphY*width + glyphX) and
// baseline math against drawText (yPos = y + ascender, glyph top-left at
// (x + left, yPos - top)). Advances are 12.4 fixed-point (fp4::toPixel);
// kerning/ligatures are intentionally skipped (UI labels only).
//
// All drawing happens in LOGICAL PORTRAIT coordinates (X3: 528x792, X4:
// 480x800 — the device is held like a phone); drawPixel() rotates 90° CW into
// the native landscape framebuffer, the same chirality as CrossPoint's default
// GfxRenderer::Portrait orientation (x4-os GfxRenderer.cpp rotateCoordinates).
// In default Portrait x4-os does NOT remap nav buttons (MappedInputManager
// isNavDirectionSwapped() swaps only for PortraitInverted/LandscapeCCW), so
// Input.h's fixed mapping stays correct. 0xFF = white, cleared bit = black
// (single-buffer FreeInkDisplay framebuffer, FreeInkDisplay.cpp memset(0xFF)).

#include <EInkDisplay.h>

#include <cstdint>

#include "fonts/EpdFontData.h"

// Logical-coordinate rectangle (M2.1a dirty rects). w/h <= 0 means empty.
struct XpRect {
  int16_t x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
  void unionWith(const XpRect& o) {
    if (o.empty()) return;
    if (empty()) { *this = o; return; }
    const int16_t x1 = (x + w > o.x + o.w) ? static_cast<int16_t>(x + w) : static_cast<int16_t>(o.x + o.w);
    const int16_t y1 = (y + h > o.y + o.h) ? static_cast<int16_t>(y + h) : static_cast<int16_t>(o.y + o.h);
    if (o.x < x) x = o.x;
    if (o.y < y) y = o.y;
    w = static_cast<int16_t>(x1 - x);
    h = static_cast<int16_t>(y1 - y);
  }
};

// Trimmed font descriptor: references only the bitmap/glyph/interval arrays of
// a builtin font header, so the header's unreferenced EpdFontData instance and
// its ~24KB kern-class matrix + ligature tables are discarded at compile time.
struct XpFont {
  const uint8_t* bitmap;
  const EpdGlyph* glyphs;
  const EpdUnicodeInterval* intervals;
  uint32_t intervalCount;
  uint8_t lineAdvance;  // EpdFontData::advanceY
  int8_t ascender;      // EpdFontData::ascender
};

class Gfx {
 public:
  explicit Gfx(EInkDisplay& d) : _d(d) {}

  // Cache framebuffer + geometry. Call after display.begin(); false if the
  // SDK returned no framebuffer.
  bool begin();

  int width() const { return _w; }
  int height() const { return _h; }

  // M3: the wrapped panel — for the SdUpdate flash UI (progress bar / error
  // X), which draws its font-free chrome straight on the EInkDisplay.
  EInkDisplay& display() { return _d; }

  void clear() { _d.clearScreen(0xFF); }
  void drawPixel(int x, int y, bool black);
  // Straight segment, any slope — for glyph-scale marks (per-pixel cost; use
  // fillRect's byte runs for axis-aligned rules). `thickness` is the side of
  // the filled square stamped on every plotted point.
  void drawLine(int x0, int y0, int x1, int y1, int thickness, bool black);
  void fillRect(int x, int y, int w, int h, bool black);
  void drawRect(int x, int y, int w, int h, int thickness, bool black);
  // Rounded-corner filled rect (integer quarter-circle corners).
  void fillRoundedRect(int x, int y, int w, int h, int r, bool black);
  // Rounded border of `thickness` px, interior left untouched-white (drawn as
  // outer black round-rect + inner white round-rect; draw it before content).
  void drawRoundedRect(int x, int y, int w, int h, int r, int thickness, bool black);

  int textWidth(const XpFont& f, const char* text) const;
  // y = TOP of the line (baseline at y + f.ascender), same as GfxRenderer.
  void drawText(const XpFont& f, int x, int y, const char* text, bool black = true);
  void drawTextCentered(const XpFont& f, int cx, int y, const char* text, bool black = true);
  int lineHeight(const XpFont& f) const { return f.lineAdvance; }
  // M3 detail views: greedy word wrap. Draws up to maxLines lines starting at
  // (x, y) (top of the first line, lines advance by lineAdvance), breaking at
  // spaces; a word longer than maxWidth is hard-broken at a UTF-8 codepoint
  // boundary (never mid-sequence); '\n' forces a break. If text remains after
  // the last allowed line, that line is re-clipped to end in "...". Returns
  // the number of lines drawn (0 for null/empty text).
  int drawTextWrapped(const XpFont& f, int x, int y, const char* text, int maxWidth, int maxLines,
                      bool black = true);

  // Refresh tier actually pushed to glass (instrumentation, M2.1a).
  enum class FlushTier : uint8_t { Full, Half, Fast, Partial };

  // Push framebuffer to glass. FULL on scene switches, HALF for the periodic
  // ghost scrub, FAST for full-panel differential updates.
  void flush(EInkDisplay::RefreshMode mode) { _d.displayBuffer(mode); }

  // M2.1a: partial-window refresh of a LOGICAL rect (framebuffer must already
  // hold the complete frame). The rect is clamped to the panel and expanded
  // outward to native byte boundaries; any degenerate/invalid window falls
  // back to a full-panel FAST flush. Returns the tier actually used.
  FlushTier flushWindow(int x, int y, int w, int h);

  // M5 Phase 3 (E2): fastest windowed flush for tiny press-feedback regions —
  // X3 fires its ~8-frame flash LUT (~170 ms), X4 its native partial. Same
  // logical-rect mapping as flushWindow; invalid windows are silently ignored
  // (feedback is best-effort, never worth a full-panel flush).
  void flushWindowFlash(int x, int y, int w, int h);

 private:
  const EpdGlyph* findGlyph(const XpFont& f, uint32_t cp) const;
  void blitGlyph(const XpFont& f, const EpdGlyph* g, int penX, int lineTopY, bool black);
  static uint32_t nextCodepoint(const char** s);

  EInkDisplay& _d;
  uint8_t* _fb = nullptr;
  int _w = 0, _h = 0, _wBytes = 0;
};
