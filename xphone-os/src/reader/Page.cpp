// Pruned from x4-os lib/Epub/Epub/Page.cpp (images, footnotes, render removed;
// serialized layout unchanged — footnote count is still written, always 0).
#include "Page.h"

#include <Serialization.h>

#include <new>

#include "ReaderLog.h"

namespace reader {

bool PageLine::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(HalFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto tb = TextBlock::deserialize(file);
  if (!tb) {
    return nullptr;
  }
  auto* line = new (std::nothrow) PageLine(std::move(tb), xPos, yPos);
  if (!line) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageLine");
    return nullptr;
  }
  return std::unique_ptr<PageLine>(line);
}

bool PageHorizontalRule::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);
  serialization::writePod(file, width);
  serialization::writePod(file, thickness);
  return true;
}

std::unique_ptr<PageHorizontalRule> PageHorizontalRule::deserialize(HalFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint16_t width = 0;
  uint8_t thickness = 0;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);
  serialization::readPod(file, width);
  serialization::readPod(file, thickness);

  if (width == 0 || thickness == 0) {
    LOG_ERR("PGE", "Deserialization failed: invalid horizontal rule metadata (width=%u thickness=%u)", width,
            thickness);
    return nullptr;
  }

  auto* rule = new (std::nothrow) PageHorizontalRule(width, thickness, xPos, yPos);
  if (!rule) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageHorizontalRule");
    return nullptr;
  }
  return std::unique_ptr<PageHorizontalRule>(rule);
}

bool Page::serialize(HalFile& file) const {
  const uint16_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    // Use getTag() method to determine type
    serialization::writePod(file, static_cast<uint8_t>(el->getTag()));

    if (!el->serialize(file)) {
      return false;
    }
  }

  // Footnote-count slot (v27 format): footnotes removed, always 0.
  serialization::writePod(file, static_cast<uint16_t>(0));

  return true;
}

std::unique_ptr<Page> Page::deserialize(HalFile& file) {
  auto page = std::unique_ptr<Page>(new (std::nothrow) Page());
  if (!page) {
    LOG_ERR("PGE", "Deserialization failed: OOM allocating Page");
    return nullptr;
  }

  uint16_t count;
  serialization::readPod(file, count);

  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag;
    serialization::readPod(file, tag);

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      if (!pl) {
        return nullptr;
      }
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_PageHorizontalRule) {
      auto rule = PageHorizontalRule::deserialize(file);
      if (!rule) {
        return nullptr;
      }
      page->elements.push_back(std::move(rule));
    } else {
      // TAG_PageImage (never emitted by this engine) or corruption.
      LOG_ERR("PGE", "Deserialization failed: unsupported tag %u", tag);
      return nullptr;
    }
  }

  // Footnote-count slot (v27 format). This engine writes 0; a non-zero count
  // means a foreign/corrupt cache — reject so the section is rebuilt.
  uint16_t fnCount;
  serialization::readPod(file, fnCount);
  if (fnCount != 0) {
    LOG_ERR("PGE", "Deserialization failed: unexpected footnote count %u", fnCount);
    return nullptr;
  }

  return page;
}

}  // namespace reader
