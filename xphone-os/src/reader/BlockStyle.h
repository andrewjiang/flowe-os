// Pruned from x4-os lib/Epub/Epub/blocks/BlockStyle.h + css/CssStyle.h.
// The CSS engine is removed for R1; only the enums/fields that are serialized
// into section.bin v27 TextBlocks are kept (same underlying types and values),
// plus the combine helpers the pruned parser still uses. Margin/padding fields
// stay so the on-disk TextBlock layout is unchanged — without CSS they are
// always 0.
#pragma once

#include <algorithm>
#include <cstdint>

namespace reader {

// Matches order of PARAGRAPH_ALIGNMENT in CrossPointSettings / CssTextAlign.
enum class CssTextAlign : uint8_t { Justify = 0, Left = 1, Center = 2, Right = 3, None = 4 };

struct BlockStyle {
  CssTextAlign alignment = CssTextAlign::Justify;

  // Spacing (in pixels). Always 0 in R1 (no CSS), kept for format compatibility.
  int16_t marginTop = 0;
  int16_t marginBottom = 0;
  int16_t marginLeft = 0;
  int16_t marginRight = 0;
  int16_t paddingTop = 0;
  int16_t paddingBottom = 0;
  int16_t paddingLeft = 0;
  int16_t paddingRight = 0;
  int16_t textIndent = 0;
  bool textIndentDefined = false;  // true if text-indent was explicitly set (never in R1)
  bool textAlignDefined = false;   // true if text-align was explicitly set
  bool isRtl = false;              // bidi removed for R1: always false
  bool directionDefined = false;   // bidi removed for R1: always false

  // Combined insets (margin + padding)
  [[nodiscard]] int16_t leftInset() const { return marginLeft + paddingLeft; }
  [[nodiscard]] int16_t rightInset() const { return marginRight + paddingRight; }
  [[nodiscard]] int16_t totalHorizontalInset() const { return leftInset() + rightInset(); }
  [[nodiscard]] int16_t topInset() const { return marginTop + paddingTop; }
  [[nodiscard]] int16_t bottomInset() const { return marginBottom + paddingBottom; }

  // Return a copy with bottom margins/padding zeroed out.
  [[nodiscard]] BlockStyle withoutBottom() const {
    BlockStyle result = *this;
    result.marginBottom = 0;
    result.paddingBottom = 0;
    return result;
  }

  // Return a copy with bottom margins/padding collapsed (max) with the source's.
  [[nodiscard]] BlockStyle addBottom(const BlockStyle& source) const {
    BlockStyle result = *this;
    result.marginBottom = std::max(marginBottom, source.marginBottom);
    result.paddingBottom = static_cast<int16_t>(paddingBottom + source.paddingBottom);
    return result;
  }

  enum class CombineAxis : uint8_t {
    Horizontal = 1,  // margins left/right, padding left/right, text-align, text-indent
    Vertical = 2,    // margins top/bottom, padding top/bottom
  };

  // Combine this style's properties with a child style along the specified axis.
  // Properties on the other axis are kept from the child unchanged.
  [[nodiscard]] BlockStyle getCombinedBlockStyle(const BlockStyle& child, CombineAxis axis) const {
    BlockStyle result = child;

    if (axis == CombineAxis::Horizontal) {
      result.marginLeft = static_cast<int16_t>(child.marginLeft + marginLeft);
      result.marginRight = static_cast<int16_t>(child.marginRight + marginRight);
      result.paddingLeft = static_cast<int16_t>(child.paddingLeft + paddingLeft);
      result.paddingRight = static_cast<int16_t>(child.paddingRight + paddingRight);
      if (!child.textIndentDefined && textIndentDefined) {
        result.textIndent = textIndent;
        result.textIndentDefined = true;
      }
      if (!child.textAlignDefined && textAlignDefined) {
        result.alignment = alignment;
        result.textAlignDefined = true;
      }
    } else {
      result.marginTop = std::max(child.marginTop, marginTop);
      result.marginBottom = std::max(child.marginBottom, marginBottom);
      result.paddingTop = static_cast<int16_t>(child.paddingTop + paddingTop);
      result.paddingBottom = static_cast<int16_t>(child.paddingBottom + paddingBottom);
    }

    return result;
  }
};

}  // namespace reader
