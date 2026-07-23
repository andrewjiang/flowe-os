#include "BookTextRenderer.h"

#include <Utf8.h>

#include <string>

#include "Page.h"
#include "ReaderLog.h"

namespace reader {

// ---------------------------------------------------------------------------
// Glyph blits. Bit packing verified against x4-os GfxRenderer renderCharImpl:
// 2-bit = 4 px/byte MSB-first (raw 0=white .. 3=black), 1-bit = 8 px/byte
// MSB-first; pixelPosition = glyphY * width + glyphX, no per-row padding.
// Glyph top-left sits at (cursorX + left, baselineY - top). Gfx::drawPixel
// clips and applies the logical-portrait -> native rotation.
// ---------------------------------------------------------------------------

void BookTextRenderer::blitGlyph(Gfx& gfx, const EpdFontData* fontData, const EpdGlyph* glyph, const int cursorX,
                                 const int baselineY) {
  if (!glyph || glyph->width == 0 || glyph->height == 0) return;
  const uint32_t glyphIndex = static_cast<uint32_t>(glyph - fontData->glyph);
  // Hot-group pointers are only valid until the next getBitmap() call —
  // consume (blit) immediately, never stash.
  const uint8_t* bmp = decomp_.getBitmap(fontData, glyph, glyphIndex);
  if (!bmp) return;

  const int x0 = cursorX + glyph->left;
  const int y0 = baselineY - glyph->top;
  const int w = glyph->width;
  const int h = glyph->height;

  if (fontData->is2Bit) {
    int pos = 0;
    for (int gy = 0; gy < h; gy++) {
      for (int gx = 0; gx < w; gx++, pos++) {
        const uint8_t raw = (bmp[pos >> 2] >> ((3 - (pos & 3)) * 2)) & 0x3;
        if (raw >= kInkThreshold) gfx.drawPixel(x0 + gx, y0 + gy, true);
      }
    }
  } else {
    int pos = 0;
    for (int gy = 0; gy < h; gy++) {
      for (int gx = 0; gx < w; gx++, pos++) {
        if ((bmp[pos >> 3] >> (7 - (pos & 7))) & 1) gfx.drawPixel(x0 + gx, y0 + gy, true);
      }
    }
  }
}

void BookTextRenderer::blitGlyphScaled(Gfx& gfx, const EpdFontData* fontData, const EpdGlyph* glyph, const int cursorX,
                                       const int baselineY) {
  if (!glyph || glyph->width == 0 || glyph->height == 0) return;
  const uint32_t glyphIndex = static_cast<uint32_t>(glyph - fontData->glyph);
  const uint8_t* bmp = decomp_.getBitmap(fontData, glyph, glyphIndex);
  if (!bmp) return;

  const int srcW = glyph->width;
  const int srcH = glyph->height;
  const int dstW = (srcW + 1) / 2;  // ceil so odd-width glyphs aren't clipped
  const int dstH = (srcH + 1) / 2;
  // Bearings scale by the same 50% (x4-os renderCharScaled).
  const int baseX = cursorX + glyph->left / 2;
  const int baseY = baselineY - glyph->top / 2;

  for (int dy = 0; dy < dstH; dy++) {
    const int sy = dy * 2;
    for (int dx = 0; dx < dstW; dx++) {
      const int sx = dx * 2;
      // Each destination pixel covers a 2x2 source block; ink when the block
      // holds a strong sample OR accumulated coverage (preserves thin strokes
      // nearest-neighbour would drop) — same rule as x4-os.
      uint8_t coverage = 0;
      uint8_t maxRaw = 0;
      bool anyInk1Bit = false;
      for (int oy = 0; oy < 2 && sy + oy < srcH; oy++) {
        for (int ox = 0; ox < 2 && sx + ox < srcW; ox++) {
          const int pos = (sy + oy) * srcW + sx + ox;
          if (fontData->is2Bit) {
            const uint8_t raw = (bmp[pos >> 2] >> ((3 - (pos & 3)) * 2)) & 0x3;
            coverage += raw;
            if (raw > maxRaw) maxRaw = raw;
          } else if ((bmp[pos >> 3] >> (7 - (pos & 7))) & 1) {
            anyInk1Bit = true;
          }
        }
      }
      const bool ink = fontData->is2Bit ? (maxRaw >= 2 || coverage >= 2) : anyInk1Bit;
      if (ink) gfx.drawPixel(baseX + dx, baseY + dy, true);
    }
  }
}

// ---------------------------------------------------------------------------
// Word drawing — the parity-critical loop. Mirrors GfxRenderer::drawText
// (TextRotation::None, no bidi: blockStyle.isRtl is always false in R1):
//  * niqqud skip, combining marks centered over the last base glyph,
//  * ligature substitution,
//  * differential rounding: pen += fp4::toPixel(prevAdvanceFP + kernFP),
//  * SUP/SUB advance halved (TextMeasure::getTextAdvanceX halves the same
//    way, so measured word widths match rendered widths exactly).
// ---------------------------------------------------------------------------

void BookTextRenderer::drawWord(Gfx& gfx, const EpdFontFamily& family, const char* text, const int x,
                                const int baselineY, const EpdFontFamily::Style style) {
  if (!text || !text[0]) return;
  const EpdFontData* fontData = family.getData(style);  // style bits > 1 masked inside
  const bool isSupSub = (style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0;

  int penX = x;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point
  uint32_t prevCp = 0;

  const char* cursor = text;
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&cursor)))) {
    // Hebrew niqqud skip — parity with GfxRenderer::drawText (the builtin
    // fonts carry no vowel-mark glyphs).
    if (cp >= 0x0591 && cp <= 0x05C7) continue;

    if (utf8IsCombiningMark(cp)) {
      const EpdGlyph* mark = family.getGlyph(cp, style);
      if (!mark) continue;
      const int raiseBy = combiningMark::raiseAboveBase(mark->top, mark->height, lastBaseTop);
      const int markX = combiningMark::centerOver(penX, lastBaseLeft, lastBaseWidth, mark->left, mark->width);
      blitGlyph(gfx, fontData, mark, markX, baselineY - raiseBy);
      continue;  // combining marks don't advance the pen (TextMeasure agrees)
    }

    cp = family.applyLigatures(cp, cursor, style);

    if (prevCp != 0) {
      const auto kernFP = family.getKerning(prevCp, cp, style);  // 4.4 fixed-point
      penX += fp4::toPixel(prevAdvanceFP + kernFP);              // snap as ONE unit
    }

    const EpdGlyph* glyph = family.getGlyph(cp, style);
    lastBaseLeft = glyph ? glyph->left : 0;
    lastBaseWidth = glyph ? glyph->width : 0;
    lastBaseTop = glyph ? glyph->top : 0;
    prevAdvanceFP = glyph ? glyph->advanceX : 0;
    if (isSupSub) prevAdvanceFP = (prevAdvanceFP + 1) / 2;  // scaled glyph, scaled advance

    if (glyph) {
      if (isSupSub) {
        blitGlyphScaled(gfx, fontData, glyph, penX, baselineY);
      } else {
        blitGlyph(gfx, fontData, glyph, penX, baselineY);
      }
    }
    prevCp = cp;
  }
}

// ---------------------------------------------------------------------------
// Page prewarm: bucket the page's words per font style and prewarm one
// decompressor page-slot per style used (getData() folds decoration bits, so
// only the 4 face variants R/B/I/BI matter). Transient heap: <= 4 small
// std::strings (~page text size) + FontDecompressor's own temp group buffer.
// ---------------------------------------------------------------------------

void BookTextRenderer::prewarmPage(const Page& page, const EpdFontFamily& family) {
  std::string buckets[4];
  for (auto& b : buckets) b.reserve(512);

  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*el);
    const auto& block = line.getBlock();
    if (!block) continue;
    const auto& words = block->getWords();
    const auto& styles = block->getWordStyles();
    const size_t n = words.size() < styles.size() ? words.size() : styles.size();
    for (size_t i = 0; i < n; i++) {
      buckets[styles[i] & 0x3] += words[i];
    }
  }

  for (int s = 0; s < 4; s++) {
    if (buckets[s].empty()) continue;
    const EpdFontData* fd = family.getData(static_cast<EpdFontFamily::Style>(s));
    const int missed = decomp_.prewarmCache(fd, buckets[s].c_str());
    if (missed > 0) {
      LOG_DBG("BTR", "prewarm style %d: %d glyph(s) missed (hot-group fallback)", s, missed);
    }
  }
}

void BookTextRenderer::renderPage(Gfx& gfx, const Page& page, const TextMeasure& measure, const int xOff,
                                  const int yOff) {
  const EpdFontFamily& family = measure.getFamily();
  const int ascender = measure.getFontAscenderSize(0);

  decomp_.clearCache();  // free any previous page's slots before allocating new ones
  prewarmPage(page, family);

  for (const auto& el : page.elements) {
    if (el->getTag() == TAG_PageLine) {
      const auto& line = static_cast<const PageLine&>(*el);
      const auto& block = line.getBlock();
      if (!block) continue;
      const auto& words = block->getWords();
      const auto& xpos = block->getWordXpos();
      const auto& styles = block->getWordStyles();
      if (words.size() != xpos.size() || words.size() != styles.size()) {
        LOG_ERR("BTR", "Line skipped: size mismatch (words=%u xpos=%u styles=%u)",
                static_cast<unsigned>(words.size()), static_cast<unsigned>(xpos.size()),
                static_cast<unsigned>(styles.size()));
        continue;
      }

      const int lineX = line.xPos + xOff;
      const int lineTopY = line.yPos + yOff;

      for (size_t i = 0; i < words.size(); i++) {
        const EpdFontFamily::Style style = styles[i];
        const int wordX = lineX + xpos[i];  // CACHED position — never re-measure

        // SUP/SUB baseline shift, relative to the full-size ascender (the
        // cached positions carry only x; the vertical shift is render-time,
        // same constants as x4-os TextBlock::render).
        int baselineY = lineTopY + ascender;
        if ((style & EpdFontFamily::SUP) != 0) {
          baselineY -= ascender * 2 / 5;
        } else if ((style & EpdFontFamily::SUB) != 0) {
          baselineY += ascender / 4;
        }

        drawWord(gfx, family, words[i].c_str(), wordX, baselineY, style);

        if ((style & EpdFontFamily::UNDERLINE) != 0) {
          // TextMeasure::getTextAdvanceX already halves SUP/SUB advances, so
          // no extra halving here (unlike x4-os, whose getTextWidth doesn't).
          const int ulWidth = measure.getTextAdvanceX(0, words[i].c_str(), style);
          if (ulWidth > 0) gfx.fillRect(wordX, baselineY + 2, ulWidth, 1, true);
        }
        // STRIKETHROUGH: not rendered (parity — x4-os TextBlock::render only
        // draws UNDERLINE; the parser never emits the strikethrough bit).
      }
    } else if (el->getTag() == TAG_PageHorizontalRule) {
      const auto& rule = static_cast<const PageHorizontalRule&>(*el);
      gfx.fillRect(rule.xPos + xOff, rule.yPos + yOff, rule.getWidth(), rule.getThickness(), true);
    }
  }

  decomp_.clearCache();  // steady-state reading holds no decompressor heap
}

}  // namespace reader
