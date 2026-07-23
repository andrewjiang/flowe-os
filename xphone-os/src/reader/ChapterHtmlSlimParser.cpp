// Pruned from x4-os lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
// (1477 LOC -> ~500). See ChapterHtmlSlimParser.h for what was removed.
#include "ChapterHtmlSlimParser.h"

#include <HalStorage.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <new>

#include "Epub.h"
#include "Page.h"
#include "ReaderLog.h"
#include "TextFold.h"
#include "htmlEntities.h"

namespace reader {

namespace {

constexpr size_t PARSE_BUFFER_SIZE = 1024;

constexpr const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote"};
constexpr const char* BOLD_TAGS[] = {"b", "strong"};
constexpr const char* ITALIC_TAGS[] = {"i", "em"};
constexpr const char* UNDERLINE_TAGS[] = {"u", "ins"};
constexpr const char* IMAGE_TAGS[] = {"img", "image"};
constexpr const char* SKIP_TAGS[] = {"head"};

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

bool matches(const char* tag_name, const char* const* possible_tags, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, std::size(HEADER_TAGS)) || matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS));
}

bool isTableStructuralTag(const char* name) {
  return strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 || strcmp(name, "td") == 0 || strcmp(name, "th") == 0;
}

// A footnote-reference anchor: EPUB3 `epub:type="noteref"` (may be a
// space-separated list) or ARIA `role="doc-noteref"`. Their superscripting is
// CSS-only, which we don't parse, so we honour the semantic markup instead.
bool isNoterefAnchor(const XML_Char** atts) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], "epub:type") == 0 && strstr(atts[i + 1], "noteref") != nullptr) return true;
    if (strcmp(atts[i], "role") == 0 && strstr(atts[i + 1], "doc-noteref") != nullptr) return true;
  }
  return false;
}

}  // namespace

// Update effective bold/italic/underline/sup/sub from the inline style stack
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  effectiveBold = false;
  effectiveItalic = false;
  effectiveUnderline = false;
  effectiveSup = false;
  effectiveSub = false;

  // Apply inline style stack in order
  for (const auto& entry : inlineStyleStack) {
    if (entry.hasBold) {
      effectiveBold = entry.bold;
    }
    if (entry.hasItalic) {
      effectiveItalic = entry.italic;
    }
    if (entry.hasUnderline) {
      effectiveUnderline = entry.underline;
    }
    if (entry.hasSup) {
      effectiveSup = entry.sup;
      if (entry.sup) effectiveSub = false;
    }
    if (entry.hasSub) {
      effectiveSub = entry.sub;
      if (entry.sub) effectiveSup = false;
    }
  }
}

// flush the contents of partWordBuffer to currentTextBlock
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  if (parseFailed || !currentTextBlock) return;

  // Determine font style from depth-based tracking and inline style stack
  const bool isBold = boldUntilDepth < depth || effectiveBold;
  const bool isItalic = italicUntilDepth < depth || effectiveItalic;
  const bool isUnderline = underlineUntilDepth < depth || effectiveUnderline;

  // Combine style flags using bitwise OR
  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
  }
  if (isItalic) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
  }
  if (isUnderline) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::UNDERLINE);
  }
  if (effectiveSup) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUP);
  } else if (effectiveSub) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUB);
  }

  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  flushOversizedTextBlockIfNeeded();
  if (parseFailed || !currentTextBlock) return;

  currentTextBlock->addWord(partWordBuffer, fontStyle, false, nextWordContinues);
  partWordBufferIndex = 0;
  nextWordContinues = false;
}

bool ChapterHtmlSlimParser::resetCurrentTextBlock(const BlockStyle& blockStyle) {
  currentTextBlock.reset(new (std::nothrow) ParsedText(extraParagraphSpacing, blockStyle));
  if (!currentTextBlock) {
    LOG_ERR("EHP", "Out of memory while creating text block");
    parseFailed = true;
    return false;
  }
  return true;
}

void ChapterHtmlSlimParser::flushOversizedTextBlockIfNeeded() {
  if (parseFailed || !currentTextBlock || currentTextBlock->size() < MAX_WORDS_PER_TEXT_BLOCK) return;

  const BlockStyle blockStyle = currentTextBlock->getBlockStyle();
  LOG_DBG("EHP", "Flushing oversized text block at %u words", static_cast<unsigned>(currentTextBlock->size()));
  makePages();
  resetCurrentTextBlock(blockStyle);
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  nextWordContinues = false;  // New block = new paragraph, no continuation
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      // Container elements deposit their vertical margins on the empty block
      // when they open; merge those into the new style so the first child in a
      // container inherits the container's vertical spacing.
      const auto style = currentTextBlock->getBlockStyle();
      currentTextBlock->setBlockStyle(style.getCombinedBlockStyle(blockStyle, BlockStyle::CombineAxis::Vertical));
      return;
    }

    makePages();
  }
  resetCurrentTextBlock(blockStyle);
}

void ChapterHtmlSlimParser::emitHorizontalRule(const BlockStyle& blockStyle) {
  if (partWordBufferIndex > 0) {
    flushPartWordBuffer();
  }

  if (currentTextBlock) {
    const BlockStyle parentBlockStyle = currentTextBlock->getBlockStyle();
    startNewTextBlock(parentBlockStyle);
  }

  if (!currentPage) {
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page for horizontal rule");
      return;
    }
    currentPageNextY = 0;
  }

  const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId) * lineCompression + 0.5f);
  const int16_t defaultVerticalSpacing = static_cast<int16_t>(lineHeight / 2);
  const int16_t topSpacing =
      static_cast<int16_t>((blockStyle.marginTop > 0 ? blockStyle.marginTop : defaultVerticalSpacing) +
                           (blockStyle.paddingTop > 0 ? blockStyle.paddingTop : 0));
  const int16_t bottomSpacing =
      static_cast<int16_t>((blockStyle.marginBottom > 0 ? blockStyle.marginBottom : defaultVerticalSpacing) +
                           (blockStyle.paddingBottom > 0 ? blockStyle.paddingBottom : 0));
  constexpr uint8_t ruleThickness = 2;
  const int16_t availableWidth =
      std::max<int16_t>(1, static_cast<int16_t>(viewportWidth - blockStyle.totalHorizontalInset()));
  const int16_t width = std::max<int16_t>(1, static_cast<int16_t>(availableWidth / 4));
  const int16_t xPos = static_cast<int16_t>(blockStyle.leftInset() + ((availableWidth - width) / 2));
  const int16_t totalHeight = static_cast<int16_t>(topSpacing + ruleThickness + bottomSpacing);

  if (!currentPage->elements.empty() && currentPageNextY + totalHeight > viewportHeight) {
    completePageFn(std::move(currentPage));
    completedPageCount++;
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page after horizontal-rule page break");
      return;
    }
    currentPageNextY = 0;
  }

  currentPageNextY += topSpacing;

  auto pageRule = std::shared_ptr<PageHorizontalRule>(
      new (std::nothrow) PageHorizontalRule(width, ruleThickness, xPos, currentPageNextY));
  if (!pageRule) {
    LOG_ERR("EHP", "Failed to create PageHorizontalRule");
    return;
  }
  currentPage->elements.push_back(pageRule);
  currentPageNextY = static_cast<int16_t>(currentPageNextY + ruleThickness + bottomSpacing);
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  // Images are removed for R1: skip the whole subtree (no placeholders).
  if (matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  if (matches(name, SKIP_TAGS, std::size(SKIP_TAGS))) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Skip blocks with role="doc-pagebreak" and epub:type="pagebreak"
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if ((strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0) ||
          (strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0)) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }

  if (strcmp(name, "hr") == 0) {
    // No CSS: default rule spacing only, left-aligned base style.
    BlockStyle hrBlockStyle;
    hrBlockStyle.alignment = CssTextAlign::Left;
    self->emitHorizontalRule(hrBlockStyle);
    self->depth += 1;
    return;
  }

  // Tables are not special-cased for R1: each cell simply starts a new
  // paragraph so the content still flows in reading order.
  if (isTableStructuralTag(name)) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    if (strcmp(name, "td") == 0 || strcmp(name, "th") == 0) {
      self->startNewTextBlock(self->blockStyleStack.back().withoutBottom());
    }
    self->nextWordContinues = false;
    self->depth += 1;
    return;
  }

  if (matches(name, HEADER_TAGS, std::size(HEADER_TAGS))) {
    BlockStyle headerBlockStyle;
    headerBlockStyle.textAlignDefined = true;
    headerBlockStyle.alignment = CssTextAlign::Center;
    const auto accumulated =
        self->blockStyleStack.back().getCombinedBlockStyle(headerBlockStyle, BlockStyle::CombineAxis::Horizontal);
    self->blockStyleStack.push_back(accumulated);
    self->startNewTextBlock(accumulated.withoutBottom());
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS))) {
    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      self->startNewTextBlock(self->blockStyleStack.back().withoutBottom());
    } else {
      // Without CSS the block carries only the user's default alignment.
      BlockStyle blockDefaults;
      blockDefaults.alignment = self->defaultAlignment;
      const auto accumulated =
          self->blockStyleStack.back().getCombinedBlockStyle(blockDefaults, BlockStyle::CombineAxis::Horizontal);
      self->blockStyleStack.push_back(accumulated);
      self->startNewTextBlock(accumulated.withoutBottom());
      self->updateEffectiveInlineStyle();

      if (strcmp(name, "li") == 0 && self->currentTextBlock) {
        self->currentTextBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR);
      }
    }
  } else if (matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasUnderline = true;
    entry.underline = true;
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BOLD_TAGS, std::size(BOLD_TAGS))) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    StyleStackEntry entry;
    entry.depth = self->depth;
    entry.hasBold = true;
    entry.bold = true;
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS))) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    StyleStackEntry entry;
    entry.depth = self->depth;
    entry.hasItalic = true;
    entry.italic = true;
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "sup") == 0 || strcmp(name, "sub") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    StyleStackEntry entry;
    entry.depth = self->depth;
    if (strcmp(name, "sup") == 0) {
      entry.hasSup = true;
      entry.sup = true;
    } else {
      entry.hasSub = true;
      entry.sub = true;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "a") == 0 && isNoterefAnchor(atts)) {
    // Footnote reference anchor: superscript it like <sup> so the marker reads
    // as a small raised number, not a full-size digit glued to the last word
    // (e.g. "Musk186"). The book's CSS did this via vertical-align, which the
    // slim parser doesn't see.
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    StyleStackEntry entry;
    entry.depth = self->depth;
    entry.hasSup = true;
    entry.sup = true;
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  }
  // All other tags (span, plain a, ...) are transparent inline wrappers here.

  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (self->parseFailed) return;

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // Glyph-coverage fold (TextFold.h): rewrite symbols the builtin font can't
  // draw (™, arrows, ♥ -> ASCII; Greek -> Latin; emoji dropped) BEFORE
  // tokenizing, so words + cached line-break widths match what renders instead
  // of a page of U+FFFD "?" boxes. Passthrough text is byte-identical, so the
  // no-break-space / BOM byte patterns the loop below keys on still match.
  std::string folded;
  int n = len;
  {
    bool mayNeed = false;
    for (int i = 0; i < len; i++) {
      if (static_cast<uint8_t>(s[i]) >= 0x80) {  // any non-ASCII: worth a fold pass
        mayNeed = true;
        break;
      }
    }
    if (mayNeed) {
      folded.reserve(static_cast<size_t>(len));
      const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
      const unsigned char* end = p + len;
      while (p < end) {
        const unsigned char* before = p;
        const uint32_t cp = utf8NextCodepoint(&p);
        if (p == before) {  // guard: malformed trailing byte, don't spin
          p = before + 1;
          continue;
        }
        appendFoldedCodepoint(cp, folded);
      }
      s = folded.c_str();
      n = static_cast<int>(folded.size());
    }
  }

  for (int i = 0; i < n; i++) {
    if (isWhitespace(s[i])) {
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        if (self->parseFailed) return;
      }
      // Whitespace is a real word boundary — reset continuation state
      self->nextWordContinues = false;
      // Skip the whitespace char
      continue;
    }

    // Detect U+00A0 (non-breaking space, UTF-8: 0xC2 0xA0) or
    //        U+202F (narrow no-break space, UTF-8: 0xE2 0x80 0xAF).
    //
    // Both are rendered as a visible space but must never allow a line break around them.
    // We split the no-break space into its own word token and link the surrounding words
    // with continuation flags so the layout engine treats them as an indivisible group.
    if (static_cast<uint8_t>(s[i]) == 0xC2 && i + 1 < n && static_cast<uint8_t>(s[i + 1]) == 0xA0) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->nextWordContinues = true;  // Attach space to previous word (no break).
      self->flushPartWordBuffer();

      self->nextWordContinues = true;  // Next real word attaches to this space (no break).

      i++;  // Skip the second byte (0xA0)
      continue;
    }

    // U+202F (narrow no-break space) — identical logic to U+00A0 above.
    if (static_cast<uint8_t>(s[i]) == 0xE2 && i + 2 < n && static_cast<uint8_t>(s[i + 1]) == 0x80 &&
        static_cast<uint8_t>(s[i + 2]) == 0xAF) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->nextWordContinues = true;
      self->flushPartWordBuffer();

      self->nextWordContinues = true;

      i += 2;  // Skip the remaining two bytes (0x80 0xAF)
      continue;
    }

    // Skip Zero Width No-Break Space / BOM (U+FEFF) = 0xEF 0xBB 0xBF
    const XML_Char FEFF_BYTE_1 = static_cast<XML_Char>(0xEF);
    const XML_Char FEFF_BYTE_2 = static_cast<XML_Char>(0xBB);
    const XML_Char FEFF_BYTE_3 = static_cast<XML_Char>(0xBF);

    if (s[i] == FEFF_BYTE_1) {
      // Check if the next two bytes complete the 3-byte sequence
      if ((i + 2 < n) && (s[i + 1] == FEFF_BYTE_2) && (s[i + 2] == FEFF_BYTE_3)) {
        i += 2;    // Skip the next two bytes
        continue;  // Move to the next iteration
      }
    }

    // If we're about to run out of space, then cut the word off and start a new one.
    // We must avoid splitting multi-byte UTF-8 sequences across word boundaries,
    // otherwise the trailing bytes become orphaned continuation bytes that the
    // decoder can't interpret.
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      int safeLen = utf8SafeTruncateBuffer(self->partWordBuffer, self->partWordBufferIndex);

      if (safeLen < self->partWordBufferIndex && safeLen > 0) {
        // Incomplete UTF-8 sequence at the end — save it before flushing
        int overflow = self->partWordBufferIndex - safeLen;
        char saved[4];
        for (int j = 0; j < overflow; j++) {
          saved[j] = self->partWordBuffer[safeLen + j];
        }
        self->partWordBufferIndex = safeLen;
        self->flushPartWordBuffer();
        for (int j = 0; j < overflow; j++) {
          self->partWordBuffer[j] = saved[j];
        }
        self->partWordBufferIndex = overflow;
      } else {
        self->flushPartWordBuffer();
      }
    }

    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }

  // If we have > 750 words buffered up, perform the layout and consume out all but the last line.
  // There should be enough here to build out 1-2 full pages and doing this will free up a lot of
  // memory (spotted on books with very long text blocks).
  if (self->currentTextBlock && self->currentTextBlock->size() > 750) {
    LOG_DBG("EHP", "Text block too long, splitting into multiple pages");
    const int horizontalInset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
    const uint16_t effectiveWidth = (horizontalInset < self->viewportWidth)
                                        ? static_cast<uint16_t>(self->viewportWidth - horizontalInset)
                                        : self->viewportWidth;
    self->currentTextBlock->layoutAndExtractLines(
        self->renderer, self->fontId, effectiveWidth,
        [self](const std::shared_ptr<TextBlock>& textBlock) { self->addLineToPage(textBlock); }, false);
  }
}

void XMLCALL ChapterHtmlSlimParser::defaultHandlerExpand(void* userData, const XML_Char* s, const int len) {
  // Check if this looks like an entity reference (&...;)
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* utf8Value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (utf8Value != nullptr) {
      // Known entity: expand to its UTF-8 value
      characterData(userData, utf8Value, strlen(utf8Value));
      return;
    }
    // Unknown entity: preserve original &...; sequence
    characterData(userData, s, len);
    return;
  }
  // Not an entity we recognize - skip it
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      !self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth - 1;
  const bool willClearBold = self->boldUntilDepth == self->depth - 1;
  const bool willClearItalic = self->italicUntilDepth == self->depth - 1;
  const bool willClearUnderline = self->underlineUntilDepth == self->depth - 1;

  const bool styleWillChange = willPopStyleStack || willClearBold || willClearItalic || willClearUnderline;
  const bool headerOrBlockTag = isHeaderOrBlock(name);
  const bool tableStructuralTag = isTableStructuralTag(name);

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag = !headerOrBlockTag && !tableStructuralTag && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || matches(name, BOLD_TAGS, std::size(BOLD_TAGS)) ||
                             matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS)) ||
                             matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS)) || tableStructuralTag ||
                             self->depth == 1;

    if (shouldFlush) {
      self->flushPartWordBuffer();
      // If closing an inline element, the next word fragment continues the same visual word
      if (isInlineTag) {
        self->nextWordContinues = true;
      }
    }
  }

  self->depth -= 1;

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  if (tableStructuralTag) {
    self->nextWordContinues = false;
  }

  // Leaving bold tag
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic tag
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }

  // Leaving underline tag
  if (self->underlineUntilDepth == self->depth) {
    self->underlineUntilDepth = INT_MAX;
  }

  // Pop from inline style stack if we pushed an entry at this depth
  if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth) {
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();
  }

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag) {
    // br is self-closing and not a container — it doesn't push/pop the stack.
    if (strcmp(name, "br") != 0 && self->blockStyleStack.size() > 1) {
      // Apply closing element's bottom margin to the current text block so
      // container spacing appears after the element's content (on the last child),
      // not on the first child via the empty-block merge in startNewTextBlock.
      if (self->currentTextBlock) {
        const auto style = self->currentTextBlock->getBlockStyle();
        self->currentTextBlock->setBlockStyle(style.addBottom(self->blockStyleStack.back()));
      }
      self->blockStyleStack.pop_back();
    }
  }
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  // Initialize block style stack with a root entry representing "no ancestor block elements".
  // The user's paragraph alignment is set as the default so child elements without explicit
  // text-align inherit it correctly through getCombinedBlockStyle.
  defaultAlignment = (paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                         ? CssTextAlign::Justify
                         : static_cast<CssTextAlign>(paragraphAlignment);
  BlockStyle rootBlockStyle;
  rootBlockStyle.alignment = defaultAlignment;
  blockStyleStack.clear();
  blockStyleStack.reserve(8);
  blockStyleStack.push_back(rootBlockStyle);

  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  paragraphAlignmentBlockStyle.alignment = defaultAlignment;
  startNewTextBlock(paragraphAlignmentBlockStyle);

  XML_Parser parser = XML_ParserCreate(nullptr);
  int done;

  if (!parser) {
    LOG_ERR("EHP", "Couldn't allocate memory for parser");
    return false;
  }

  // Handle HTML entities (like &nbsp;) that aren't in XML spec or DTD
  // Using DefaultHandlerExpand preserves normal entity expansion from DOCTYPE
  XML_SetDefaultHandlerExpand(parser, defaultHandlerExpand);

  HalFile file;
  if (!Storage.openFileForRead("EHP", filepath, file)) {
    destroyXmlParser(parser);
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  // Compute the time taken to parse and build pages
  const uint32_t chapterStartTime = millis();
  do {
    void* const buf = XML_GetBuffer(parser, PARSE_BUFFER_SIZE);
    if (!buf) {
      LOG_ERR("EHP", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      file.close();
      return false;
    }

    const size_t len = file.read(buf, PARSE_BUFFER_SIZE);

    if (len == 0 && file.available() > 0) {
      LOG_ERR("EHP", "File read error");
      destroyXmlParser(parser);
      file.close();
      return false;
    }

    done = file.available() == 0;

    if (XML_ParseBuffer(parser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      LOG_ERR("EHP", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      file.close();
      return false;
    }
    if (parseFailed) {
      LOG_ERR("EHP", "Parse aborted after recoverable memory pressure");
      destroyXmlParser(parser);
      file.close();
      return false;
    }
  } while (!done);
  LOG_DBG("EHP", "Time to parse and build pages: %lu ms", millis() - chapterStartTime);

  destroyXmlParser(parser);
  file.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    makePages();
    if (currentPage) {
      completePageFn(std::move(currentPage));
      completedPageCount++;
    }
    currentPage.reset();
    currentTextBlock.reset();
  }

  if (parseFailed) {
    return false;
  }

  return true;
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line) {
  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  if (!currentPage) {
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page");
      parseFailed = true;
      return;
    }
    currentPageNextY = 0;
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    completePageFn(std::move(currentPage));
    completedPageCount++;
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page");
      parseFailed = true;
      return;
    }
    currentPageNextY = 0;
  }

  // Apply horizontal left inset (margin + padding) as x position offset
  const int16_t xOffset = line->getBlockStyle().leftInset();
  currentPage->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY));
  currentPageNextY += lineHeight;
}

void ChapterHtmlSlimParser::makePages() {
  if (!currentTextBlock) {
    LOG_ERR("EHP", "!! No text block to make pages for !!");
    return;
  }

  if (!currentPage) {
    currentPage.reset(new (std::nothrow) Page());
    if (!currentPage) {
      LOG_ERR("EHP", "Failed to create page");
      parseFailed = true;
      return;
    }
    currentPageNextY = 0;
  }

  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  // Apply top spacing before the paragraph (stored in pixels)
  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  if (blockStyle.marginTop > 0) {
    currentPageNextY += blockStyle.marginTop;
  }
  if (blockStyle.paddingTop > 0) {
    currentPageNextY += blockStyle.paddingTop;
  }

  // Calculate effective width accounting for horizontal margins/padding
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  currentTextBlock->layoutAndExtractLines(
      renderer, fontId, effectiveWidth,
      [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); });

  // Apply bottom spacing after the paragraph (stored in pixels)
  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  // Extra paragraph spacing if enabled (default behavior)
  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}

}  // namespace reader
