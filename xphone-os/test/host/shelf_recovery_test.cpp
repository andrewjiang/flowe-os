#include "reader/FbpBook.h"
#include "Gfx.h"
#include <Arduino.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" uint32_t uzlib_crc32(const void*, unsigned, uint32_t) { std::abort(); }
extern "C" uint32_t uzlib_adler32(const void*, unsigned, uint32_t) { std::abort(); }
void Gfx::drawPixel(int, int, bool) {}
using HalFile = FsFile;
struct TestStorage {
  bool openFileForRead(const char*, const char* p, HalFile& f) {
    f = SdMan.open(p, O_RDONLY); return bool(f);
  }
} Storage;
void xpTrace(const char*) {}
bool endsWith(const char* p, const char* ext) {
  const size_t n = strlen(p), m = strlen(ext);
  return n >= m && strcasecmp(p + n - m, ext) == 0;
}
bool endsWithFbpCI(const char* p) { return endsWith(p, ".fbp"); }
bool endsWithEpubCI(const char* p) { return endsWith(p, ".epub"); }
const char* baseName(const char* p) { const char* s = strrchr(p, '/'); return s ? s + 1 : p; }
void prettyFileTitle(const char* p, char* out, size_t n) { snprintf(out, n, "%s", baseName(p)); }
constexpr int kThumbW = 200, kThumbH = 260;
void publishCoverSidecar(const char*, const char*) { std::abort(); }
namespace reader {
constexpr const char* kReaderCacheRoot = "/cache";
struct ReadingStats {
  static bool bookStats(const char*, uint32_t*, uint32_t*, uint32_t*) { return false; }
};
struct Epub {
  Epub(const char*, const char*) {}
  std::string getCachePath() const { return "/cache"; }
  bool load(bool) { std::abort(); }
  const std::string& getTitle() const { static std::string s; return s; }
  const std::string& getAuthor() const { static std::string s; return s; }
};
struct CoverThumb { static int probe(Epub&, int, int, std::string*) { std::abort(); } };
}
struct ReaderScene {
  static constexpr int kMaxBooks = 32;
  // PRODUCTION_ENTRIES
  BookEntry _books[kMaxBooks]{};
  int _bookCount = 0, _totalBooks = 0, _windowOffset = 0, _sel = 0, _scroll = 0;
  int _liveCount = 0, _doneCount = 0;
  unsigned _skippedNames = 0;
  bool _sdOk = false, _doneView = false, _radioSuspended = false;
  bool loadShelfIndex();
  void saveShelfIndex();
  void scanBooks(int windowOffset = 0);
  void scanDir(const char*, int);
  void applyShelfView() {} // No done books in these fixtures.
};
// PRODUCTION_METHODS

static void put(std::vector<uint8_t>& b, size_t off, uint64_t v, size_t n) {
  for (size_t i = 0; i < n; ++i) b[off+i] = v >> (i*8);
}
static void writeFile(const char* p, const std::vector<uint8_t>& b) {
  FILE* f = fopen((testSdRoot + p).c_str(), "wb");
  assert(f && fwrite(b.data(), 1, b.size(), f) == b.size());
  fclose(f);
}
static std::vector<uint8_t> package(bool cover, bool strip, bool noShelf = false) {
  const uint32_t coverBytes = cover ? 8 : 0, stripBytes = strip ? 4 : 0;
  std::vector<uint8_t> b(272 + coverBytes + stripBytes, 0);
  memcpy(b.data(), "FBPK", 4);
  put(b, 4, 6, 2); put(b, 6, 6, 2); put(b, 24, 1, 4);
  put(b, 64, 200, 8); put(b, 72, noShelf ? 0 : 256, 8);
  // Metadata uses u16 lengths. Index fixture deliberately differs, proving
  // a cover retry preserves cached title/author/focus without re-reading.
  put(b, 200, 7, 2); memcpy(b.data()+202, "Package", 7);
  put(b, 209, 6, 2); memcpy(b.data()+211, "Author", 6);
  put(b, 256, cover ? 8 : 0, 2); put(b, 258, cover ? 8 : 0, 2);
  put(b, 260, coverBytes, 4);
  put(b, 264, strip ? 8 : 0, 2); put(b, 266, strip ? 4 : 0, 2);
  put(b, 268, stripBytes, 4);
  for (size_t i = 272; i < b.size(); ++i) b[i] = 0xa5;
  return b;
}
static void seedIndex(uint32_t size, bool cover) {
  ReaderScene scene;
  scene._bookCount = scene._totalBooks = 1;
  auto& b = scene._books[0];
  strcpy(b.path, "/books/test.fbp"); b.sizeBytes = size;
  strcpy(b.title, "Cached title"); strcpy(b.author, "Cached author"); b.focusEdition = true;
  b.meta = cover ? ReaderScene::TileMeta::Cover : ReaderScene::TileMeta::NoCover;
  b.thumbW = b.thumbH = cover ? 8 : 0;
  scene.saveShelfIndex();
  testIndexWrites = 0;
}
static void expectCover(const ReaderScene& s) {
  assert(s._bookCount == 1);
  assert(s._books[0].meta == ReaderScene::TileMeta::Cover && "shelf scan must recover cached missing cover");
  assert(s._books[0].thumbW == 8 && s._books[0].thumbH == 8);
  auto f = SdMan.open("/books/test.fbp.cov", O_RDONLY);
  assert(f && f.size() == 16);
  uint8_t bytes[16]{}; assert(f.read(bytes, sizeof(bytes)) == 16);
  for (size_t i = 8; i < 16; ++i) assert(bytes[i] == 0xa5);
}
static void expectCachedMeta(const ReaderScene& s) {
  assert(strcmp(s._books[0].title, "Cached title") == 0);
  assert(strcmp(s._books[0].author, "Cached author") == 0);
  assert(s._books[0].focusEdition);
}
int main(int argc, char** argv) {
  assert(argc == 3);
  testSdRoot = argv[1];
  std::filesystem::create_directory(testSdRoot + "/books");
  const std::string scenario = argv[2];
  if (scenario == "deferred") {
    writeFile("/books/test.epub", {});
    ReaderScene scene; scene.scanBooks();
    assert(scene._bookCount == 1 && scene._books[0].meta == ReaderScene::TileMeta::Unknown);
    assert(testIndexWrites == 0); return 0;
  }
  if (scenario == "unknown") {
    writeFile("/books/test.fbp", {1,2,3});
    ReaderScene scene; scene.scanBooks(); scene.scanBooks();
    assert(scene._books[0].meta == ReaderScene::TileMeta::Unknown);
    assert(testIndexWrites == 0); return 0;
  }
  const bool coverless = scenario == "coverless" || scenario == "strip-only";
  auto bytes = package(!coverless, scenario == "strip-only", scenario == "coverless");
  if (scenario == "truncated-package") bytes.resize(276);
  if (scenario == "bad-dimensions") put(bytes, 258, 9, 2); // 8 bytes cannot encode 8x9.
  writeFile("/books/test.fbp", bytes);
  if (scenario == "declares") {
    bool cover = false, strip = false, declares = false;
    assert(reader::FbpBook::ensureShelfSidecars("/books/test.fbp", &cover, &strip, &declares));
    assert(cover && !strip && declares);
    writeFile("/books/test.fbp", package(true, true));
    assert(reader::FbpBook::ensureShelfSidecars("/books/test.fbp", &cover, &strip, &declares));
    assert(cover && strip && declares);
    declares = false;
    assert(reader::FbpBook::ensureShelfSidecars("/books/test.fbp", &cover, &strip, &declares));
    assert(declares); // Both sidecars already exist: still report the package declaration.
    SdMan.remove("/books/test.fbp.cov"); SdMan.remove("/books/test.fbp.str");
    writeFile("/books/test.fbp", package(false, true));
    assert(reader::FbpBook::ensureShelfSidecars("/books/test.fbp", &cover, &strip, &declares));
    assert(!cover && strip && !declares);
    SdMan.remove("/books/test.fbp.str");
    writeFile("/books/test.fbp", package(false, false, true));
    declares = true;
    assert(!reader::FbpBook::ensureShelfSidecars("/books/test.fbp", &cover, &strip, &declares));
    assert(!cover && !strip && !declares);
    return 0;
  }
  if (scenario != "fresh")
    seedIndex(bytes.size(), scenario.find("positive-") == 0 || scenario == "cache-hit");
  if (scenario == "positive-truncated" || scenario == "cache-hit") {
    std::vector<uint8_t> cov = {0x54,0x58,1,0,8,0,8,0};
    if (scenario == "cache-hit") cov.resize(16, 0xa5);
    writeFile("/books/test.fbp.cov", cov);
  }
  if (scenario == "write-failure") testFailCoverWrite = true;
  if (scenario == "partial-write") testCoverWriteLimit = 10;
  ReaderScene scene; scene.scanBooks();
  if (scenario == "fresh") {
    expectCover(scene);
    assert(strcmp(scene._books[0].title, "Package") == 0);
    assert(testIndexWrites == 1);
    scene.scanBooks(); expectCover(scene);
    assert(testIndexWrites == 1);
    return 0;
  }
  expectCachedMeta(scene);
  if (coverless) {
    assert(scene._books[0].meta == ReaderScene::TileMeta::NoCover);
    assert(!SdMan.exists("/books/test.fbp.cov"));
    assert(SdMan.exists("/books/test.fbp.str") == (scenario == "strip-only"));
    scene.scanBooks();
    assert(testIndexWrites == 0 && "unchanged coverless row must not rewrite index");
  } else if (scenario == "write-failure" || scenario == "partial-write" || scenario == "truncated-package" || scenario == "bad-dimensions") {
    assert(scene._books[0].meta == ReaderScene::TileMeta::NoCover);
    assert(!SdMan.exists("/books/test.fbp.cov") && "failed extraction must leave no partial cover");
    assert(testIndexWrites == 0);
    testFailCoverWrite = false; testCoverWriteLimit = SIZE_MAX;
    if (scenario == "truncated-package" || scenario == "bad-dimensions") return 0;
    scene.scanBooks(); expectCover(scene);
    assert(testIndexWrites == 1);
  } else {
    expectCover(scene);
    const unsigned writes = testIndexWrites;
    assert(writes == (scenario == "cache-hit" ? 0 : 1));
    scene.scanBooks(); expectCover(scene); expectCachedMeta(scene);
    assert(testIndexWrites == writes);
    auto index = SdMan.open(kShelfIdxPath, O_RDONLY);
    ShelfIdxHead head{}; ShelfIdxRow row{};
    assert(index.read(&head, sizeof(head)) == sizeof(head) && head.count == 1);
    assert(index.read(&row, sizeof(row)) == sizeof(row) && row.hasCover == 1);
  }
  printf("PASS: %s\n", scenario.c_str());
}
