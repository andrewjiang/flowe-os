// Pruned from x4-os lib/Epub.cpp (928 LOC -> ~290). See Epub.h.
#include "Epub.h"

#include <HalStorage.h>
#include <Utf8.h>
#include <ZipFile.h>

#include "ContainerParser.h"
#include "ContentOpfParser.h"
#include "TocNavParser.h"
#include "TocNcxParser.h"
#include "ReaderFsUtils.h"
#include "ReaderLog.h"

namespace reader {

bool Epub::findContentOpfFile(std::string* contentOpfFile) const {
  const auto containerPath = "META-INF/container.xml";
  size_t containerSize;

  // Get file size without loading it all into heap
  if (!getItemSize(containerPath, &containerSize)) {
    LOG_ERR("EBP", "Could not find or size META-INF/container.xml");
    return false;
  }

  ContainerParser containerParser(containerSize);

  if (!containerParser.setup()) {
    return false;
  }

  // Stream read
  if (!readItemContentsToStream(containerPath, containerParser, 512)) {
    LOG_ERR("EBP", "Could not read META-INF/container.xml");
    return false;
  }

  // Extract the result
  if (containerParser.fullPath.empty()) {
    LOG_ERR("EBP", "Could not find valid rootfile in container.xml");
    return false;
  }

  *contentOpfFile = std::move(containerParser.fullPath);
  return true;
}

bool Epub::parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata) {
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    LOG_ERR("EBP", "Could not find content.opf in zip");
    return false;
  }

  contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  LOG_DBG("EBP", "Parsing content.opf: %s", contentOpfFilePath.c_str());

  size_t contentOpfSize;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    LOG_ERR("EBP", "Could not get size of content.opf");
    return false;
  }

  ContentOpfParser opfParser(getCachePath(), contentBasePath, contentOpfSize, bookMetadataCache.get());
  if (!opfParser.setup()) {
    LOG_ERR("EBP", "Could not setup content.opf parser");
    return false;
  }

  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024)) {
    LOG_ERR("EBP", "Could not read content.opf");
    return false;
  }

  // Grab data from opfParser into epub. Normalize titles to NFC so NFD (combining
  // mark) text renders correctly — the device fonts have no mark positioning.
  bookMetadata.title = utf8ComposeNfc(opfParser.title);
  bookMetadata.author = opfParser.author;
  bookMetadata.language = opfParser.language;
  bookMetadata.textReferenceHref = opfParser.textReferenceHref;
  tocNcxItem = opfParser.tocNcxHref;
  tocNavItem = opfParser.tocNavHref;
  // coverItemHref intentionally left empty so book.bin v8 stays byte-identical
  // to R1 output; the cover href lives in the cover.href sidecar instead.
  writeCoverSidecar(opfParser.coverHref);

  LOG_DBG("EBP", "Successfully parsed content.opf");
  return true;
}

// Cover-only content.opf pass for caches that predate the cover.href sidecar.
// Passes a null BookMetadataCache so the parser skips spine resolution.
bool Epub::parseCoverHrefFromOpf(std::string* coverHref) const {
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    LOG_ERR("EBP", "Could not find content.opf for cover re-parse");
    return false;
  }

  const std::string basePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  size_t contentOpfSize;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    return false;
  }

  ContentOpfParser opfParser(getCachePath(), basePath, contentOpfSize, nullptr);
  if (!opfParser.setup()) {
    return false;
  }
  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024)) {
    return false;
  }

  *coverHref = opfParser.coverHref;
  return true;
}

// Sidecar write; an empty href writes an empty file, which getCoverHref reads
// as "no cover, don't rescan".
void Epub::writeCoverSidecar(const std::string& coverHref) const {
  const std::string sidecarPath = cachePath + "/cover.href";
  HalFile f;
  if (!Storage.openFileForWrite("EBP", sidecarPath, f)) {
    LOG_ERR("EBP", "Could not write cover sidecar: %s", sidecarPath.c_str());
    return;
  }
  if (!coverHref.empty()) {
    f.write(reinterpret_cast<const uint8_t*>(coverHref.data()), coverHref.size());
  }
  f.flush();
}

bool Epub::getCoverHref(std::string* href) const {
  const std::string sidecarPath = cachePath + "/cover.href";

  if (Storage.exists(sidecarPath.c_str())) {
    HalFile f;
    if (!Storage.openFileForRead("EBP", sidecarPath, f)) {
      return false;
    }
    const size_t len = f.size();
    if (len == 0) {
      return false;  // built with no cover — don't rescan
    }
    std::string sidecarHref(len, '\0');
    if (f.read(&sidecarHref[0], len) != static_cast<int>(len)) {
      LOG_ERR("EBP", "Short read on cover sidecar: %s", sidecarPath.c_str());
      return false;
    }
    *href = std::move(sidecarHref);
    return true;
  }

  // Sidecar missing: cache predates the cover feature. Re-parse content.opf
  // for the cover only and persist the answer (even a "no cover" one).
  LOG_DBG("EBP", "No cover sidecar, re-parsing content.opf for cover");
  setupCacheDir();
  std::string coverHref;
  if (!parseCoverHrefFromOpf(&coverHref)) {
    return false;  // parse/IO failure: leave the sidecar missing so a retry can succeed
  }
  writeCoverSidecar(coverHref);
  if (coverHref.empty()) {
    return false;
  }
  *href = std::move(coverHref);
  return true;
}

// load in the meta data for the epub file
bool Epub::load(const bool buildIfMissing) {
  LOG_DBG("EBP", "Loading ePub: %s", filepath.c_str());

  // Initialize spine cache
  bookMetadataCache.reset(new (std::nothrow) BookMetadataCache(cachePath));
  if (!bookMetadataCache) {
    LOG_ERR("EBP", "OOM: BookMetadataCache");
    return false;
  }

  // Try to load existing cache first
  if (bookMetadataCache->load()) {
    LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
    return true;
  }

  // If we didn't load from cache above and we aren't allowed to build, fail now
  if (!buildIfMissing) {
    return false;
  }

  // Cache doesn't exist or is invalid, build it
  LOG_DBG("EBP", "Cache not found, building spine cache");
  setupCacheDir();

  const uint32_t indexingStart = millis();

  // Begin building cache - stream entries to disk immediately
  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("EBP", "Could not begin writing cache");
    return false;
  }

  // OPF Pass
  const uint32_t opfStart = millis();
  BookMetadataCache::BookMetadata bookMetadata;
  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR("EBP", "Could not begin writing content.opf pass");
    return false;
  }
  if (!parseContentOpf(bookMetadata)) {
    LOG_ERR("EBP", "Could not parse content.opf");
    return false;
  }
  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR("EBP", "Could not end writing content.opf pass");
    return false;
  }
  LOG_DBG("EBP", "OPF pass completed in %lu ms", millis() - opfStart);

  // TOC Pass. A spine lists every file in reading order — cover, title page,
  // copyright, then chapters — so numbering it gave "Chapter 6" for the text's
  // chapter one (#42). The book's real chapter names live in its declared TOC
  // document, which is what every other reader shows. Either flavour is fine;
  // a book with neither keeps an empty TOC and the reader falls back to
  // numbered sections.
  const uint32_t tocStart = millis();
  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR("EBP", "Could not begin writing toc pass");
    return false;
  }
  bool tocParsed = false;
  if (!tocNcxItem.empty()) tocParsed = parseTocNcxFile();
  if (!tocParsed && !tocNavItem.empty()) tocParsed = parseTocNavFile();
  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR("EBP", "Could not end writing toc pass");
    return false;
  }
  LOG_DBG("EBP", "TOC pass completed in %lu ms (%d entries)", millis() - tocStart,
          bookMetadataCache->getTocCount());

  // Close the cache files
  if (!bookMetadataCache->endWrite()) {
    LOG_ERR("EBP", "Could not end writing cache");
    return false;
  }

  // Build final book.bin
  const uint32_t buildStart = millis();
  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata)) {
    LOG_ERR("EBP", "Could not update mappings and sizes");
    return false;
  }
  LOG_DBG("EBP", "buildBookBin completed in %lu ms", millis() - buildStart);
  LOG_DBG("EBP", "Total indexing completed in %lu ms", millis() - indexingStart);

  if (!bookMetadataCache->cleanupTmpFiles()) {
    LOG_DBG("EBP", "Could not cleanup tmp files - ignoring");
  }

  // Reload the cache from disk so it's in the correct state
  bookMetadataCache.reset(new (std::nothrow) BookMetadataCache(cachePath));
  if (!bookMetadataCache || !bookMetadataCache->load()) {
    LOG_ERR("EBP", "Failed to reload cache after writing");
    return false;
  }

  LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
  return true;
}

bool Epub::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("EBP", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("EBP", "Failed to clear cache");
    return false;
  }

  LOG_DBG("EBP", "Cache cleared successfully");
  return true;
}

void Epub::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  // pFlag default creates parent dirs, so /.xphone is created too.
  Storage.mkdir(cachePath.c_str());
}

const std::string& Epub::getCachePath() const { return cachePath; }

const std::string& Epub::getPath() const { return filepath; }

const std::string& Epub::getTitle() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.title;
}

const std::string& Epub::getAuthor() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.author;
}

const std::string& Epub::getLanguage() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.language;
}

// Restored with the TOC parsers (dropped in 622fc5f). The TOC document is
// extracted from the zip to a temp file first: expat needs a forward-only
// stream and the zip reader is happier writing whole files than seeking.
bool Epub::parseTocNcxFile() const {
  if (tocNcxItem.empty()) return false;
  LOG_DBG("EBP", "Parsing toc ncx: %s", tocNcxItem.c_str());

  const auto tmpPath = getCachePath() + "/toc.ncx.tmp";
  HalFile tmp;
  if (!Storage.openFileForWrite("EBP", tmpPath, tmp)) return false;
  readItemContentsToStream(tocNcxItem, tmp, 1024);
  tmp.close();
  if (!Storage.openFileForRead("EBP", tmpPath, tmp)) return false;

  TocNcxParser ncxParser(contentBasePath, tmp.size(), bookMetadataCache.get());
  bool ok = ncxParser.setup();
  if (ok) {
    const auto buf = static_cast<uint8_t*>(malloc(1024));
    if (!buf) {
      LOG_ERR("EBP", "No memory for toc ncx parser");
      ok = false;
    } else {
      while (tmp.available()) {
        const auto readSize = tmp.read(buf, 1024);
        if (readSize == 0) break;
        if (ncxParser.write(buf, readSize) != readSize) {
          LOG_ERR("EBP", "Could not process all toc ncx data");
          ok = false;
          break;
        }
      }
      free(buf);
    }
  }
  tmp.close();
  Storage.remove(tmpPath.c_str());
  return ok && bookMetadataCache->getTocCount() > 0;
}

// EPUB 3 books carry the same information as an XHTML <nav> document.
bool Epub::parseTocNavFile() const {
  if (tocNavItem.empty()) return false;
  LOG_DBG("EBP", "Parsing toc nav: %s", tocNavItem.c_str());

  const auto tmpPath = getCachePath() + "/toc.nav.tmp";
  HalFile tmp;
  if (!Storage.openFileForWrite("EBP", tmpPath, tmp)) return false;
  readItemContentsToStream(tocNavItem, tmp, 1024);
  tmp.close();
  if (!Storage.openFileForRead("EBP", tmpPath, tmp)) return false;

  TocNavParser navParser(contentBasePath, tmp.size(), bookMetadataCache.get());
  bool ok = navParser.setup();
  if (ok) {
    const auto buf = static_cast<uint8_t*>(malloc(1024));
    if (!buf) {
      LOG_ERR("EBP", "No memory for toc nav parser");
      ok = false;
    } else {
      while (tmp.available()) {
        const auto readSize = tmp.read(buf, 1024);
        if (readSize == 0) break;
        if (navParser.write(buf, readSize) != readSize) {
          LOG_ERR("EBP", "Could not process all toc nav data");
          ok = false;
          break;
        }
      }
      free(buf);
    }
  }
  tmp.close();
  Storage.remove(tmpPath.c_str());
  return ok && bookMetadataCache->getTocCount() > 0;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return false;
  }

  const std::string path = fsutil::normalisePath(itemHref);
  return ZipFile(filepath).readFileToStream(path.c_str(), out, chunkSize);
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const std::string path = fsutil::normalisePath(itemHref);
  return ZipFile(filepath).getInflatedFileSize(path.c_str(), size);
}

int Epub::getSpineItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }
  return bookMetadataCache->getSpineCount();
}

int Epub::getTocItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return 0;
  return bookMetadataCache->getTocCount();
}

BookMetadataCache::TocEntry Epub::getTocItem(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return {};
  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) return {};
  return bookMetadataCache->getTocEntry(tocIndex);
}

int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  return getTocItem(tocIndex).spineIndex;
}

// The chapter a spine position sits inside: the last TOC entry that opens at
// or before it. TOC entries are in document order, and several can share one
// spine file (a book split by chapter has one file per entry; a book in one
// big file has many entries pointing at spine 0), so this is a scan for the
// greatest spineIndex <= the target rather than an exact match.
int Epub::getTocIndexForSpineIndex(const int spineIndex) const {
  const int n = getTocItemsCount();
  int best = -1;
  for (int i = 0; i < n; i++) {
    const int si = getTocItem(i).spineIndex;
    if (si >= 0 && si <= spineIndex) best = i;
    else if (si > spineIndex) break;
  }
  return best;
}

size_t Epub::getCumulativeSpineItemSize(const int spineIndex) const { return getSpineItem(spineIndex).cumulativeSize; }

BookMetadataCache::SpineEntry Epub::getSpineItem(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineItem called but cache not loaded");
    return {};
  }

  if (spineIndex < 0 || spineIndex >= bookMetadataCache->getSpineCount()) {
    LOG_ERR("EBP", "getSpineItem index:%d is out of range", spineIndex);
    return bookMetadataCache->getSpineEntry(0);
  }

  return bookMetadataCache->getSpineEntry(spineIndex);
}

size_t Epub::getBookSize() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || bookMetadataCache->getSpineCount() == 0) {
    return 0;
  }
  return getCumulativeSpineItemSize(getSpineItemsCount() - 1);
}

int Epub::getSpineIndexForTextReference() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTextReference called but cache not loaded");
    return 0;
  }

  if (bookMetadataCache->coreMetadata.textReferenceHref.empty()) {
    // there was no textReference in epub, so we return 0 (the first chapter)
    return 0;
  }

  // loop through spine items to get the correct index matching the text href
  for (int i = 0; i < getSpineItemsCount(); i++) {
    if (getSpineItem(i).href == bookMetadataCache->coreMetadata.textReferenceHref) {
      LOG_DBG("EBP", "Text reference %s found at index %d", bookMetadataCache->coreMetadata.textReferenceHref.c_str(),
              i);
      return i;
    }
  }
  // This should not happen, as we checked for empty textReferenceHref earlier
  LOG_DBG("EBP", "Section not found for text reference");
  return 0;
}

// Calculate progress in book (returns 0.0-1.0)
float Epub::calculateProgress(const int currentSpineIndex, const float currentSpineRead) const {
  const size_t bookSize = getBookSize();
  if (bookSize == 0) {
    return 0.0f;
  }
  const size_t prevChapterSize = (currentSpineIndex >= 1) ? getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t curChapterSize = getCumulativeSpineItemSize(currentSpineIndex) - prevChapterSize;
  const float sectionProgSize = currentSpineRead * static_cast<float>(curChapterSize);
  const float totalProgress = static_cast<float>(prevChapterSize) + sectionProgSize;
  return totalProgress / static_cast<float>(bookSize);
}

}  // namespace reader
