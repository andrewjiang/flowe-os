#include "ReaderScene.h"

#include "../CpuBoost.h"

// xphone-os Reader R2a — scene over the CrossPoint engine port (src/reader).
//
// Flow:
//   onEnter: NVS "rdBook" path exists on SD -> Opening (deferred open+resume);
//            otherwise scan /books (+ root) -> BookList. NVS "rdFont" restores
//            the last font size before any open.
//   Opening/Indexing frames: render() composes the progress frame and ARMS the
//     pending Work; handleInput() runs the blocking work only once the flush
//     worker is idle (frame guaranteed on glass). Chains naturally:
//     Opening -> (no section cache) -> Indexing -> Reading.
//   Reading: render() deserializes the current Page from section.bin, blits it
//     via BookTextRenderer, draws the status line, writes progress.bin
//     (atomic), and — in the last 30% of a chapter — arms a silent
//     next-chapter prefetch that runs on a quiet tick.
//   Page turns: Right/Down forward, Left/Up back; chapter roll at section
//     ends, spine roll at book ends (backwards lands on the previous
//     chapter's LAST page via the kLastPageSentinel). CONFIRM ("SIZE")
//     cycles the font 12 -> 14 -> 16 -> 12 pt: fontId is part of the
//     section.bin cache key, so ensureSectionOrIndex() rebuilds (or reloads)
//     the chapter at the new size and the position survives as a page RATIO
//     applied once the new pageCount is known. BACK tap -> BookList;
//     long-press BACK -> launcher (OS-wide).
//   BookList: a 2-column x 2-row cover grid. Tiles start as placeholders;
//     Work::GridMeta lazily fills ONE visible tile per quiet tick (load the
//     book.bin cache, building it — metadata pass only — if missing so a
//     never-opened book still shows its title + cover, then CoverThumb::ensure
//     — the expensive zip extract + JPEG/PNG decode). Unreadable books show
//     "unreadable"; opening a book never waits on grid work (it yields to
//     input and is dropped on open).
//
// E-ink: page turns are plain markDirty() (full-panel) — SceneManager's
// FAST/HALF cadence + flush worker handle refresh discipline; no refresh
// logic here. Grid selection moves use partial dirty rects
// (NotificationsScene idiom): old+new tile when the scroll row is unchanged,
// the whole list region otherwise.
//
// SD concurrency: heavy engine work is additionally gated on the flush worker
// being idle. Small SD reads/writes (progress.bin, section headers) can
// overlap a flush — both EpdBus and SdFat wrap every transfer in SPI
// transactions, which serialize on arduino-esp32's bus lock.

#include <Arduino.h>
#include <InflateReader.h>
#include <Preferences.h>
#include <SDCardManager.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>
#include <new>

#include "../Fonts.h"
#include "../ble/CompanionBleService.h"
#include "../reader/CoverThumb.h"
#include "../reader/ReadingStats.h"
#include "../reader/Epub.h"
#include "../reader/Page.h"
#include "../reader/ProgressFile.h"
#include "../reader/ReaderFonts.h"
#include "../reader/Section.h"
#include "../reader/TextMeasure.h"
#ifdef XP_READER_SMOKE
#include "../reader/ReaderSmokeTest.h"
#endif
#include "AppScenes.h"

namespace {

// NVS (same "xphone" namespace as the Sleep persists; key <= 15 chars).
constexpr const char* kPrefsNamespace = "xphone";
constexpr const char* kPrefsBookKey = "rdBook";
constexpr const char* kPrefsFontKey = "rdFont";  // reader fontId (0/1/2)

// Layout (logical portrait). The text viewport is part of the section.bin
// cache key (settings-in-header), so these only change with a cache rebuild.
constexpr int kMarginX = 24;
constexpr int kMarginTop = 24;
constexpr int kStatusH = 24;  // status line strip above the soft-key bar

// Book list — 2x2 cover grid. Tile w/h derive from the panel (X3: 264x351,
// X4: 240x355); the thumb target box is a fixed constant because it names the
// on-SD cache file (cover_200x260.bin). 200x260 leaves room in the X3 tile
// for a bold title line + a small author line under the cover without the
// text crossing the selection border.
constexpr int kListMarginX = 20;
constexpr int kHeaderH = 46;
constexpr int kGridCols = 2;
constexpr int kGridRows = 2;
// Concept A stats band (docs/plans/2026-07-29-reading-stats-design.md):
// one compact line — streak + pages-today inline, Mon–Sun week bars right —
// between the header and the shelf. 56px: the 260px thumb bitmaps are fixed,
// so every band pixel comes out of tile text room (84px overlapped, live X4).
constexpr int kStatsBandH = 56;
constexpr int kThumbW = 200;      // cover thumb target box (aspect-fit inside)
constexpr int kThumbH = 260;
constexpr int kSelInset = 6;      // selection border inset from the tile edge
constexpr int kSelRadius = 10;
constexpr int kSelThick = 3;
constexpr int kThumbTop = 8;      // tile top -> thumb box (tightened for the stats band)
constexpr int kTitleGap = 6;      // thumb box bottom -> title line
constexpr int kTileTextPad = 14;  // horizontal padding for tile text

bool endsWithEpubCI(const char* name) {
  const size_t len = strlen(name);
  if (len < 6) return false;
  const char* ext = name + len - 5;
  return tolower(ext[0]) == '.' && tolower(ext[1]) == 'e' && tolower(ext[2]) == 'p' && tolower(ext[3]) == 'u' &&
         tolower(ext[4]) == 'b';
}

const char* baseName(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

// Copy `src` into `dst`, chopping whole UTF-8 sequences and appending "..."
// until it fits in `maxWidth` pixels (NotificationsScene helper).
void truncateToWidth(Gfx& gfx, const XpFont& font, const char* src, int maxWidth, char* dst, size_t dstSize) {
  snprintf(dst, dstSize, "%s", src ? src : "");
  if (gfx.textWidth(font, dst) <= maxWidth) return;
  size_t len = strlen(dst);
  while (len > 0) {
    do {
      len--;
    } while (len > 0 && (static_cast<uint8_t>(dst[len]) & 0xC0) == 0x80);
    dst[len] = '\0';
    char probe[128];
    snprintf(probe, sizeof(probe), "%s...", dst);
    if (gfx.textWidth(font, probe) <= maxWidth) {
      snprintf(dst, dstSize, "%s", probe);
      return;
    }
  }
}

// Read a thumb .bin's actual dimensions (header format documented in
// CoverThumb.h) once at grid-work time, so render() can CENTER the aspect-fit
// blit without reopening the file every frame.
bool readThumbDims(const char* binPath, uint16_t* w, uint16_t* h) {
  HalFile f;
  if (!Storage.openFileForRead("RDR", binPath, f)) return false;
  uint8_t hdr[8];
  if (f.read(hdr, sizeof(hdr)) != sizeof(hdr)) return false;
  if (hdr[0] != 0x54 || hdr[1] != 0x58 || hdr[2] != 1) return false;  // 'XT' LE + v1
  const uint16_t tw = static_cast<uint16_t>(hdr[4] | (hdr[5] << 8));
  const uint16_t th = static_cast<uint16_t>(hdr[6] | (hdr[7] << 8));
  if (tw == 0 || th == 0) return false;
  *w = tw;
  *h = th;
  return true;
}

}  // namespace

// --- Lifecycle ---------------------------------------------------------------

ReaderScene::ReaderScene() = default;   // Epub/Section/TextMeasure complete here
ReaderScene::~ReaderScene() = default;

void ReaderScene::onEnter() {
  // Radio and reader take turns for the WHOLE scene — grid included. The
  // BLE-connected-shelf experiment died on live X3 numbers: with the static
  // 32 KB dict resident, full BLE+ANCS bring-up beside even a torn-down
  // Reader left 1,352 B free (min 448) — every connection-time malloc
  // failed, iOS hung at "discovering services", reconnect churn fragmented
  // the rest. The stack barely fits at boot (~6 KB margin); it cannot share
  // the scene with anything. Phone sync happens from home and every
  // non-Reader scene, one Back press away.
  suspendRadioForBookWork();

  _work = Work::None;
  _workArmed = false;
  _pageLoadRetries = 0;
  _hasPendingRatio = false;

#ifdef XP_READER_SMOKE
  // Stage-1 serial smoke hook, compile-gated (default off): run once against
  // the first epub found on the card.
  {
    static bool smokeRan = false;
    if (!smokeRan) {
      smokeRan = true;
      scanBooks();
      if (_bookCount > 0) readerSmokeTest(_books[0].path);
    }
  }
#endif

  // Resume the last book when its file still exists (otherwise book list) and
  // the last font size (before any Work::OpenBook builds a TextMeasure).
  char path[sizeof(BookEntry::path)] = {0};
  {
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, /*readOnly=*/true)) {
      prefs.getString(kPrefsBookKey, path, sizeof(path));
      const uint8_t fid = prefs.getUChar(kPrefsFontKey, static_cast<uint8_t>(_settings.fontId));
      if (fid < reader::kReaderFontCount) _settings.fontId = fid;
      prefs.end();
    }
  }
  if (path[0] != '\0' && Storage.exists(path)) {
    _bookPath = path;
    _prefetchAttemptedSpine = -1;
    _state = State::Opening;
    _work = Work::OpenBook;
    markDirty();
  } else {
    enterBookList();
  }
}

void ReaderScene::suspendRadioForBookWork() {
  // Radio and reader take turns (no PSRAM on the X3): with BLE+ANCS resident
  // there isn't enough heap for expat + pagination + the cover scratch.
  // Called from onEnter (idempotent belt at workOpenBook); onExit resumes
  // the radio and the phone reconnects automatically.
  if (_radioSuspended) return;
  _radioSuspended = true;
  COMPANION_BLE.suspendForReader();

  // DICT FIRST, cover scratch second — reading beats covers. Claimed at the
  // cleanest post-suspend heap; freed in onExit before the radio returns.
  // Probe-verified on X3: fresh boots claim fine (largest 59-102 KB free
  // here); heavy BLE churn within one power-on can fragment below 32 KB,
  // in which case streaming inflate falls to the repaired one-shot path:
  // small/medium entries read, oversized chapters fail as a retryable
  // "Couldn't index" until a restart. STORED-repacked uploads (book-
  // management design doc) remove the window dependency entirely.
  // No heap dict claim anymore: the 32 KB inflate window is LENT from the
  // static framebuffer inside runWork() (see there). The suspend-time malloc
  // was a fragmentation dice-roll — it lost by 12 bytes to allocator crumbs
  // after BLE churn (FRAGMAP 2026-08-06); the loan cannot lose. Reading also
  // gains the 32 KB the parked dict used to hold.
  reader::CoverThumb::preacquireScratch();
}

void ReaderScene::onExit() {
  reader::ReadingStats::sessionEnd();  // covers home/sleep exits mid-book
  // CrossPoint activity model: hold nothing while another scene is up.
  // Reopening from book.bin + progress.bin in onEnter is fast.
  _section.reset();
  _epub.reset();
  _measure.reset();
  _renderer.releaseCaches();
  reader::CoverThumb::releaseScratch();  // don't hold the ~58KB decoder block for other scenes
  InflateReader::releaseSharedDict();    // 32 KB back to the heap before the radio needs it
  _work = Work::None;
  _workArmed = false;

  if (_radioSuspended) {
    _radioSuspended = false;
    COMPANION_BLE.resumeAfterReader();
  }
}

// --- Soft keys -----------------------------------------------------------------

const char* const* ReaderScene::softKeys() const {
  static constexpr const char* kHidden[4] = {nullptr, nullptr, nullptr, nullptr};
  // Slot 1 = the CONFIRM front button (SDK ladder order): font-size cycle.
  static constexpr const char* kReading[4] = {"BOOKS", "SIZE", "PREV", "NEXT"};
  static constexpr const char* kList[4] = {"BACK", "OPEN", "UP", "DOWN"};
  static constexpr const char* kListEmpty[4] = {"BACK", nullptr, nullptr, nullptr};
  switch (_state) {
    case State::Reading:
      return kReading;
    case State::BookList:
      return _bookCount > 0 ? kList : kListEmpty;
    case State::Error:
      return kListEmpty;
    case State::Opening:
    case State::Indexing:
    default:
      return kHidden;  // busy frames: no tabs (long-press BACK still OS-wide)
  }
}

// --- Deferred blocking work ----------------------------------------------------

void ReaderScene::runWork() {
  // 160 MHz exactly for the blocking work below (indexing dominates at ~51 ms
  // per page on the 80 MHz park); re-parks on every exit path via the guard.
  // Radio is already suspended scene-wide, so there is no BLE coexistence to
  // weigh — and the panel/SPI paths run at 160 on every boot before Stage 6.
  CpuBoost boost;
  // CrossPoint-style framebuffer loan (upstream #2563): the static BSS
  // framebuffer (52,272 B X3 / 48,000 B X4) doubles as the 32 KB inflate
  // window for the duration of this work unit. Safe because the caller
  // guarantees the flush worker is idle (frame already on glass, panel
  // retains it) and render() recomposes the framebuffer from scratch after
  // every work unit. A loan cannot fragment the heap and cannot be denied —
  // the whole 12-bytes-short fragmentation class ends here.
  if (_fbForLoan) InflateReader::lendDict(_fbForLoan);
  const Work w = _work;
  _work = Work::None;
  _workArmed = false;
  switch (w) {
    case Work::OpenBook:
      workOpenBook();
      break;
    case Work::BuildSection:
      workBuildSection();
      break;
    case Work::PrefetchNext:
      workPrefetchNext();
      break;
    case Work::GridMeta:
      workGridMeta();
      break;
    case Work::None:
      break;
  }
  InflateReader::returnDict();  // loan ends before the next render scribbles
}

void ReaderScene::failWith(const char* msg) {
  // Largest CONTIGUOUS block, not just total free: every reader OOM here is a
  // single big allocation (32 KB inflate window, whole-entry one-shot buffer),
  // so total-free alone reads "plenty of heap" while the failure is real.
  Serial.printf("[xphone-os] reader: %s (free=%u largest=%u)\n", msg, ESP.getFreeHeap(),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  _errorMsg = msg;
  _state = State::Error;
  _work = Work::None;
  markDirty();
}

void ReaderScene::workOpenBook() {
  suspendRadioForBookWork();  // opening always needs the dict + a quiet heap
  // Leaving the grid: hand the ~58KB cover-decoder scratch back to the heap so
  // chapter indexing (32KB inflate + expat + line breaking) has full headroom.
  reader::CoverThumb::releaseScratch();
  const uint32_t t0 = millis();
  auto* e = new (std::nothrow) reader::Epub(_bookPath, reader::kReaderCacheRoot);
  if (!e) {
    failWith("Out of memory opening book");
    return;
  }
  _epub.reset(e);

  // load() reads the cached book.bin (fast) or builds it on first open (zip
  // scan + content.opf parse — the slow path this Opening frame covers).
  if (!_epub->load(/*buildIfMissing=*/true) || _epub->getSpineItemsCount() <= 0) {
    _epub.reset();
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, /*readOnly=*/false)) {
      prefs.remove(kPrefsBookKey);  // don't loop into the same failure on re-entry
      prefs.end();
    }
    failWith("Couldn't open this book");
    return;
  }

  auto* m = new (std::nothrow) reader::TextMeasure(reader::readerFontFamily(_settings.fontId));
  if (!m) {
    _epub.reset();
    failWith("Out of memory opening book");
    return;
  }
  _measure.reset(m);

  loadProgress();
  Serial.printf("[xphone-os] reader: opened '%s' (%d spine items, %lu ms) heap=%u\n", _epub->getTitle().c_str(),
                _epub->getSpineItemsCount(), static_cast<unsigned long>(millis() - t0), ESP.getFreeHeap());
  ensureSectionOrIndex();
}

void ReaderScene::workBuildSection() {
  if (!_epub || !_section) {
    failWith("Couldn't index this chapter");
    return;
  }
  // BLOCKING: zip inflate + expat SAX + DP line break + serialize, up to
  // seconds for a big chapter. The loop (input drain, BLE pump) stalls for
  // the duration — acceptable v1; known R2 improvement is chunking this work.
  const uint32_t t0 = millis();
  if (!_section->createSectionFile(_settings) || !_section->loadSectionFile(_settings)) {
    _section.reset();
    failWith("Couldn't index this chapter");
    return;
  }
  Serial.printf("[xphone-os] reader: indexed spine %d: %u pages in %lu ms, heap=%u\n", _spine,
                _section->pageCount, static_cast<unsigned long>(millis() - t0), ESP.getFreeHeap());
  applyPendingPage();
  _state = State::Reading;
  reader::ReadingStats::sessionStart(_bookPath);
  markDirty();
}

void ReaderScene::workPrefetchNext() {
  const int next = _spine + 1;
  _prefetchAttemptedSpine = next;  // one attempt per spine, success or not
  if (!_epub || !_measure || next >= _epub->getSpineItemsCount()) return;

  reader::Section prefetch(_epub, next, *_measure);
  if (prefetch.loadSectionFile(_settings)) return;  // already cached + valid

  // BLOCKING (same engine path as workBuildSection, up to seconds) — done
  // silently while a page sits on glass, only on a tick with no input
  // waiting. Known R2 improvement: chunk it so the loop keeps pumping.
  const uint32_t t0 = millis();
  if (prefetch.createSectionFile(_settings)) {
    Serial.printf("[xphone-os] reader: prefetched spine %d (%lu ms)\n", next,
                  static_cast<unsigned long>(millis() - t0));
  } else {
    Serial.printf("[xphone-os] reader: prefetch of spine %d FAILED\n", next);
  }
}

void ReaderScene::workGridMeta() {
  if (_state != State::BookList) return;  // state moved on while armed — drop
  int idx = -1;
  const int perPage = kGridCols * kGridRows;
  for (int i = 0; i < perPage && _scroll + i < _bookCount; i++) {
    if (_books[_scroll + i].meta == TileMeta::Unknown) {
      idx = _scroll + i;
      break;
    }
  }
  if (idx < 0) return;  // scroll moved past the tiles that armed us
  BookEntry& b = _books[idx];

  // ONE unit per quiet tick: load the book.bin cache, BUILDING it if missing so
  // a never-opened book still gets a title + cover on the grid (build is only
  // the metadata pass — container.xml + content.opf parse + write, no chapter
  // pagination — cheap enough for one-per-quiet-tick and it warms the cache for
  // a later open). Then ensure the cover thumb — the expensive step (zip
  // extract + JPEG/PNG decode on a cache miss; a hit is just an exists() probe).
  // The Epub is transient: copy the strings into the fixed entry buffers and
  // drop it. A load failure here now means the book is genuinely unreadable.
  const uint32_t t0 = millis();
  const std::unique_ptr<reader::Epub> epub(new (std::nothrow)
                                               reader::Epub(b.path, reader::kReaderCacheRoot));
  if (!epub || !epub->load(/*buildIfMissing=*/true)) {
    b.meta = TileMeta::NotOpened;
  } else {
    const std::string& title = epub->getTitle();
    snprintf(b.title, sizeof(b.title), "%s", title.empty() ? baseName(b.path) : title.c_str());
    snprintf(b.author, sizeof(b.author), "%s", epub->getAuthor().c_str());
    b.meta = TileMeta::NoCover;  // coverless AND transient failures render text-only
    std::string thumbPath;
    if (reader::CoverThumb::ensure(*epub, kThumbW, kThumbH, &thumbPath) &&
        thumbPath.size() < sizeof(b.thumbPath) && readThumbDims(thumbPath.c_str(), &b.thumbW, &b.thumbH)) {
      memcpy(b.thumbPath, thumbPath.c_str(), thumbPath.size() + 1);
      b.meta = TileMeta::Cover;
    }
  }
  Serial.printf("[xphone-os] reader: grid meta '%s' -> %d (%lu ms) heap=%u\n", baseName(b.path),
                static_cast<int>(b.meta), static_cast<unsigned long>(millis() - t0), ESP.getFreeHeap());
  // Repaint just this tile; that render re-arms GridMeta if visible tiles
  // still need work (renderBookList owns the arming).
  markDirty(tileRect(idx - _scroll));
}

// --- Engine plumbing -----------------------------------------------------------

void ReaderScene::ensureSectionOrIndex() {
  _section.reset();
  auto* s = new (std::nothrow) reader::Section(_epub, _spine, *_measure);
  if (!s) {
    failWith("Out of memory loading chapter");
    return;
  }
  _section.reset(s);
  if (_section->loadSectionFile(_settings)) {
    applyPendingPage();
    _state = State::Reading;
    reader::ReadingStats::sessionStart(_bookPath);
  } else {
    // Cache missing (or stale — loadSectionFile cleared it): show the
    // Indexing frame first, build on the next idle tick.
    _state = State::Indexing;
    _work = Work::BuildSection;
    _workArmed = false;
  }
  markDirty();
}

void ReaderScene::applyPendingPage() {
  int page;
  if (_hasPendingRatio) {
    // Font-size cycle: ratio -> page needs the NEW pageCount, only known now.
    page = static_cast<int>(_pendingRatio * _section->pageCount + 0.5f);
    _hasPendingRatio = false;
  } else if (_nextPage == kLastPageSentinel) {
    page = _section->pageCount > 0 ? _section->pageCount - 1 : 0;
  } else {
    page = static_cast<int>(_nextPage);
  }
  if (_section->pageCount > 0 && page >= _section->pageCount) page = _section->pageCount - 1;
  if (page < 0) page = 0;
  _section->currentPage = page;
  _nextPage = 0;
}

void ReaderScene::loadProgress() {
  _spine = 0;
  _nextPage = 0;
  _hadProgress = false;

  HalFile f;
  if (Storage.openFileForRead("RDR", _epub->getCachePath() + "/progress.bin", f)) {
    uint8_t d[6];
    const int n = f.read(d, sizeof(d));
    if (n == 4 || n == 6) {
      _spine = d[0] | (d[1] << 8);
      _nextPage = static_cast<uint16_t>(d[2] | (d[3] << 8));
      if (_nextPage == kLastPageSentinel) _nextPage = 0;  // stale nav sentinel
      _hadProgress = true;
    }
  }

  const int count = _epub->getSpineItemsCount();
  if (_spine >= count) {
    _spine = count > 0 ? count - 1 : 0;
    _nextPage = 0;
  }
  if (_spine < 0) _spine = 0;
  if (!_hadProgress && _spine == 0) {
    // First open: skip cover/frontmatter via the OPF text reference.
    _spine = _epub->getSpineIndexForTextReference();
  }
}

void ReaderScene::saveProgress() {
  if (!_epub || !_section) return;
  const int page = _section->currentPage;
  const int pc = _section->pageCount;
  const uint8_t d[6] = {
      static_cast<uint8_t>(_spine & 0xFF), static_cast<uint8_t>((_spine >> 8) & 0xFF),
      static_cast<uint8_t>(page & 0xFF),   static_cast<uint8_t>((page >> 8) & 0xFF),
      static_cast<uint8_t>(pc & 0xFF),     static_cast<uint8_t>((pc >> 8) & 0xFF),
  };
  ProgressFile::writeAtomic(_epub->getCachePath(), d, sizeof(d));
}

void ReaderScene::maybeArmPrefetch() {
  if (_work != Work::None || !_epub || !_section || _section->pageCount == 0) return;
  const int next = _spine + 1;
  if (next >= _epub->getSpineItemsCount() || _prefetchAttemptedSpine == next) return;
  // Only once the reader is in the last 30% of the chapter.
  if ((_section->currentPage + 1) * 10 < static_cast<int>(_section->pageCount) * 7) return;

  const std::string nextPath = _epub->getCachePath() + "/sections/" + std::to_string(next) + ".bin";
  if (Storage.exists(nextPath.c_str())) {
    _prefetchAttemptedSpine = next;  // present; staleness is handled at nav time
    return;
  }
  _work = Work::PrefetchNext;  // armed by this render, runs on a quiet idle tick
}

// --- Input ----------------------------------------------------------------------

void ReaderScene::handleInput(Input& in) {
  // Deferred work: run only after its announcing frame reached glass (armed
  // by render(), flush worker idle). The silent kinds (prefetch, grid
  // metadata) yield to pending input.
  if (_work != Work::None && _workArmed && !SCENES.flushInFlight()) {
    const bool inputWaiting = in.wasPressed(Btn::Left) || in.wasPressed(Btn::Right) || in.wasPressed(Btn::Up) ||
                              in.wasPressed(Btn::Down) || in.wasPressed(Btn::Back) || in.wasPressed(Btn::Confirm);
    const bool yieldsToInput = _work == Work::PrefetchNext || _work == Work::GridMeta;
    if (!yieldsToInput || !inputWaiting) {
      runWork();
      return;
    }
  }

  switch (_state) {
    case State::Opening:
    case State::Indexing:
      return;  // busy frame on glass; input resumes when the work lands

    case State::Error:
      if (in.wasPressed(Btn::Back)) enterBookList();
      return;

    case State::Reading:
      if (in.wasPressed(Btn::Right) || in.wasPressed(Btn::Down)) {
        pageTurn(/*forward=*/true);
      } else if (in.wasPressed(Btn::Left) || in.wasPressed(Btn::Up)) {
        pageTurn(/*forward=*/false);
      } else if (in.wasPressed(Btn::Confirm)) {
        cycleFontSize();  // "SIZE" soft key
      } else if (in.wasPressed(Btn::Back)) {
        enterBookList();
      }
      return;

    case State::BookList:
      if (in.wasPressed(Btn::Back)) {
        if (_work == Work::GridMeta) _work = Work::None;  // don't scan behind another state
        // Always home, even with a book open — bouncing back into the book
        // trapped the user in a book <-> list loop. Progress is saved, so
        // reopening from the grid (or the resume path in onEnter) is cheap.
        showLauncher();
        return;
      }
      if (in.wasPressed(Btn::Confirm) && _bookCount > 0) {
        openSelectedBook();  // overwrites any pending GridMeta — opens never wait on thumbs
        return;
      }
      // Grid nav: front Left/Right step one book, top Up/Down one ROW.
      if (in.wasPressed(Btn::Up)) moveSelection(-kGridCols);
      if (in.wasPressed(Btn::Down)) moveSelection(+kGridCols);
      if (in.wasPressed(Btn::Left)) moveSelection(-1);
      if (in.wasPressed(Btn::Right)) moveSelection(+1);
      return;
  }
}

void ReaderScene::pageTurn(const bool forward) {
  if (!_epub || !_section) return;
  reader::ReadingStats::pageTurn();
  if (forward) {
    if (_section->currentPage + 1 < static_cast<int>(_section->pageCount)) {
      _section->currentPage++;
      markDirty();  // full-panel: the flush worker + FAST/HALF cadence handle the rest
    } else if (_spine + 1 < _epub->getSpineItemsCount()) {
      _spine++;
      _nextPage = 0;
      ensureSectionOrIndex();
    }
    // else: final page of the book — stay put.
  } else {
    if (_section->currentPage > 0) {
      _section->currentPage--;
      markDirty();
    } else if (_spine > 0) {
      _spine--;
      _nextPage = kLastPageSentinel;  // land on the previous chapter's LAST page
      ensureSectionOrIndex();
    }
  }
}

// CONFIRM in Reading: cycle 12 -> 14 -> 16 -> 12 pt. fontId is part of the
// section.bin cache key (settings-in-header), so ensureSectionOrIndex()
// reloads a previously-built cache at that size instantly, or falls into the
// normal Indexing rebuild. Other chapters rebuild lazily on nav via the same
// header-mismatch path.
void ReaderScene::cycleFontSize() {
  if (!_epub || !_section || !_measure) return;
  const int nextId = (_settings.fontId + 1) % reader::kReaderFontCount;
  auto* m = new (std::nothrow) reader::TextMeasure(reader::readerFontFamily(nextId));
  if (m == nullptr) return;  // transient OOM: stay at the current size, keep reading

  // Position survives as a chapter ratio — the page index only translates
  // once the re-laid-out section reports its NEW pageCount (applyPendingPage).
  _pendingRatio =
      _section->pageCount > 0 ? static_cast<float>(_section->currentPage) / _section->pageCount : 0.0f;
  _hasPendingRatio = true;
  _settings.fontId = nextId;
  {
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, /*readOnly=*/false)) {
      prefs.putUChar(kPrefsFontKey, static_cast<uint8_t>(nextId));
      prefs.end();
    }
  }
  _section.reset();  // the old section layout references the outgoing TextMeasure
  _measure.reset(m);
  _prefetchAttemptedSpine = -1;  // next chapter re-prefetches at the new size
  Serial.printf("[xphone-os] reader: font size -> id %d (ratio %.3f)\n", nextId,
                static_cast<double>(_pendingRatio));
  ensureSectionOrIndex();
}

// --- Book list -------------------------------------------------------------------

void ReaderScene::enterBookList() {
  reader::ReadingStats::sessionEnd();  // flush before the grid repaints (band shows fresh numbers)

  scanBooks();
  _state = State::BookList;
  markDirty();
}

void ReaderScene::scanBooks() {
  _bookCount = 0;
  _sel = 0;
  _scroll = 0;
  scanDir("/books");
  scanDir("/");

  // Alphabetical by display name (insertion sort; <= 32 entries).
  for (int i = 1; i < _bookCount; i++) {
    BookEntry key = _books[i];
    int j = i - 1;
    while (j >= 0 && strcasecmp(baseName(_books[j].path), baseName(key.path)) > 0) {
      _books[j + 1] = _books[j];
      j--;
    }
    _books[j + 1] = key;
  }

  // Fast-pass: tiles whose caches already exist paint complete on the FIRST
  // render. Reopening the Reader used to visibly re-upgrade every tile
  // one-per-quiet-tick even though each was a ~30 ms cache hit; that belongs
  // before first paint. Anything needing real work (book.bin build, cover
  // decode) stays Unknown for the GridMeta tick.
  for (int i = 0; i < _bookCount; i++) {
    BookEntry& b = _books[i];
    const std::unique_ptr<reader::Epub> epub(new (std::nothrow)
                                                 reader::Epub(b.path, reader::kReaderCacheRoot));
    if (!epub || !epub->load(/*buildIfMissing=*/false)) continue;
    const std::string& title = epub->getTitle();
    snprintf(b.title, sizeof(b.title), "%s", title.empty() ? baseName(b.path) : title.c_str());
    snprintf(b.author, sizeof(b.author), "%s", epub->getAuthor().c_str());
    std::string thumbPath;
    const int hit = reader::CoverThumb::probe(*epub, kThumbW, kThumbH, &thumbPath);
    if (hit == 1 && thumbPath.size() < sizeof(b.thumbPath) &&
        readThumbDims(thumbPath.c_str(), &b.thumbW, &b.thumbH)) {
      memcpy(b.thumbPath, thumbPath.c_str(), thumbPath.size() + 1);
      b.meta = TileMeta::Cover;
    } else if (hit == 0) {
      b.meta = TileMeta::NoCover;
    }
  }
}

void ReaderScene::scanDir(const char* dir) {
  // Directory iteration needs SdFat's FsFile::openNext — below the ReaderFs
  // shim's surface, so use the SDK manager directly (same pattern as
  // SdUpdate.cpp; all SD access stays on the main loop task).
  if (!SdMan.ready() && !SdMan.begin()) return;
  FsFile d = SdMan.open(dir, O_RDONLY);
  if (!d || !d.isDir()) return;

  FsFile f;
  while (_bookCount < kMaxBooks && f.openNext(&d, O_RDONLY)) {
    if (!f.isDir()) {
      char name[80];
      if (f.getName(name, sizeof(name)) > 0 && name[0] != '.' && endsWithEpubCI(name)) {
        BookEntry& b = _books[_bookCount];
        // Fresh entry, meta reset to Unknown (rescan: paths may have changed;
        // grid work reloads titles/thumbs lazily).
        memset(&b, 0, sizeof(b));
        const bool isRoot = (dir[0] == '/' && dir[1] == '\0');
        const int n = snprintf(b.path, sizeof(b.path), "%s/%s", isRoot ? "" : dir, name);
        if (n > 0 && n < static_cast<int>(sizeof(b.path))) {
          _bookCount++;  // paths longer than the fixed buffer are skipped
        }
      }
    }
    f.close();
  }
  d.close();
}

void ReaderScene::openSelectedBook() {
  if (_sel < 0 || _sel >= _bookCount) return;

  // Full reopen (even for the currently open book — progress.bin preserves
  // the position, and the path is one code path).
  _section.reset();
  _epub.reset();
  _measure.reset();
  _bookPath = _books[_sel].path;
  {
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, /*readOnly=*/false)) {
      prefs.putString(kPrefsBookKey, _bookPath.c_str());
      prefs.end();
    }
  }
  _spine = 0;
  _nextPage = 0;
  _hasPendingRatio = false;
  _prefetchAttemptedSpine = -1;
  _pageLoadRetries = 0;
  _state = State::Opening;
  _work = Work::OpenBook;  // replaces any pending GridMeta — opens never wait on thumbs
  _workArmed = false;
  markDirty();
}

XpRect ReaderScene::listRect() const {
  if (_hCache <= 0) return XpRect{};  // no layout yet -> full-panel fallback
  return XpRect{0, kHeaderH + kStatsBandH, _wCache,
                static_cast<int16_t>(_hCache - kHeaderH - kStatsBandH - Scene::SOFTKEY_BAR_H)};
}

XpRect ReaderScene::tileRect(const int visibleIndex) const {
  if (_hCache <= 0 || visibleIndex < 0) return XpRect{};
  const int16_t tileW = static_cast<int16_t>(_wCache / kGridCols);
  const int16_t tileH = static_cast<int16_t>(
      (_hCache - kHeaderH - kStatsBandH - Scene::SOFTKEY_BAR_H) / kGridRows);
  const int col = visibleIndex % kGridCols;
  const int row = visibleIndex / kGridCols;
  return XpRect{static_cast<int16_t>(col * tileW),
                static_cast<int16_t>(kHeaderH + kStatsBandH + row * tileH), tileW, tileH};
}

void ReaderScene::moveSelection(const int delta) {
  int sel = _sel + delta;  // row moves clamp into the ends, not off them
  if (sel > _bookCount - 1) sel = _bookCount - 1;
  if (sel < 0) sel = 0;
  if (sel == _sel) return;
  const int prev = _sel;
  _sel = sel;

  // Scroll by whole ROWS so _scroll stays a multiple of kGridCols.
  const int perPage = kGridCols * kGridRows;
  const int oldScroll = _scroll;
  while (_sel < _scroll) _scroll -= kGridCols;
  while (_sel >= _scroll + perPage) _scroll += kGridCols;
  if (_scroll < 0) _scroll = 0;
  if (_scroll != oldScroll) {
    markDirty(listRect());  // tiles shifted — repaint the whole grid region
    return;
  }
  XpRect dirty = tileRect(prev - _scroll);
  dirty.unionWith(tileRect(_sel - _scroll));
  markDirty(dirty);
}

// --- Render ---------------------------------------------------------------------

void ReaderScene::render(Gfx& gfx) {
  _fbForLoan = gfx.display().getFrameBuffer();  // for runWork()'s dict loan
  _wCache = static_cast<int16_t>(gfx.width());
  _hCache = static_cast<int16_t>(gfx.height());
  // Text viewport from the actual panel (resolution-agnostic; part of the
  // section.bin cache key). Constant per device, so caches stay valid.
  _settings.viewportWidth = static_cast<uint16_t>(_wCache - 2 * kMarginX);
  _settings.viewportHeight = static_cast<uint16_t>(_hCache - kMarginTop - kStatusH - Scene::SOFTKEY_BAR_H);

  renderBody(gfx);

  // Arm pending deferred work: the frame announcing it is now composed and
  // will be dispatched to glass right after this render returns.
  if (_work != Work::None) _workArmed = true;
}

void ReaderScene::renderBody(Gfx& gfx) {
  switch (_state) {
    case State::Opening:
      renderMessage(gfx, "Opening book...", baseName(_bookPath.c_str()));
      return;
    case State::Indexing: {
      char info[40];
      snprintf(info, sizeof(info), "chapter %d of %d", _spine + 1, _epub ? _epub->getSpineItemsCount() : 0);
      renderMessage(gfx, "Indexing chapter...", info);
      return;
    }
    case State::Error:
      renderMessage(gfx, "Reader", _errorMsg);
      return;
    case State::BookList:
      renderBookList(gfx);
      return;
    case State::Reading:
      renderReading(gfx);
      return;
  }
}

void ReaderScene::renderMessage(Gfx& gfx, const char* line1, const char* line2) {
  const int cx = gfx.width() / 2;
  const int cy = gfx.height() / 2;
  if (line1 && line1[0]) gfx.drawTextCentered(kFontBold, cx, cy - gfx.lineHeight(kFontBold), line1);
  if (line2 && line2[0]) {
    char clipped[96];
    truncateToWidth(gfx, kFontRegular, line2, gfx.width() - 2 * kListMarginX, clipped, sizeof(clipped));
    gfx.drawTextCentered(kFontRegular, cx, cy + 6, clipped);
  }
}

void ReaderScene::renderReading(Gfx& gfx) {
  if (!_epub || !_section || !_measure) {  // defensive: should be unreachable
    renderMessage(gfx, "Reader", "No book open");
    return;
  }

  if (_section->pageCount == 0) {
    renderMessage(gfx, nullptr, "(empty chapter)");
    renderStatusLine(gfx);
    saveProgress();
    return;
  }

  if (_section->currentPage >= static_cast<int>(_section->pageCount)) {
    _section->currentPage = _section->pageCount - 1;
  }
  if (_section->currentPage < 0) _section->currentPage = 0;

  {
    // CrossPoint replay model: the Page lives ONLY inside this render — it is
    // deserialized, composed into the framebuffer, and freed at scope exit.
    CpuBoost boost;  // page compose is pure CPU; ~halves at 160 MHz
    auto page = _section->loadPageFromSectionFile();
    if (!page) {
      if (_pageLoadRetries++ < 1) {
        // Corrupt/torn page cache: clear it and rebuild once via the normal
        // Indexing flow, preserving the position.
        Serial.println("[xphone-os] reader: page deserialize failed — rebuilding section cache");
        _nextPage = static_cast<uint16_t>(_section->currentPage);
        _section->clearCache();
        _state = State::Indexing;
        _work = Work::BuildSection;
        char info[40];
        snprintf(info, sizeof(info), "chapter %d of %d", _spine + 1, _epub->getSpineItemsCount());
        renderMessage(gfx, "Indexing chapter...", info);
        return;
      }
      failWith("Couldn't read this chapter");
      renderMessage(gfx, "Reader", _errorMsg);
      return;
    }
    _pageLoadRetries = 0;

    const uint32_t t0 = millis();
    _renderer.renderPage(gfx, *page, *_measure, kMarginX, kMarginTop);
    Serial.printf("[xphone-os] reader: page %d/%u composed in %lu ms\n", _section->currentPage + 1,
                  _section->pageCount, static_cast<unsigned long>(millis() - t0));
  }

  renderStatusLine(gfx);
  saveProgress();     // crash-safe resume: 6 bytes, atomic, every page
  maybeArmPrefetch();  // silent next-chapter indexing near the chapter end
}

void ReaderScene::renderStatusLine(Gfx& gfx) {
  // "34%  ·  ch 3/12" — the ONLY reading chrome. The UI fonts are ASCII-only
  // subsets, so the middle dot is stamped as a 3x3 square instead of U+00B7.
  const int count = _section ? static_cast<int>(_section->pageCount) : 0;
  const float chapterProgress = count > 0 ? static_cast<float>(_section->currentPage + 1) / count : 0.0f;
  int pct = static_cast<int>(_epub->calculateProgress(_spine, chapterProgress) * 100.0f + 0.5f);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;

  char left[8];
  char right[24];
  snprintf(left, sizeof(left), "%d%%", pct);
  snprintf(right, sizeof(right), "ch %d/%d", _spine + 1, _epub->getSpineItemsCount());

  const int lineH = gfx.lineHeight(kFontSmall);
  const int y = gfx.height() - Scene::SOFTKEY_BAR_H - kStatusH + (kStatusH - lineH) / 2;
  constexpr int kDot = 3;
  constexpr int kGap = 12;
  const int wl = gfx.textWidth(kFontSmall, left);
  const int wr = gfx.textWidth(kFontSmall, right);
  const int total = wl + kGap + kDot + kGap + wr;
  const int x = (gfx.width() - total) / 2;
  gfx.drawText(kFontSmall, x, y, left);
  gfx.fillRect(x + wl + kGap, y + lineH / 2 - kDot / 2, kDot, kDot, true);
  gfx.drawText(kFontSmall, x + wl + kGap + kDot + kGap, y, right);
}

void ReaderScene::renderBookList(Gfx& gfx) {
  const int w = gfx.width();
  const int perPage = kGridCols * kGridRows;
  // Clamp against a shrunken list (rescan may have removed entries); the
  // scroll stays row-aligned.
  if (_sel > _bookCount - 1) _sel = _bookCount > 0 ? _bookCount - 1 : 0;
  if (_scroll > _sel) _scroll = (_sel / kGridCols) * kGridCols;
  if (_sel >= _scroll + perPage) _scroll = (_sel / kGridCols - (kGridRows - 1)) * kGridCols;
  if (_scroll < 0) _scroll = 0;

  // Header.
  char line[64];
  snprintf(line, sizeof(line), "Reader (%d)", _bookCount);
  gfx.drawText(kFontBold, kListMarginX, 8, line);
  if (_bookCount > perPage) {
    snprintf(line, sizeof(line), "%d-%d", _scroll + 1, (_scroll + perPage < _bookCount) ? _scroll + perPage : _bookCount);
    gfx.drawText(kFontRegular, w - kListMarginX - gfx.textWidth(kFontRegular, line), 8, line);
  }
  gfx.fillRect(0, kHeaderH - 2, w, 2, true);

  // ── Stats band (concept A, compact): "N DAY STREAK   N PAGES TODAY"
  // inline on one baseline, THIS calendar week Mon–Sun as bars on the right
  // (trailing-7 ordering read as alphabet soup — "TWTFSSM" — on hardware).
  {
    const reader::ReadingStats::Band sb = reader::ReadingStats::band();
    const int bandY = kHeaderH;
    const int numY = bandY + 10;
    const int capNudge = gfx.lineHeight(kFontBold) - gfx.lineHeight(kFontSmall);
    char num[16];

    if (!sb.clockValid) {
      // No phone time since boot: totals still count, days can't. Say so
      // instead of drawing dash numerals and a meaningless week.
      gfx.drawText(kFontSmall, kListMarginX, numY + capNudge, "Sync iPhone to track streaks");
    } else {
      int x = kListMarginX;
      snprintf(num, sizeof(num), "%u", sb.streakDays);
      gfx.drawText(kFontBold, x, numY, num);
      x += gfx.textWidth(kFontBold, num) + 6;
      gfx.drawText(kFontSmall, x, numY + capNudge, "DAY STREAK");
      x += gfx.textWidth(kFontSmall, "DAY STREAK") + 24;
      snprintf(num, sizeof(num), "%u", sb.todayPages);
      gfx.drawText(kFontBold, x, numY, num);
      x += gfx.textWidth(kFontBold, num) + 6;
      gfx.drawText(kFontSmall, x, numY + capNudge, "PAGES TODAY");

      // Mon..Sun of the current week; future days render as hairline stubs.
      // One type weight for all seven letters, centered under their bars:
      // the old 13 px pitch let adjacent W/M glyphs touch, and the faux-bold
      // today letter outgrew its cell and overlapped. Today is marked with a
      // 2 px underline instead — emphasis that cannot change glyph metrics.
      const int barW = 9, barGap = 8, barMaxH = 26;
      const int blockW = 7 * barW + 6 * barGap;
      const int barX0 = w - kListMarginX - blockW;
      const int barBase = bandY + 4 + barMaxH;
      uint16_t maxPages = 1;
      for (int i = 0; i < 7; i++) {
        if (sb.weekPages[i] > maxPages) maxPages = sb.weekPages[i];
      }
      static const char* kDow = "MTWTFSS";
      for (int slot = 0; slot < 7; slot++) {
        const int bx = barX0 + slot * (barW + barGap);
        const int daysBack = static_cast<int>(sb.todayWeekday) - slot;
        int h = 1;  // future days & zero days: hairline stub
        if (daysBack >= 0) {
          const uint16_t p = sb.weekPages[6 - daysBack];
          if (p) h = 2 + (barMaxH - 2) * p / maxPages;
        }
        gfx.fillRect(bx, barBase - h, barW, h, true);
        const char letter[2] = {kDow[slot], 0};
        const int lw = gfx.textWidth(kFontSmall, letter);
        gfx.drawText(kFontSmall, bx + (barW - lw) / 2, barBase + 3, letter);
        if (slot == sb.todayWeekday) {
          gfx.fillRect(bx, barBase + 3 + gfx.lineHeight(kFontSmall) + 1, barW, 2, true);
        }
      }
    }

    gfx.fillRect(0, kHeaderH + kStatsBandH - 2, w, 2, true);
  }

  if (_bookCount == 0) {
    gfx.drawTextCentered(kFontBold, w / 2, gfx.height() / 2 - gfx.lineHeight(kFontBold), "No books found");
    gfx.drawTextCentered(kFontRegular, w / 2, gfx.height() / 2 + 6, "Put .epub files in /books on the SD card");
    return;
  }

  bool tilesNeedWork = false;
  for (int i = 0; i < perPage && _scroll + i < _bookCount; i++) {
    renderTile(gfx, i);
    if (_books[_scroll + i].meta == TileMeta::Unknown) tilesNeedWork = true;
  }
  // Lazy metadata + thumbs: one tile per quiet tick via the standard
  // arm-then-run dance. Only claims the Work slot when it is free (an
  // OpenBook queued this same tick must win).
  if (tilesNeedWork && _work == Work::None) _work = Work::GridMeta;
}

void ReaderScene::renderTile(Gfx& gfx, const int visibleIndex) {
  const int idx = _scroll + visibleIndex;
  const BookEntry& b = _books[idx];
  const XpRect r = tileRect(visibleIndex);

  const int cx = r.x + r.w / 2;
  const int thumbTop = r.y + kThumbTop;

  // Selection: rounded border around the COVER BOX, not the whole tile — the
  // stats band ate the tile's bottom slack, and a full-tile border sliced
  // through the author line on hardware.
  if (idx == _sel) {
    gfx.drawRoundedRect(cx - kThumbW / 2 - kSelInset - kSelThick, thumbTop - kSelInset - kSelThick,
                        kThumbW + 2 * (kSelInset + kSelThick), kThumbH + 2 * (kSelInset + kSelThick),
                        kSelRadius, kSelThick, true);
  }
  bool drewThumb = false;
  if (b.meta == TileMeta::Cover) {
    // Center the aspect-fit thumb (dims cached at grid-work time) in the box.
    drewThumb = reader::CoverThumb::draw(gfx, b.thumbPath, cx - b.thumbW / 2, thumbTop + (kThumbH - b.thumbH) / 2);
  }
  if (!drewThumb) {
    // Placeholder frame where the cover would sit; text-only "covers" carry
    // the wrapped title inside it.
    const int fx = cx - kThumbW / 2;
    gfx.drawRoundedRect(fx, thumbTop, kThumbW, kThumbH, 8, 2, true);
    if (b.meta == TileMeta::NoCover && b.title[0] != '\0') {
      const int maxLines = (kThumbH - 40) / gfx.lineHeight(kFontRegular);
      gfx.drawTextWrapped(kFontRegular, fx + 14, thumbTop + 20, b.title, kThumbW - 28, maxLines);
    }
  }

  // Text block under the cover box: title (bold once known, basename until
  // then) over a small author/status line.
  const bool loaded = b.meta == TileMeta::Cover || b.meta == TileMeta::NoCover;
  const XpFont& titleFont = loaded ? kFontBold : kFontRegular;
  const char* titleSrc = loaded ? b.title : baseName(b.path);
  const char* sub = b.meta == TileMeta::Unknown     ? "..."
                    : b.meta == TileMeta::NotOpened ? "unreadable"
                                                    : b.author;
  const int textW = r.w - 2 * kTileTextPad;
  const int titleY = thumbTop + kThumbH + kTitleGap;
  char clipped[96];
  truncateToWidth(gfx, titleFont, titleSrc, textW, clipped, sizeof(clipped));
  gfx.drawTextCentered(titleFont, cx, titleY, clipped);
  if (sub[0] != '\0') {
    truncateToWidth(gfx, kFontSmall, sub, textW, clipped, sizeof(clipped));
    gfx.drawTextCentered(kFontSmall, cx, titleY + gfx.lineHeight(titleFont) + 2, clipped);
  }
}
