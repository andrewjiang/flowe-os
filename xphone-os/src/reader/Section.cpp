// Pruned from x4-os lib/Epub/Epub/Section.cpp (518 LOC -> ~250). See Section.h.
#include "Section.h"

#include <Serialization.h>

#include <vector>

#include "ChapterHtmlSlimParser.h"
#include "Page.h"
#include "ReaderLog.h"

namespace reader {

namespace {
// v27: words NFC-composed at layout time. Header layout matches CrossPoint's
// section.bin v27 exactly:
//   version(u8) fontId(int) lineCompression(float) extraParagraphSpacing(bool)
//   paragraphAlignment(u8) viewportWidth(u16) viewportHeight(u16)
//   hyphenationEnabled(bool) embeddedStyle(bool) imageRendering(u8)
//   focusReadingEnabled(bool) pageCount(u16) lutOffset(u32)
//   anchorMapOffset(u32) paragraphLutOffset(u32) liLutOffset(u32)
// The last three offsets are always 0 in the pruned engine.
//
// Header byte layout is still CrossPoint's v27; the version NUMBER is bumped to
// 28 only to invalidate caches built before the TextFold glyph-coverage pass
// (symbol/Greek remap) and noteref superscripting changed the paginated text.
// Old files fail the version check below and re-index cleanly.
constexpr uint8_t SECTION_FILE_VERSION = 28;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint8_t) + sizeof(bool) + sizeof(uint32_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t);
}  // namespace

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!page) {
    LOG_ERR("SCT", "Null page passed for page %d", pageCount);
    return 0;
  }
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", pageCount);

  pageCount++;
  return position;
}

void Section::writeSectionFileHeader(const ReaderSettings& settings) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  serialization::writePod(file, SECTION_FILE_VERSION);
  serialization::writePod(file, settings.fontId);
  serialization::writePod(file, settings.lineCompression);
  serialization::writePod(file, settings.extraParagraphSpacing);
  serialization::writePod(file, settings.paragraphAlignment);
  serialization::writePod(file, settings.viewportWidth);
  serialization::writePod(file, settings.viewportHeight);
  serialization::writePod(file, ReaderSettings::kHyphenationEnabled);
  serialization::writePod(file, ReaderSettings::kEmbeddedStyle);
  serialization::writePod(file, ReaderSettings::kImageRendering);
  serialization::writePod(file, ReaderSettings::kFocusReadingEnabled);
  serialization::writePod(file, pageCount);                  // Placeholder for page count (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));   // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));   // anchor map offset — always 0 (pruned)
  serialization::writePod(file, static_cast<uint32_t>(0));   // paragraph LUT offset — always 0 (pruned)
  serialization::writePod(file, static_cast<uint32_t>(0));   // li LUT offset — always 0 (pruned)
}

bool Section::loadSectionFile(const ReaderSettings& settings) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  {
    uint8_t version;
    serialization::readPod(file, version);
    if (version != SECTION_FILE_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
      return false;
    }

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    bool fileFocusReadingEnabled;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileEmbeddedStyle);
    serialization::readPod(file, fileImageRendering);
    serialization::readPod(file, fileFocusReadingEnabled);

    if (settings.fontId != fileFontId || settings.lineCompression != fileLineCompression ||
        settings.extraParagraphSpacing != fileExtraParagraphSpacing ||
        settings.paragraphAlignment != fileParagraphAlignment || settings.viewportWidth != fileViewportWidth ||
        settings.viewportHeight != fileViewportHeight ||
        ReaderSettings::kHyphenationEnabled != fileHyphenationEnabled ||
        ReaderSettings::kEmbeddedStyle != fileEmbeddedStyle ||
        ReaderSettings::kImageRendering != fileImageRendering ||
        ReaderSettings::kFocusReadingEnabled != fileFocusReadingEnabled) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  serialization::readPod(file, pageCount);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages", pageCount);
  return true;
}

bool Section::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

bool Section::createSectionFile(const ReaderSettings& settings) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Retry logic for SD card timing issues
  bool success = false;
  uint32_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
      delay(50);  // Brief delay before retry
    }

    // Remove any incomplete file from previous attempt before retrying
    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }

    HalFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    // Explicitly close() file before calling Storage.remove()
    tmpHtml.close();

    // If streaming failed, remove the incomplete file immediately
    if (!success && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
      LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
    }
  }

  if (!success) {
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    return false;
  }

  LOG_DBG("SCT", "Streamed temp HTML to %s (%lu bytes)", tmpHtmlPath.c_str(), static_cast<unsigned long>(fileSize));

  if (!Storage.openFileForWrite("SCT", filePath, file)) {
    return false;
  }
  pageCount = 0;
  writeSectionFileHeader(settings);
  std::vector<uint32_t> lut = {};

  ChapterHtmlSlimParser visitor(epub, tmpHtmlPath, measure, settings.fontId, settings.lineCompression,
                                settings.extraParagraphSpacing, settings.paragraphAlignment, settings.viewportWidth,
                                settings.viewportHeight,
                                [this, &lut](std::unique_ptr<Page> page) { lut.push_back(onPageComplete(std::move(page))); });
  success = visitor.parseAndBuildPages();

  Storage.remove(tmpHtmlPath.c_str());
  if (!success) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filePath.c_str());
    return false;
  }

  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  // Write LUT
  for (const auto offset : lut) {
    if (offset == 0) {
      hasFailedLutRecords = true;
      break;
    }
    serialization::writePod(file, offset);
  }

  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filePath.c_str());
    return false;
  }

  // Patch header with final pageCount and lutOffset. The anchor-map /
  // paragraph-LUT / li-LUT offsets stay 0 (features pruned).
  file.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  return true;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return nullptr;
  }

  file.seek(HEADER_SIZE - sizeof(uint32_t) * 4);
  uint32_t lutOffset;
  serialization::readPod(file, lutOffset);
  file.seek(lutOffset + sizeof(uint32_t) * currentPage);
  uint32_t pagePos;
  serialization::readPod(file, pagePos);
  file.seek(pagePos);

  auto page = Page::deserialize(file);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  return page;
}

std::optional<uint16_t> Section::getCachedPageCount() const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (fileSize < HEADER_SIZE) {
    return std::nullopt;
  }

  f.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(uint16_t));
  uint16_t count;
  serialization::readPod(f, count);
  return count;
}

}  // namespace reader
