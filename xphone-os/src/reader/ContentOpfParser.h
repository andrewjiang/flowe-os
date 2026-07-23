// Pruned from x4-os lib/Epub/Epub/parsers/ContentOpfParser.h.
// Removed: the >=400-item manifest FNV index (linear idref scan kept), CSS
// file collection, NCX/nav TOC path capture (spine-linear R1). Kept:
// streaming manifest -> .items.bin temp store, spine idref -> href
// resolution, dc:title/creator/language metadata, guide "text" reference
// (start-of-content). R2a re-adds CrossPoint's cover-item detection (see
// coverHref below) for the cover thumbnail pipeline.
#pragma once
#include <HalStorage.h>
#include <Print.h>
#include <expat.h>

#include <string>

namespace reader {

class BookMetadataCache;

class ContentOpfParser final : public Print {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
    IN_BOOK_TITLE,
    IN_BOOK_AUTHOR,
    IN_BOOK_LANGUAGE,
    IN_MANIFEST,
    IN_SPINE,
    IN_GUIDE,
  };

  const std::string& cachePath;
  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;
  HalFile tempItemStore;

  // Cover candidates, resolved into coverHref when the manifest ends.
  // <meta name="cover" content="ID"> (metadata precedes the manifest per the
  // OPF spec, so the id is known while manifest items stream past — same
  // ordering assumption CrossPoint makes).
  std::string coverMetaItemId;
  std::string coverMetaHref;   // manifest item whose id matches coverMetaItemId (image/* only)
  std::string coverPropsHref;  // EPUB 3 manifest item with properties containing "cover-image"
  std::string coverGuessHref;  // last resort: image/* manifest item whose href contains "cover"

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);

 public:
  std::string title;
  std::string author;
  std::string language;
  std::string textReferenceHref;
  // Cover image href (resolved against baseContentPath, uri-decoded and
  // normalised like spine hrefs). Precedence: properties="cover-image" >
  // <meta name="cover"> id match > href containing "cover". Empty = no cover.
  std::string coverHref;

  explicit ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, const size_t xmlSize,
                            BookMetadataCache* cache)
      : cachePath(cachePath), baseContentPath(baseContentPath), remainingSize(xmlSize), cache(cache) {}
  ~ContentOpfParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};

}  // namespace reader
