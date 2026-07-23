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

 public:
  explicit TextMeasure(const EpdFontFamily& family) : family(family) {}

  const EpdFontFamily& getFamily() const { return family; }

  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getSpaceAdvance(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  int getKerning(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style) const;
  int getFontAscenderSize(int fontId) const;
  int getLineHeight(int fontId) const;
};

}  // namespace reader
