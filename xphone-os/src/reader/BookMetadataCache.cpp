// Pruned from x4-os lib/Epub/Epub/BookMetadataCache.cpp (460 LOC -> ~290).
// Removed: large-manifest (>=400 spine entries) FNV index + ZipFile batch size
// lookup, createTocEntry (no TOC parsers in R1). book.bin v8 format unchanged.
#include "BookMetadataCache.h"

#include <Serialization.h>
#include <Utf8.h>   // utf8ComposeNfc for TOC titles
#include <ZipFile.h>

#include "ReaderFsUtils.h"
#include "ReaderLog.h"

namespace reader {

namespace {
// v9: the TOC is actually populated. v8 caches were all written with
// tocCount=0 (TOC parsing was pruned), so a book indexed before this build
// would keep showing numbered sections forever. Bumping forces one silent
// re-index per book on next open — the "Indexing..." screen users already
// know — after which chapter names appear. Nothing else in the layout moved.
constexpr uint8_t BOOK_CACHE_VERSION = 9;
constexpr char bookBinFile[] = "/book.bin";
constexpr char tmpSpineBinFile[] = "/spine.bin.tmp";
constexpr char tmpTocBinFile[] = "/toc.bin.tmp";
}  // namespace

/* ============= WRITING / BUILDING FUNCTIONS ================ */

bool BookMetadataCache::beginWrite() {
  buildMode = true;
  spineCount = 0;
  tocCount = 0;
  LOG_DBG("BMC", "Entering write mode");
  return true;
}

bool BookMetadataCache::beginContentOpfPass() {
  LOG_DBG("BMC", "Beginning content opf pass");

  // Open spine file for writing
  return Storage.openFileForWrite("BMC", cachePath + tmpSpineBinFile, spineFile);
}

bool BookMetadataCache::endContentOpfPass() {
  // Explicit close() required: member variable persists beyond function scope
  spineFile.close();
  return true;
}

bool BookMetadataCache::beginTocPass() {
  LOG_DBG("BMC", "Beginning toc pass");

  // createTocEntry resolves each entry's href against the spine written in the
  // previous pass, and endContentOpfPass() closed that file — so reopen it for
  // READING here. (A book with no TOC simply never calls createTocEntry and
  // the file is closed again below, unread.)
  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    LOG_ERR("BMC", "Could not reopen spine file for the toc pass");
    return false;
  }
  return Storage.openFileForWrite("BMC", cachePath + tmpTocBinFile, tocFile);
}

bool BookMetadataCache::endTocPass() {
  // Explicit close() required: member variable persists beyond function scope
  spineFile.close();  // reopened read-only by beginTocPass for href resolution
  tocFile.close();
  return true;
}

bool BookMetadataCache::endWrite() {
  if (!buildMode) {
    LOG_DBG("BMC", "endWrite called but not in build mode");
    return false;
  }

  buildMode = false;
  LOG_DBG("BMC", "Wrote %d spine, %d TOC entries", spineCount, tocCount);
  return true;
}

bool BookMetadataCache::buildBookBin(const std::string& epubPath, const BookMetadata& metadata) {
  // Open all three files, writing to meta, reading from spine and toc
  if (!Storage.openFileForWrite("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    // Explicit close() required: member variable persists beyond function scope
    bookFile.close();
    return false;
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpTocBinFile, tocFile)) {
    // Explicit close() required: member variables persist beyond function scope
    bookFile.close();
    spineFile.close();
    return false;
  }

  constexpr uint32_t headerASize =
      sizeof(BOOK_CACHE_VERSION) + /* LUT Offset */ sizeof(uint32_t) + sizeof(spineCount) + sizeof(tocCount);
  const uint32_t metadataSize = metadata.title.size() + metadata.author.size() + metadata.language.size() +
                                metadata.coverItemHref.size() + metadata.textReferenceHref.size() +
                                sizeof(uint32_t) * 5;
  const uint32_t lutSize = sizeof(uint32_t) * spineCount + sizeof(uint32_t) * tocCount;
  const uint32_t lutOffset = headerASize + metadataSize;

  // Header A
  serialization::writePod(bookFile, BOOK_CACHE_VERSION);
  serialization::writePod(bookFile, lutOffset);
  serialization::writePod(bookFile, spineCount);
  serialization::writePod(bookFile, tocCount);
  // Metadata
  serialization::writeString(bookFile, metadata.title);
  serialization::writeString(bookFile, metadata.author);
  serialization::writeString(bookFile, metadata.language);
  serialization::writeString(bookFile, metadata.coverItemHref);
  serialization::writeString(bookFile, metadata.textReferenceHref);

  // Loop through spine entries, writing LUT positions
  spineFile.seek(0);
  for (int i = 0; i < spineCount; i++) {
    uint32_t pos = spineFile.position();
    readSpineEntry(spineFile);
    serialization::writePod(bookFile, pos + lutOffset + lutSize);
  }

  // Loop through toc entries, writing LUT positions (tocCount is 0 in R1;
  // loop kept so the v8 layout stays byte-for-byte compatible)
  tocFile.seek(0);
  for (int i = 0; i < tocCount; i++) {
    uint32_t pos = tocFile.position();
    readTocEntry(tocFile);
    serialization::writePod(bookFile, pos + lutOffset + lutSize + static_cast<uint32_t>(spineFile.position()));
  }

  ZipFile zip(epubPath);
  // Pre-open zip file to speed up size calculations
  if (!zip.open()) {
    LOG_ERR("BMC", "Could not open EPUB zip for size calculations");
    // Explicit close() required: member variables persist beyond function scope
    bookFile.close();
    spineFile.close();
    tocFile.close();
    return false;
  }

  uint32_t cumSize = 0;
  spineFile.seek(0);
  for (int i = 0; i < spineCount; i++) {
    auto spineEntry = readSpineEntry(spineFile);

    // No TOC in R1: every spine entry keeps tocIndex = -1.
    spineEntry.tocIndex = -1;

    size_t itemSize = 0;
    const std::string path = fsutil::normalisePath(spineEntry.href);
    if (!zip.getInflatedFileSize(path.c_str(), &itemSize)) {
      LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", path.c_str());
    }

    cumSize += itemSize;
    spineEntry.cumulativeSize = cumSize;

    // Write out spine data to book.bin
    writeSpineEntry(bookFile, spineEntry);
  }
  // Close opened zip file
  zip.close();

  // Loop through toc entries from toc file writing to book.bin (empty in R1)
  tocFile.seek(0);
  for (int i = 0; i < tocCount; i++) {
    auto tocEntry = readTocEntry(tocFile);
    writeTocEntry(bookFile, tocEntry);
  }

  // Explicit close() required: member variables persist beyond function scope
  bookFile.close();
  spineFile.close();
  tocFile.close();

  LOG_DBG("BMC", "Successfully built book.bin");
  return true;
}

bool BookMetadataCache::cleanupTmpFiles() const {
  const auto spineBinFile = cachePath + tmpSpineBinFile;
  if (Storage.exists(spineBinFile.c_str())) {
    Storage.remove(spineBinFile.c_str());
  }
  const auto tocBinFile = cachePath + tmpTocBinFile;
  if (Storage.exists(tocBinFile.c_str())) {
    Storage.remove(tocBinFile.c_str());
  }
  return true;
}

uint32_t BookMetadataCache::writeSpineEntry(HalFile& file, const SpineEntry& entry) const {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.href);
  serialization::writePod(file, entry.cumulativeSize);
  serialization::writePod(file, entry.tocIndex);
  return pos;
}

uint32_t BookMetadataCache::writeTocEntry(HalFile& file, const TocEntry& entry) const {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.title);
  serialization::writeString(file, entry.href);
  serialization::writeString(file, entry.anchor);
  serialization::writePod(file, entry.level);
  serialization::writePod(file, entry.spineIndex);
  return pos;
}

// Note: for the LUT to be accurate, this **MUST** be called for all spine items during the OPF pass
void BookMetadataCache::createSpineEntry(const std::string& href) {
  if (!buildMode || !spineFile) {
    LOG_DBG("BMC", "createSpineEntry called but not in build mode");
    return;
  }

  const SpineEntry entry(href, 0, -1);
  writeSpineEntry(spineFile, entry);
  spineCount++;
}

// Restored with the TOC parsers (dropped in 622fc5f). Resolves the TOC
// entry's href against the spine entries written so far, so the reader can
// jump from a chapter name to the file that holds it. The original also had
// an FNV hash index for books with 400+ spine items; that was pruned with
// everything else and a linear scan is fine at this scale (a 300-chapter
// book costs 300 short reads, once, at index time).
void BookMetadataCache::createTocEntry(const std::string& title, const std::string& href, const std::string& anchor,
                                       const uint8_t level) {
  if (!buildMode || !tocFile || !spineFile) {
    LOG_DBG("BMC", "createTocEntry called but not in build mode");
    return;
  }

  int16_t spineIndex = -1;
  const uint32_t restorePos = spineFile.position();
  spineFile.seek(0);
  for (int i = 0; i < spineCount; i++) {
    if (readSpineEntry(spineFile).href == href) {
      spineIndex = static_cast<int16_t>(i);
      break;
    }
  }
  spineFile.seek(restorePos);
  if (spineIndex == -1) {
    LOG_DBG("BMC", "createTocEntry: no spine item for TOC href %s", href.c_str());
  }

  // Compose to NFC at index time: device fonts have no combining-mark
  // positioning, so an NFD title renders broken.
  TocEntry entry;
  entry.title = utf8ComposeNfc(title);
  entry.href = href;
  entry.anchor = anchor;
  entry.level = level;
  entry.spineIndex = spineIndex;
  writeTocEntry(tocFile, entry);
  tocCount++;
}

/* ============= READING / LOADING FUNCTIONS ================ */

bool BookMetadataCache::load() {
  if (!Storage.openFileForRead("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(bookFile, version);
  if (version != BOOK_CACHE_VERSION) {
    LOG_DBG("BMC", "Cache version mismatch: expected %d, got %d", BOOK_CACHE_VERSION, version);
    // Explicit close() required: member variable persists beyond function scope
    bookFile.close();
    return false;
  }

  serialization::readPod(bookFile, lutOffset);
  serialization::readPod(bookFile, spineCount);
  serialization::readPod(bookFile, tocCount);

  serialization::readString(bookFile, coreMetadata.title);
  serialization::readString(bookFile, coreMetadata.author);
  serialization::readString(bookFile, coreMetadata.language);
  serialization::readString(bookFile, coreMetadata.coverItemHref);
  serialization::readString(bookFile, coreMetadata.textReferenceHref);

  loaded = true;
  LOG_DBG("BMC", "Loaded cache data: %d spine, %d TOC entries", spineCount, tocCount);
  return true;
}

BookMetadataCache::SpineEntry BookMetadataCache::getSpineEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getSpineEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(spineCount)) {
    LOG_ERR("BMC", "getSpineEntry index %d out of range", index);
    return {};
  }

  // Seek to spine LUT item, read from LUT and get out data
  bookFile.seek(lutOffset + sizeof(uint32_t) * index);
  uint32_t spineEntryPos;
  serialization::readPod(bookFile, spineEntryPos);
  bookFile.seek(spineEntryPos);
  return readSpineEntry(bookFile);
}

BookMetadataCache::TocEntry BookMetadataCache::getTocEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getTocEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(tocCount)) {
    LOG_DBG("BMC", "getTocEntry index %d out of range", index);
    return {};
  }

  // Seek to TOC LUT item, read from LUT and get out data
  bookFile.seek(lutOffset + sizeof(uint32_t) * spineCount + sizeof(uint32_t) * index);
  uint32_t tocEntryPos;
  serialization::readPod(bookFile, tocEntryPos);
  bookFile.seek(tocEntryPos);
  return readTocEntry(bookFile);
}

BookMetadataCache::SpineEntry BookMetadataCache::readSpineEntry(HalFile& file) const {
  SpineEntry entry;
  serialization::readString(file, entry.href);
  serialization::readPod(file, entry.cumulativeSize);
  serialization::readPod(file, entry.tocIndex);
  return entry;
}

BookMetadataCache::TocEntry BookMetadataCache::readTocEntry(HalFile& file) const {
  TocEntry entry;
  serialization::readString(file, entry.title);
  serialization::readString(file, entry.href);
  serialization::readString(file, entry.anchor);
  serialization::readPod(file, entry.level);
  serialization::readPod(file, entry.spineIndex);
  return entry;
}

}  // namespace reader
