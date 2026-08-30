// Pruned from x4-os lib/Epub/Epub/BookMetadataCache.h.
// Removed: the >=400-entry large-manifest FNV spine-href index (spine-linear
// R1 books don't need it; TOC parsing is gone anyway). The book.bin v8 on-disk
// format is preserved exactly: version, LUT offset, spine+TOC counts, core
// metadata strings, spine LUT, TOC LUT, spine entries, TOC entries. This
// engine always writes tocCount=0 with an empty TOC LUT (slots stay zeroed).
#pragma once

#include <HalStorage.h>

#include <string>

namespace reader {

class BookMetadataCache {
 public:
  struct BookMetadata {
    std::string title;
    std::string author;
    std::string language;
    std::string coverItemHref;      // unused in R1; kept so book.bin v8 layout is unchanged
    std::string textReferenceHref;  // guide "text" reference (start-of-content)
  };

  struct SpineEntry {
    std::string href;
    uint32_t cumulativeSize;
    int16_t tocIndex;

    SpineEntry() : cumulativeSize(0), tocIndex(-1) {}
    SpineEntry(std::string href, const uint32_t cumulativeSize, const int16_t tocIndex)
        : href(std::move(href)), cumulativeSize(cumulativeSize), tocIndex(tocIndex) {}
  };

  struct TocEntry {
    std::string title;
    std::string href;
    std::string anchor;
    uint8_t level;
    int16_t spineIndex;

    TocEntry() : level(0), spineIndex(-1) {}
  };

 private:
  std::string cachePath;
  uint32_t lutOffset;
  uint16_t spineCount;
  uint16_t tocCount;
  bool loaded;
  bool buildMode;

  HalFile bookFile;
  // Temp file handles during build
  HalFile spineFile;
  HalFile tocFile;

  uint32_t writeSpineEntry(HalFile& file, const SpineEntry& entry) const;
  uint32_t writeTocEntry(HalFile& file, const TocEntry& entry) const;
  SpineEntry readSpineEntry(HalFile& file) const;
  TocEntry readTocEntry(HalFile& file) const;

 public:
  BookMetadata coreMetadata;

  explicit BookMetadataCache(std::string cachePath)
      : cachePath(std::move(cachePath)), lutOffset(0), spineCount(0), tocCount(0), loaded(false), buildMode(false) {}
  ~BookMetadataCache() = default;

  // Building phase (stream to disk immediately)
  bool beginWrite();
  bool beginContentOpfPass();
  void createSpineEntry(const std::string& href);
  // Appends one TOC entry, resolving its href to a spine index.
  void createTocEntry(const std::string& title, const std::string& href, const std::string& anchor, uint8_t level);
  bool endContentOpfPass();
  // TOC parsing is removed for R1 (spine-linear). The TOC pass still runs so
  // the empty toc.bin.tmp exists and book.bin's v8 TOC slots stay zeroed.
  bool beginTocPass();
  bool endTocPass();
  bool endWrite();
  bool cleanupTmpFiles() const;

  // Post-processing to update mappings and sizes
  bool buildBookBin(const std::string& epubPath, const BookMetadata& metadata);

  // Reading phase (read mode)
  bool load();
  SpineEntry getSpineEntry(int index);
  TocEntry getTocEntry(int index);
  int getSpineCount() const { return spineCount; }
  int getTocCount() const { return tocCount; }
  bool isLoaded() const { return loaded; }
};

}  // namespace reader
