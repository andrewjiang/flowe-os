#pragma once
#include <cstdint>

namespace reader {

// Root of the reader cache on SD. Mirrors CrossPoint's `.crosspoint/epub_<hash>`
// scheme (std::hash<std::string> of the book filepath) under our own namespace:
//   /.xphone/epub_<hash>/{book.bin, progress.bin, sections/<spine>.bin}
constexpr const char* kReaderCacheRoot = "/.xphone";

// Layout inputs. These are exactly the values written into the section.bin v27
// header (settings-in-header is the cache key): any change invalidates and
// rebuilds section caches — zero extra invalidation bookkeeping.
//
// Resolution-agnostic: the viewport always comes from these parameters, never
// from a display constant. Defaults are X3 logical portrait (528x792) minus a
// 24 px margin on every side.
struct ReaderSettings {
  // Cache-key font id: 0/1/2 => notoserif 12/14/16 pt (see ReaderFonts.h).
  // Serialized as int to match the v27 header field width.
  int fontId = 1;
  float lineCompression = 1.0f;      // CrossPoint "Normal" line spacing
  bool extraParagraphSpacing = false;
  // Matches CrossPoint PARAGRAPH_ALIGNMENT / CssTextAlign: 0 = Justify.
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 528 - 2 * 24;   // 480
  uint16_t viewportHeight = 792 - 2 * 24;  // 744

  // Pruned-feature header fields. Still serialized so the on-disk layout stays
  // v27-compatible; the pruned engine never computes them.
  static constexpr bool kHyphenationEnabled = false;   // R2: Liang hyphenation
  static constexpr bool kEmbeddedStyle = false;        // CSS engine removed
  static constexpr uint8_t kImageRendering = 2;        // 2 = images suppressed
  static constexpr bool kFocusReadingEnabled = false;  // focus reading removed

  static ReaderSettings xphoneDefaults() { return ReaderSettings{}; }
};

}  // namespace reader
