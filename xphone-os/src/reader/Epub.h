// Pruned from x4-os lib/Epub.h. Removed: CSS engine (CssParser, css file
// discovery/parsing), cover/thumbnail BMP generation, TOC parsing and TOC
// lookups (spine-linear R1), href->spine resolution (KOReader sync),
// readItemContentsToBytes (only used by the cover fallback).
// Kept: book.bin v8 build/load, spine access, zip streaming, progress math.
// R2a adds cover-href discovery (sidecar <cachePath>/cover.href; book.bin v8
// stays byte-identical — its coverItemHref slot is still an empty string).
#pragma once

#include <Print.h>

#include <functional>
#include <memory>
#include <string>

#include "BookMetadataCache.h"
#include "ReaderSettings.h"

namespace reader {

class Epub {
  // where is the EPUB file?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Unique cache key based on filepath
  std::string cachePath;
  // Spine cache (book.bin v8; TOC slots stay zeroed)
  std::unique_ptr<BookMetadataCache> bookMetadataCache;

  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata);
  // Cover-only content.opf re-parse (for books whose cache predates the
  // cover.href sidecar). Does not touch bookMetadataCache.
  bool parseCoverHrefFromOpf(std::string* coverHref) const;
  void writeCoverSidecar(const std::string& coverHref) const;

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir = kReaderCacheRoot)
      : filepath(std::move(filepath)) {
    // create a cache key based on the filepath (CrossPoint scheme, own root)
    cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true);
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  // Cover image href, from the <cachePath>/cover.href sidecar written during
  // build. Empty sidecar = "book has no cover" (returns false, no rescan). A
  // MISSING sidecar (cache built before this feature) triggers a one-off
  // cover-only re-parse of content.opf and writes the sidecar. Returns true
  // and fills *href only when the book has a cover.
  bool getCoverHref(std::string* href) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  int getSpineItemsCount() const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
};

}  // namespace reader
