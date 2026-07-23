// Pruned from x4-os lib/Epub/Epub/blocks/TextBlock.cpp: render() dropped
// (stage 2), focus vectors dropped. Byte layout of serialize/deserialize is
// unchanged from section.bin v27: the focus presence flag is still written
// (always 0) and skipped on read if a foreign cache happens to contain it.
#include "TextBlock.h"

#include <Serialization.h>

#include "ReaderLog.h"

namespace reader {

bool TextBlock::serialize(HalFile& file) const {
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size()) {
    LOG_ERR("TXB", "Serialization failed: size mismatch (words=%u, xpos=%u, styles=%u)",
            static_cast<uint32_t>(words.size()), static_cast<uint32_t>(wordXpos.size()),
            static_cast<uint32_t>(wordStyles.size()));
    return false;
  }

  // Word data
  serialization::writePod(file, static_cast<uint16_t>(words.size()));
  for (const auto& w : words) serialization::writeString(file, w);
  for (auto x : wordXpos) serialization::writePod(file, x);
  for (auto s : wordStyles) serialization::writePod(file, s);
  // Focus block presence flag (v27 format slot). Focus reading is removed, so
  // this is always 0 and no per-word vectors follow.
  serialization::writePod(file, static_cast<uint8_t>(0));

  // Style (alignment + margins/padding/indent)
  serialization::writePod(file, blockStyle.alignment);
  serialization::writePod(file, blockStyle.textAlignDefined);
  serialization::writePod(file, blockStyle.marginTop);
  serialization::writePod(file, blockStyle.marginBottom);
  serialization::writePod(file, blockStyle.marginLeft);
  serialization::writePod(file, blockStyle.marginRight);
  serialization::writePod(file, blockStyle.paddingTop);
  serialization::writePod(file, blockStyle.paddingBottom);
  serialization::writePod(file, blockStyle.paddingLeft);
  serialization::writePod(file, blockStyle.paddingRight);
  serialization::writePod(file, blockStyle.textIndent);
  serialization::writePod(file, blockStyle.textIndentDefined);
  serialization::writePod(file, blockStyle.isRtl);
  serialization::writePod(file, blockStyle.directionDefined);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(HalFile& file) {
  uint16_t wc;
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  BlockStyle blockStyle;

  // Word count
  serialization::readPod(file, wc);

  // Sanity check: prevent allocation of unreasonably large vectors (max 10000 words per block)
  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }

  // Word data
  words.resize(wc);
  wordXpos.resize(wc);
  wordStyles.resize(wc);
  for (auto& w : words) serialization::readString(file, w);
  for (auto& x : wordXpos) serialization::readPod(file, x);
  for (auto& s : wordStyles) serialization::readPod(file, s);

  // Focus block (v27 format slot): our writer never sets the flag; skip the
  // per-word annotations if a foreign v27 cache contains them.
  uint8_t hasFocus;
  serialization::readPod(file, hasFocus);
  if (hasFocus) {
    for (uint16_t i = 0; i < wc; i++) {
      uint8_t discardBoundary;
      serialization::readPod(file, discardBoundary);
    }
    for (uint16_t i = 0; i < wc; i++) {
      uint16_t discardSuffixX;
      serialization::readPod(file, discardSuffixX);
    }
  }

  // Style (alignment + margins/padding/indent)
  serialization::readPod(file, blockStyle.alignment);
  serialization::readPod(file, blockStyle.textAlignDefined);
  serialization::readPod(file, blockStyle.marginTop);
  serialization::readPod(file, blockStyle.marginBottom);
  serialization::readPod(file, blockStyle.marginLeft);
  serialization::readPod(file, blockStyle.marginRight);
  serialization::readPod(file, blockStyle.paddingTop);
  serialization::readPod(file, blockStyle.paddingBottom);
  serialization::readPod(file, blockStyle.paddingLeft);
  serialization::readPod(file, blockStyle.paddingRight);
  serialization::readPod(file, blockStyle.textIndent);
  serialization::readPod(file, blockStyle.textIndentDefined);
  serialization::readPod(file, blockStyle.isRtl);
  serialization::readPod(file, blockStyle.directionDefined);

  auto* block = new (std::nothrow) TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles), blockStyle);
  if (!block) {
    LOG_ERR("TXB", "Deserialization failed: OOM");
    return nullptr;
  }
  return std::unique_ptr<TextBlock>(block);
}

}  // namespace reader
