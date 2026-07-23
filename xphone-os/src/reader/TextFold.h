#pragma once

// Reader glyph-coverage fold. The builtin Noto serif family (lib/EpdFontCore)
// covers ASCII, Latin-1 + Latin Extended, Cyrillic, General Punctuation (smart
// quotes, en/em dashes, ellipsis, bullet), super/subscripts, currency and the
// common ligatures. Any codepoint outside that set renders as the U+FFFD box,
// which reads on-screen as a "?" — so books that use trademark/arrow/heart
// symbols (e.g. The Book of Elon) or inline classical Greek (Meditations) come
// out peppered with boxes.
//
// This fold runs once at parse time (so section.bin caches the renderable text
// and line-break widths are measured on what actually draws): it remaps the
// common offending symbols to ASCII, romanizes Greek to Latin, and drops emoji
// / variation selectors (a gap beats a box). Everything the font already covers
// passes through byte-identical.

#include <cstdint>
#include <string>

namespace reader {

// Append the font-renderable form of `cp` to `out`. May append zero bytes
// (dropped), one codepoint (passthrough / single remap) or several ASCII bytes
// (e.g. U+2122 -> "(TM)", a Greek letter -> "th").
void appendFoldedCodepoint(uint32_t cp, std::string& out);

// True if folding `cp` would change it (i.e. it is not a plain passthrough).
// Lets the caller keep its fast path for the overwhelmingly common ASCII/Latin
// text and only allocate/branch when a codepoint actually needs folding.
bool codepointNeedsFold(uint32_t cp);

}  // namespace reader
