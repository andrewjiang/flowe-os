// Pruned from x4-os lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h.
// Removed: CSS engine hooks (class/style/dir attributes, CssParser), images
// (subtree skipped entirely), table flattening with generated cell headers
// (cells simply start new paragraphs), footnote link collection, anchor maps,
// TOC-anchor page breaks, paragraph/li XPath LUT counters, focus reading,
// indexing popup callback. Kept: streaming expat SAX parse, word buffer with
// UTF-8-safe overflow, no-break-space continuation groups, b/i/u/sup/sub style
// stacks, block/header handling, incremental layout of very long blocks.
#pragma once

#include <expat.h>

#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "BlockStyle.h"
#include "ParsedText.h"
#include "TextBlock.h"
#include "TextMeasure.h"

namespace reader {

class Page;
class Epub;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
  std::shared_ptr<Epub> epub;
  const std::string& filepath;
  const TextMeasure& renderer;
  std::function<void(std::unique_ptr<Page>)> completePageFn;
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  int underlineUntilDepth = INT_MAX;
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  bool parseFailed = false;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  // Resolved default alignment (user pref, Justify when unset).
  CssTextAlign defaultAlignment = CssTextAlign::Justify;

  // Inline style tracking for b/i/u/sup/sub tags
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasUnderline = false, underline = false;
    bool hasSup = false, sup = false;
    bool hasSub = false, sub = false;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  std::vector<BlockStyle> blockStyleStack;  // accumulated block styles from open ancestor elements
  bool effectiveBold = false;
  bool effectiveItalic = false;
  bool effectiveUnderline = false;
  bool effectiveSup = false;
  bool effectiveSub = false;

  int completedPageCount = 0;
  static constexpr size_t MAX_WORDS_PER_TEXT_BLOCK = 192;

  void updateEffectiveInlineStyle();
  void startNewTextBlock(const BlockStyle& blockStyle);
  bool resetCurrentTextBlock(const BlockStyle& blockStyle);
  void flushOversizedTextBlockIfNeeded();
  void flushPartWordBuffer();
  void reservePageElements();
  void makePages();
  void emitHorizontalRule(const BlockStyle& blockStyle);
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(std::shared_ptr<Epub> epub, const std::string& filepath, const TextMeasure& renderer,
                                 const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                 const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                 const uint16_t viewportHeight,
                                 const std::function<void(std::unique_ptr<Page>)>& completePageFn)
      : epub(std::move(epub)),
        filepath(filepath),
        renderer(renderer),
        completePageFn(completePageFn),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight) {}

  ~ChapterHtmlSlimParser() = default;
  bool parseAndBuildPages();
  void addLineToPage(std::shared_ptr<TextBlock> line);
};

}  // namespace reader
