// Pruned from x4-os lib/Epub/Epub/Section.h.
// Removed: anchor map, paragraph LUT, li LUT lookups (KOReader-sync support),
// getTextFromSectionFile, the indexing-popup callback, GfxRenderer coupling
// (TextMeasure instead). The section.bin v27 header keeps its exact byte
// layout — the anchor-map / paragraph-LUT / li-LUT offset fields are written
// as zeros since the pruned engine no longer computes them.
#pragma once

#include <HalStorage.h>

#include <memory>
#include <optional>
#include <string>

#include "Epub.h"
#include "ReaderSettings.h"
#include "TextMeasure.h"

namespace reader {

class Page;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  const TextMeasure& measure;
  std::string filePath;
  HalFile file;

  void writeSectionFileHeader(const ReaderSettings& settings);
  uint32_t onPageComplete(std::unique_ptr<Page> page);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, const TextMeasure& measure)
      : epub(epub),
        spineIndex(spineIndex),
        measure(measure),
        filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}
  ~Section() = default;
  // Open an existing section cache and validate its settings header against
  // `settings`; on mismatch/corruption the cache is cleared and false returned.
  bool loadSectionFile(const ReaderSettings& settings);
  bool clearCache() const;
  // Parse + layout the chapter and stream positioned pages to the cache file.
  bool createSectionFile(const ReaderSettings& settings);
  std::unique_ptr<Page> loadPageFromSectionFile();

  // Get the page count from the section cache file without fully loading it.
  std::optional<uint16_t> getCachedPageCount() const;
};

}  // namespace reader
