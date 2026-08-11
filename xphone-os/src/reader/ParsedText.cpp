// Pruned from x4-os lib/Epub/Epub/ParsedText.cpp (1175 LOC -> ~430).
// Kept verbatim where possible: the non-hyphenated DP line-break algorithm
// (original lines 538-651), justification (computeJustifyExtra), soft-hyphen
// handling, measureWordWidth, the LTR positioning loop of extractLine.
// Removed: bidi/RTL reorder path, CJK break opportunities, focus reading,
// Liang hyphenation (computeHyphenatedLineBreaks/hyphenateWordAtIndex — an
// emergency per-codepoint forceSplitWordAtIndex replaces the oversized-word
// fallback), SD-card font metric preloading.
#include "ParsedText.h"

#include <Utf8.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <limits>
#include <new>

namespace reader {

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;
constexpr size_t MIN_JUSTIFY_GAPS = 1;

// Returns the first rendered codepoint of a word (skipping leading soft hyphens).
uint32_t firstCodepoint(const std::string& word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  while (true) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD) return cp;  // skip soft hyphens
  }
}

// Returns the last codepoint of a word by scanning backward for the start of the last UTF-8 sequence.
uint32_t lastCodepoint(const std::string& word) {
  if (word.empty()) return 0;
  // UTF-8 continuation bytes start with 10xxxxxx; scan backward to find the leading byte.
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) {
    --i;
  }
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + i);
  return utf8NextCodepoint(&ptr);
}

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

int computeJustifyExtra(const int spareSpace, const size_t gapCount) {
  if (gapCount < MIN_JUSTIFY_GAPS || spareSpace <= 0) return 0;
  // Distribute the spare space evenly across gaps. Do NOT bail out to 0 when the
  // per-gap stretch is large: a sparse line (few words on a wide page) legitimately
  // needs big gaps to reach the margin.
  return spareSpace / static_cast<int>(gapCount);
}

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances + kerning) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
uint16_t measureWordWidth(const TextMeasure& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool appendHyphen = false) {
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    return renderer.getSpaceWidth(fontId, style);
  }
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) {
    return renderer.getTextAdvanceX(fontId, word.c_str(), style);
  }

  std::string sanitized = word;
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return renderer.getTextAdvanceX(fontId, sanitized.c_str(), style);
}

}  // namespace

bool ParsedText::init() {
  // The word vector is the largest single ask; styles plus the two bit-vectors
  // add a few hundred bytes. Demand 4 KB of headroom on top so this reserve is
  // never the allocation that exhausts the heap mid-parse.
  constexpr size_t reserveBytes = INITIAL_WORD_CAPACITY * (sizeof(std::string) + sizeof(EpdFontFamily::Style)) +
                                  2 * (INITIAL_WORD_CAPACITY / 8);
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < reserveBytes + 4096) {
    return false;
  }
  words.reserve(INITIAL_WORD_CAPACITY);
  wordStyles.reserve(INITIAL_WORD_CAPACITY);
  wordContinues.reserve(INITIAL_WORD_CAPACITY);
  wordNoSpaceBefore.reserve(INITIAL_WORD_CAPACITY);
  return true;
}

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious) {
  if (word.empty()) return;

  // The device fonts carry no combining-mark positioning, so EPUB text stored in NFD
  // (a base letter followed by separate combining accents) renders with the marks
  // detached or misplaced. Compose to NFC here, the single funnel every word passes
  // through, so a precomposed glyph is used instead. This runs once per word at
  // layout time (the result is cached in the section file) and is a cheap no-op for
  // mark-free text.
  word = utf8ComposeNfc(word);

  EpdFontFamily::Style baseStyle = fontStyle;
  if (underline) {
    baseStyle = static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::UNDERLINE);
  }

  words.push_back(std::move(word));
  wordStyles.push_back(baseStyle);
  wordContinues.push_back(attachToPrevious);
  wordNoSpaceBefore.push_back(false);
}

int ParsedText::resolveFirstLineIndent(const bool isFirstLine, const TextMeasure& renderer, const int fontId) const {
  if (!isFirstLine || !isNaturalAlign) {
    return 0;
  }
  if (blockStyle.textIndentDefined) {
    if (blockStyle.textIndent < 0 || !extraParagraphSpacing) {
      return blockStyle.textIndent;
    }
    return 0;
  }
  if (!extraParagraphSpacing) {
    return renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR) * 3;
  }
  return 0;
}

// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const TextMeasure& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                       const bool includeLastLine) {
  if (words.empty()) {
    return;
  }

  isNaturalAlign = blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left;

  const int pageWidth = viewportWidth;
  auto wordWidths = calculateWordWidths(renderer, fontId);

  std::vector<size_t> lineBreakIndices =
      computeLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore);
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore, lineBreakIndices, processLine, renderer,
                fontId);
    if (layoutFailed) {
      return;  // OOM in extractLine — stop allocating; the parse is already lost
    }
  }

  // Remove consumed words so size() reflects only remaining words
  if (lineCount > 0) {
    const size_t consumed = lineBreakIndices[lineCount - 1];
    words.erase(words.begin(), words.begin() + consumed);
    wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
    wordContinues.erase(wordContinues.begin(), wordContinues.begin() + consumed);
    wordNoSpaceBefore.erase(wordNoSpaceBefore.begin(), wordNoSpaceBefore.begin() + consumed);
  }
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const TextMeasure& renderer, const int fontId) {
  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(words.size());

  for (size_t i = 0; i < words.size(); ++i) {
    wordWidths.push_back(measureWordWidth(renderer, fontId, words[i], wordStyles[i]));
  }

  return wordWidths;
}

// The non-hyphenated DP line-break path (x4-os ParsedText.cpp:538-651),
// unchanged except the oversized-word fallback now calls forceSplitWordAtIndex.
std::vector<size_t> ParsedText::computeLineBreaks(const TextMeasure& renderer, const int fontId, const int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec) {
  if (words.empty()) {
    return {};
  }

  const int firstLineIndent = resolveFirstLineIndent(true, renderer, fontId);

  // Ensure any word that would overflow even as the first entry on a line is split using the emergency splitter.
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    // First word needs to fit in reduced width if there's an indent
    const int effectiveWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;
    while (wordWidths[i] > effectiveWidth) {
      if (!forceSplitWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths)) {
        break;
      }
    }
  }

  const size_t totalWordCount = words.size();

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int> dp(totalWordCount);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  std::vector<size_t> ans(totalWordCount);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = 0;
    dp[i] = MAX_COST;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;

    for (size_t j = i; j < totalWordCount; ++j) {
      // Add space before word j, unless it's the first word on the line or a continuation
      int gap = 0;
      if (j > static_cast<size_t>(i) && noSpaceBeforeVec[j]) {
        gap = 0;
      } else if (j > static_cast<size_t>(i) && !continuesVec[j]) {
        gap =
            renderer.getSpaceAdvance(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
      } else if (j > static_cast<size_t>(i) && continuesVec[j]) {
        // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
        gap = renderer.getKerning(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
      }
      currlen += wordWidths[j] + gap;

      if (currlen > effectivePageWidth) {
        break;
      }

      // Cannot break after word j if the next word attaches to it (continuation group)
      if (j + 1 < totalWordCount && continuesVec[j + 1]) {
        continue;
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;  // Last line
      } else {
        const int remainingSpace = effectivePageWidth - currlen;
        // Use long long for the square to prevent overflow
        const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      if (cost < dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }

    // Handle oversized word: if no valid configuration found, force single-word line
    // This prevents cascade failure where one oversized word breaks all preceding words
    if (dp[i] == MAX_COST) {
      ans[i] = i;  // Just this word on its own line
      // Inherit cost from next word to allow subsequent words to find valid configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1)
  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return lineBreakIndices;
}

// Splits words[wordIndex] into prefix + '-' and remainder at the widest UTF-8
// codepoint boundary whose hyphenated prefix fits availableWidth. Splice
// bookkeeping mirrors the original hyphenateWordAtIndex: the prefix keeps the
// word's continuation flag, the remainder starts fresh.
bool ParsedText::forceSplitWordAtIndex(const size_t wordIndex, const int availableWidth, const TextMeasure& renderer,
                                       const int fontId, std::vector<uint16_t>& wordWidths) {
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  const std::string& word = words[wordIndex];
  const auto style = wordStyles[wordIndex];

  size_t chosenOffset = 0;
  int chosenWidth = -1;

  const auto* start = reinterpret_cast<const unsigned char*>(word.c_str());
  const auto* ptr = start;
  while (*ptr) {
    if (utf8NextCodepoint(&ptr) == 0) break;
    const size_t offset = static_cast<size_t>(ptr - start);
    if (offset >= word.size()) {
      break;  // never "split" at the end of the word
    }
    const int prefixWidth = measureWordWidth(renderer, fontId, word.substr(0, offset), style, /*appendHyphen=*/true);
    if (prefixWidth > availableWidth) {
      break;  // prefix widths grow monotonically; nothing wider will fit
    }
    chosenWidth = prefixWidth;
    chosenOffset = offset;
  }

  if (chosenWidth < 0 || chosenOffset == 0) {
    // Not even a single codepoint plus hyphen fits; let the DP fallback place
    // the word on its own overflowing line.
    return false;
  }

  // Split the word at the selected boundary and append a visible hyphen.
  std::string remainder = word.substr(chosenOffset);
  words[wordIndex].resize(chosenOffset);
  words[wordIndex].push_back('-');

  // Insert the remainder word (with matching style) directly after the prefix.
  // The prefix keeps its original continuation flag (no-break groups stay
  // linked); the remainder starts fresh on the next line.
  words.insert(words.begin() + wordIndex + 1, remainder);
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, style);
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, false);
  wordNoSpaceBefore.insert(wordNoSpaceBefore.begin() + wordIndex + 1, false);

  // Update cached widths to reflect the new prefix/remainder pairing.
  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = measureWordWidth(renderer, fontId, remainder, style);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const std::vector<uint16_t>& wordWidths,
                             const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                             const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             const TextMeasure& renderer, const int fontId) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  const int firstLineIndent = resolveFirstLineIndent(breakIndex == 0, renderer, fontId);

  // Build line data by moving from the original vectors using index range
  std::vector<std::string> lineWords;
  lineWords.reserve(lineWordCount);
  std::vector<EpdFontFamily::Style> lineWordStyles;
  lineWordStyles.reserve(lineWordCount);

  for (size_t i = 0; i < lineWordCount; ++i) {
    std::string word = std::move(words[lastBreakAt + i]);
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
    lineWords.push_back(std::move(word));
    lineWordStyles.push_back(wordStyles[lastBreakAt + i]);
  }

  // Calculate total word width for this line, count actual word gaps,
  // and accumulate total natural gap widths (including space kerning adjustments).
  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;
  int totalNaturalGaps = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    // Count gaps: each word after the first creates a gap, unless it's a continuation
    if (wordIdx > 0 && noSpaceBeforeVec[lastBreakAt + wordIdx]) {
      // Break opportunity with no inserted space — still a stretchable gap.
      actualGapCount++;
    } else if (wordIdx > 0 && !continuesVec[lastBreakAt + wordIdx]) {
      actualGapCount++;
      totalNaturalGaps += renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx - 1]),
                                                   firstCodepoint(lineWords[wordIdx]), lineWordStyles[wordIdx - 1]);
    } else if (wordIdx > 0 && continuesVec[lastBreakAt + wordIdx]) {
      // Non-breaking space tokens (" " with continues=true) are visible, stretchable spaces —
      // count them as justifiable gaps so justifyExtra is distributed to them too.
      if (lineWords[wordIdx] == " ") {
        actualGapCount++;
      }
      // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
      totalNaturalGaps += renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx - 1]),
                                              firstCodepoint(lineWords[wordIdx]), lineWordStyles[wordIdx - 1]);
    }
  }

  // Calculate spacing (account for indent reducing effective page width on first line)
  const int effectivePageWidth = pageWidth - firstLineIndent;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;
  const CssTextAlign effectiveAlignment = blockStyle.alignment;

  // For justified text, compute per-gap extra to distribute remaining space evenly
  const int spareSpace = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
  const int justifyExtra = (effectiveAlignment == CssTextAlign::Justify && !isLastLine)
                               ? computeJustifyExtra(spareSpace, actualGapCount)
                               : 0;

  std::vector<int16_t> lineXPos;
  lineXPos.reserve(lineWordCount);

  // LTR positioning loop (the RTL/bidi-reorder branches are removed for R1).
  int xpos = firstLineIndent;
  if (effectiveAlignment == CssTextAlign::Right) {
    xpos = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
  } else if (effectiveAlignment == CssTextAlign::Center) {
    xpos = (effectivePageWidth - lineWordWidthSum - totalNaturalGaps) / 2;
  }

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineXPos.push_back(static_cast<int16_t>(xpos));

    const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
    if (nextIsContinuation) {
      int advance = wordWidths[lastBreakAt + wordIdx];
      advance += renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx]), firstCodepoint(lineWords[wordIdx + 1]),
                                     lineWordStyles[wordIdx]);
      // wordIdx > 0 mirrors the gap accounting above (which skips index 0): a leading
      // no-break space must not receive justifyExtra, or the line over-stretches by one
      // gap and the last word is pushed past the right margin.
      if (wordIdx > 0 && lineWords[wordIdx] == " " && continuesVec[lastBreakAt + wordIdx] &&
          effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
        advance += justifyExtra;
      }
      xpos += advance;
    } else {
      int gap = 0;
      bool nextNoSpace = false;
      if (wordIdx + 1 < lineWordCount) {
        nextNoSpace = noSpaceBeforeVec[lastBreakAt + wordIdx + 1];
        gap = nextNoSpace ? 0
                          : renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx]),
                                                     firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
      }
      if (wordIdx + 1 < lineWordCount && effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
        gap += justifyExtra;
      }
      xpos += wordWidths[lastBreakAt + wordIdx] + gap;
    }
  }

  auto lineBlock = std::shared_ptr<TextBlock>(
      new (std::nothrow) TextBlock(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), blockStyle));
  if (!lineBlock) {
    layoutFailed = true;  // line dropped; parser checks hasLayoutFailed() and fails the parse
    return;
  }
  processLine(std::move(lineBlock));
}

}  // namespace reader
