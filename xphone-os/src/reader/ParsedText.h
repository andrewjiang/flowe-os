// Pruned from x4-os lib/Epub/Epub/ParsedText.h.
// Removed: bidi/RTL (MiniBidi), CJK breaking, focus reading, Liang
// hyphenation. Kept: the non-hyphenated DP line-break path + justification
// (x4-os ParsedText.cpp:538-651), soft-hyphen stripping, NFC composition,
// continuation groups (no-break spaces), first-line indent.
#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "BlockStyle.h"
#include "TextBlock.h"
#include "TextMeasure.h"

namespace reader {

class ParsedText {
  // Must stay below MAX_WORDS_PER_TEXT_BLOCK (192): the parser flushes there,
  // so capacity past the flush point is unreachable. Kept small so the init()
  // reserve is a modest contiguous ask on a fragmented heap.
  static constexpr size_t INITIAL_WORD_CAPACITY = 64;
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;      // true = word attaches to previous with no break
  std::vector<bool> wordNoSpaceBefore;  // kept for layout-code shape; never set without CJK breaking
  BlockStyle blockStyle;
  bool extraParagraphSpacing;
  bool isNaturalAlign;
  bool layoutFailed;  // set when extractLine cannot allocate a TextBlock; checked by the parser after layout

  int resolveFirstLineIndent(bool isFirstLine, const TextMeasure& renderer, int fontId) const;
  std::vector<size_t> computeLineBreaks(const TextMeasure& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        std::vector<bool>& noSpaceBeforeVec);
  // Emergency split for a word wider than the line (replaces the Liang
  // fallback-hyphenation call in the original DP path): splits at the widest
  // codepoint-boundary prefix that fits, appending a visible hyphen.
  bool forceSplitWordAtIndex(size_t wordIndex, int availableWidth, const TextMeasure& renderer, int fontId,
                             std::vector<uint16_t>& wordWidths);
  void extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine, const TextMeasure& renderer,
                   int fontId);
  std::vector<uint16_t> calculateWordWidths(const TextMeasure& renderer, int fontId);

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const BlockStyle& blockStyle = BlockStyle())
      : blockStyle(blockStyle), extraParagraphSpacing(extraParagraphSpacing), isNaturalAlign(false), layoutFailed(false) {}
  ~ParsedText() = default;

  // Reserves word storage. vector::reserve throws on OOM (-fno-exceptions →
  // abort), so this pre-checks the largest free block and returns false when
  // the heap is too tight; the caller must treat false as a parse failure.
  bool init();
  bool hasLayoutFailed() const { return layoutFailed; }

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false);
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  void layoutAndExtractLines(const TextMeasure& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true);
};

}  // namespace reader
