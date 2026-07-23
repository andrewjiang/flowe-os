// Pruned from x4-os lib/Epub/Epub/blocks/TextBlock.h (+Block.h, folded in).
// Removed: render() (stage 2 supplies a TextRenderer over the accessors),
// focus-reading boundary/suffix vectors (the serialized 1-byte presence flag
// is kept, always 0, so the on-disk v27 TextBlock layout is unchanged), bidi.
#pragma once
#include <EpdFontFamily.h>
#include <HalStorage.h>

#include <memory>
#include <string>
#include <vector>

#include "BlockStyle.h"

namespace reader {

// Represents one laid-out line of text on a page: the words, their absolute
// x positions (justification already applied), and per-word styles.
class TextBlock {
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  BlockStyle blockStyle;

 public:
  explicit TextBlock(std::vector<std::string> words, std::vector<int16_t> word_xpos,
                     std::vector<EpdFontFamily::Style> word_styles, const BlockStyle& blockStyle = BlockStyle())
      : words(std::move(words)),
        wordXpos(std::move(word_xpos)),
        wordStyles(std::move(word_styles)),
        blockStyle(blockStyle) {}
  ~TextBlock() = default;
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  const BlockStyle& getBlockStyle() const { return blockStyle; }
  const std::vector<std::string>& getWords() const { return words; }
  const std::vector<int16_t>& getWordXpos() const { return wordXpos; }
  const std::vector<EpdFontFamily::Style>& getWordStyles() const { return wordStyles; }
  bool isEmpty() const { return words.empty(); }
  size_t wordCount() const { return words.size(); }
  bool serialize(HalFile& file) const;
  static std::unique_ptr<TextBlock> deserialize(HalFile& file);
};

}  // namespace reader
