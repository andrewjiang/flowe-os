#pragma once

// xphone-os Reader R1 — renders one deserialized section.bin Page into the
// 1bpp Gfx framebuffer.
//
// This is the small reimplementation the design doc calls for (GfxRenderer
// itself is too coupled to CrossPoint's HAL to vendor): the glyph-positioning
// math is copied line-for-line from x4-os GfxRenderer::drawText /
// renderCharImpl / renderCharScaled so the words land EXACTLY on the x
// positions the layout engine measured with TextMeasure — both sides use the
// same 12.4 fixed-point differential rounding (prev advance + kern snapped as
// one unit per glyph step). Do not "simplify" the advance loop: any deviation
// breaks justification (word right edges drift off the measured line width).
//
// Glyph bitmaps: all builtin reader fonts are 2-bit COMPRESSED (DEFLATE
// groups); FontDecompressor::getBitmap() decompresses. Per page we prewarm
// one page-buffer slot per font style actually used (<= 4), so page turns do
// a couple of group inflates up front instead of thrashing the hot-group slot
// on every quote/dash; the slots are freed again at the end of renderPage()
// (steady-state reading holds no decompressor heap).
//
// 1bpp ink threshold: the 2-bit alpha levels are 0=white 1=light 2=dark
// 3=black. We ink levels >= 2 (kInkThreshold) — dark-gray + black, i.e. the
// glyph core plus strong edges — which best approximates CrossPoint's default
// grayscale-AA text on a pure black/white buffer. (CrossPoint's own BW
// fallback mode inks >= 1, which reads noticeably bolder; flip the constant
// if that looks better on glass.)

#include <EpdFontFamily.h>
#include <FontDecompressor.h>

#include "../Gfx.h"
#include "TextMeasure.h"

namespace reader {

class Page;

class BookTextRenderer {
 public:
  // Render one deserialized Page with its content origin at (xOff, yOff)
  // (logical-portrait pixels). `measure` must wrap the SAME EpdFontFamily the
  // section cache was measured with (fontId is part of the cache key, so a
  // loaded section always matches the family the caller resolved from it).
  void renderPage(Gfx& gfx, const Page& page, const TextMeasure& measure, int xOff, int yOff);

  // Drop all decompressor caches (page-buffer slots + hot group). renderPage
  // already frees per-page state; this is for scene onExit belt-and-braces.
  void releaseCaches() { decomp_.clearCache(); }

 private:
  static constexpr uint8_t kInkThreshold = 2;  // 2-bit level >= this -> black pixel

  FontDecompressor decomp_;

  void prewarmPage(const Page& page, const EpdFontFamily& family);
  // One word at its cached x position. `baselineY` is the line's baseline
  // ALREADY shifted for SUP/SUB by the caller.
  void drawWord(Gfx& gfx, const EpdFontFamily& family, const char* text, int x, int baselineY,
                EpdFontFamily::Style style);
  // Full-size glyph blit: 2-bit (thresholded) or 1-bit bitmaps.
  void blitGlyph(Gfx& gfx, const EpdFontData* fontData, const EpdGlyph* glyph, int cursorX, int baselineY);
  // 50%-scaled blit for SUP/SUB (2x2 source block per destination pixel,
  // same ink rule as x4-os renderCharScaled).
  void blitGlyphScaled(Gfx& gfx, const EpdFontData* fontData, const EpdGlyph* glyph, int cursorX, int baselineY);
};

}  // namespace reader
