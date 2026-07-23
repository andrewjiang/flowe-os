// TEMPORARY stage-1 smoke hook (see ReaderSmokeTest.h). Not called anywhere.
#include "ReaderSmokeTest.h"

#include <Arduino.h>

#include <memory>

#include "Epub.h"
#include "Page.h"
#include "ReaderFonts.h"
#include "ReaderLog.h"
#include "ReaderSettings.h"
#include "Section.h"
#include "TextMeasure.h"

bool readerSmokeTest(const char* epubPath) {
  using namespace reader;

  const uint32_t t0 = millis();
  const ReaderSettings settings = ReaderSettings::xphoneDefaults();

  auto epub = std::shared_ptr<Epub>(new (std::nothrow) Epub(epubPath, kReaderCacheRoot));
  if (!epub) {
    Serial.println("[reader:SMK] OOM allocating Epub");
    return false;
  }

  if (!epub->load()) {
    Serial.printf("[reader:SMK] failed to open/index %s\n", epubPath);
    return false;
  }
  Serial.printf("[reader:SMK] opened '%s' by '%s' — %d spine items, %u bytes, cache %s (%lu ms)\n",
                epub->getTitle().c_str(), epub->getAuthor().c_str(), epub->getSpineItemsCount(),
                static_cast<unsigned>(epub->getBookSize()), epub->getCachePath().c_str(), millis() - t0);

  if (epub->getSpineItemsCount() == 0) {
    Serial.println("[reader:SMK] no spine items");
    return false;
  }

  TextMeasure measure(readerFontFamily(settings.fontId));
  Section section(epub, 0, measure);

  const uint32_t t1 = millis();
  if (!section.loadSectionFile(settings)) {
    Serial.println("[reader:SMK] no valid section cache — building section 0");
    if (!section.createSectionFile(settings)) {
      Serial.println("[reader:SMK] failed to build section 0");
      return false;
    }
    if (!section.loadSectionFile(settings)) {
      Serial.println("[reader:SMK] section 0 built but failed to load back");
      return false;
    }
  }
  Serial.printf("[reader:SMK] section 0 ready: %u pages (%lu ms), heap free=%u\n", section.pageCount, millis() - t1,
                ESP.getFreeHeap());

  if (section.pageCount == 0) {
    Serial.println("[reader:SMK] section 0 has no pages");
    return false;
  }

  section.currentPage = 0;
  auto page = section.loadPageFromSectionFile();
  if (!page) {
    Serial.println("[reader:SMK] failed to deserialize page 0");
    return false;
  }

  size_t lineCount = 0;
  size_t wordCount = 0;
  size_t ruleCount = 0;
  for (const auto& el : page->elements) {
    if (el->getTag() == TAG_PageLine) {
      lineCount++;
      const auto& line = static_cast<const PageLine&>(*el);
      if (line.getBlock()) {
        wordCount += line.getBlock()->wordCount();
      }
    } else if (el->getTag() == TAG_PageHorizontalRule) {
      ruleCount++;
    }
  }

  Serial.printf("[reader:SMK] page 0: %u elements — %u lines, %u words, %u rules; progress %.1f%%\n",
                static_cast<unsigned>(page->elements.size()), static_cast<unsigned>(lineCount),
                static_cast<unsigned>(wordCount), static_cast<unsigned>(ruleCount),
                epub->calculateProgress(0, 0.0f) * 100.0f);
  return true;
}
