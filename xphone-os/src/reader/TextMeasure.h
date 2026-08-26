#pragma once
#include <EpdFontFamily.h>

#include <cstdint>

namespace reader {

// Layout-time text measurement against one EpdFontFamily. Replaces the
// GfxRenderer measurement surface the CrossPoint Epub engine used (GfxRenderer
// itself is too coupled to CrossPoint's HAL to vendor).
//
// The fontId parameter is kept on every method so the ported layout code keeps
// its original call shape; it selects nothing here — one family per
// TextMeasure, chosen by the caller (see ReaderFonts.h). The measurement math
// (12.4 fixed-point advances, differential rounding of advance+kern pairs) is
// copied from GfxRenderer so a future renderer that uses the same math renders
// exactly what was measured.
class TextMeasure {
  const EpdFontFamily& family;
  // Glyph-coverage tally, filled as layout measures words (flowe-os#3 UX):
  // words are re-measured during wrapping, so the counts over-sample, but the
  // RATIO stays representative — its only use is the "sync this book with the
  // app" notice threshold.
  mutable uint32_t _glyphsSeen = 0;
  mutable uint32_t _glyphsMissing = 0;

 public:
  explicit TextMeasure(const EpdFontFamily& family) : family(family) {}

  const EpdFontFamily& getFamily() const { return family; }

  uint32_t glyphsSeen() const { return _glyphsSeen; }
  uint32_t glyphsMissing() const { return _glyphsMissing; }

  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getSpaceAdvance(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  int getKerning(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
};

}  // namespace reader
