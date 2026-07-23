// Pruned from x4-os lib/Epub/Epub/Page.h. Removed: PageImage (images are out
// for R1 — the TAG_PageImage enum value is kept so tags stay v27-stable),
// footnotes (the serialized footnote-count slot is kept, always 0), render()
// methods (stage 2 supplies rendering over the deserialized data).
#pragma once
#include <HalStorage.h>

#include <memory>
#include <vector>

#include "TextBlock.h"

namespace reader {

enum PageElementTag : uint8_t {
  TAG_PageLine = 1,
  TAG_PageImage = 2,  // never emitted by the pruned engine; kept for v27 tag stability
  TAG_PageHorizontalRule = 3,
};

// represents something that has been added to a page
class PageElement {
 public:
  int16_t xPos;
  int16_t yPos;
  explicit PageElement(const int16_t xPos, const int16_t yPos) : xPos(xPos), yPos(yPos) {}
  virtual ~PageElement() = default;
  virtual bool serialize(HalFile& file) = 0;
  virtual PageElementTag getTag() const = 0;
};

// a line from a block element
class PageLine final : public PageElement {
  std::shared_ptr<TextBlock> block;

 public:
  PageLine(std::shared_ptr<TextBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), block(std::move(block)) {}
  const std::shared_ptr<TextBlock>& getBlock() const { return block; }
  bool serialize(HalFile& file) override;
  PageElementTag getTag() const override { return TAG_PageLine; }
  static std::unique_ptr<PageLine> deserialize(HalFile& file);
};

class PageHorizontalRule final : public PageElement {
  uint16_t width;
  uint8_t thickness;

 public:
  PageHorizontalRule(uint16_t width, uint8_t thickness, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), width(width), thickness(thickness) {}

  uint16_t getWidth() const { return width; }
  uint8_t getThickness() const { return thickness; }
  bool serialize(HalFile& file) override;
  PageElementTag getTag() const override { return TAG_PageHorizontalRule; }
  static std::unique_ptr<PageHorizontalRule> deserialize(HalFile& file);
};

class Page {
 public:
  // the list of laid-out elements on this page
  std::vector<std::shared_ptr<PageElement>> elements;

  bool serialize(HalFile& file) const;
  static std::unique_ptr<Page> deserialize(HalFile& file);
};

}  // namespace reader
