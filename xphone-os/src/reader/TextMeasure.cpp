// Measurement math copied from x4-os lib/GfxRenderer/GfxRenderer.cpp
// (getSpaceWidth / getSpaceAdvance / getKerning / getTextAdvanceX /
// getFontAscenderSize / getLineHeight), with the SD-card-font fast paths and
// the fontId->family map removed: one flash-resident family per TextMeasure.
#include "TextMeasure.h"

#include <Utf8.h>

#include "ReaderLog.h"

namespace reader {

int TextMeasure::getSpaceWidth(const int fontId, const EpdFontFamily::Style style) const {
  (void)fontId;
  const EpdGlyph* spaceGlyph = family.getGlyph(' ', style);
  return spaceGlyph ? fp4::toPixel(spaceGlyph->advanceX) : 0;  // snap 12.4 fixed-point to nearest pixel
}

int TextMeasure::getSpaceAdvance(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                                 const EpdFontFamily::Style style) const {
  (void)fontId;
  const EpdGlyph* spaceGlyph = family.getGlyph(' ', style);
  const int32_t spaceAdvanceFP = spaceGlyph ? static_cast<int32_t>(spaceGlyph->advanceX) : 0;
  // Combine space advance + flanking kern into one fixed-point sum before snapping.
  // Snapping the combined value avoids the +/-1 px error from snapping each component separately.
  const int32_t kernFP = static_cast<int32_t>(family.getKerning(leftCp, ' ', style)) +
                         static_cast<int32_t>(family.getKerning(' ', rightCp, style));
  return fp4::toPixel(spaceAdvanceFP + kernFP);
}

int TextMeasure::getKerning(const int fontId, const uint32_t leftCp, const uint32_t rightCp,
                            const EpdFontFamily::Style style) const {
  (void)fontId;
  const int kernFP = family.getKerning(leftCp, rightCp, style);  // 4.4 fixed-point
  return fp4::toPixel(kernFP);
}

int TextMeasure::getTextAdvanceX(const int fontId, const char* text, EpdFontFamily::Style style) const {
  (void)fontId;
  uint32_t cp;
  uint32_t prevCp = 0;
  int widthPx = 0;
  int32_t prevAdvanceFP = 0;  // 12.4 fixed-point: prev glyph's advance + next kern for snap
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    if (utf8IsCombiningMark(cp)) {
      continue;
    }
    cp = family.applyLigatures(cp, text, style);

    // Differential rounding: snap (previous advance + current kern) together,
    // so measurement and a matching renderer agree exactly.
    if (prevCp != 0) {
      const auto kernFP = family.getKerning(prevCp, cp, style);  // 4.4 fixed-point kern
      widthPx += fp4::toPixel(prevAdvanceFP + kernFP);           // snap 12.4 fixed-point to nearest pixel
    }

    if (cp > 0x20) {
      _glyphsSeen++;
      if (!family.covers(cp, style)) _glyphsMissing++;
    }
    const EpdGlyph* glyph = family.getGlyph(cp, style);
    prevAdvanceFP = glyph ? glyph->advanceX : 0;
    if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
      prevAdvanceFP = (prevAdvanceFP + 1) / 2;
    }
    prevCp = cp;
  }
  widthPx += fp4::toPixel(prevAdvanceFP);  // final glyph's advance
  return widthPx;
}

int TextMeasure::getFontAscenderSize(const int fontId) const {
  (void)fontId;
  return family.getData(EpdFontFamily::REGULAR)->ascender;
}

int TextMeasure::getLineHeight(const int fontId) const {
  (void)fontId;
  return family.getData(EpdFontFamily::REGULAR)->advanceY;
}

}  // namespace reader
