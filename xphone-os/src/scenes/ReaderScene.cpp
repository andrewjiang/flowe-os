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
#include "../reader/FbpBook.h"
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
// Reading orientation is GLOBAL and sticky, the way Kindle, Kobo and
// CrossPoint all scope it — rotation is a fact about the device you are
// holding, not about the book (docs/research/2026-08-16-orientation-scope.md).
// A book whose package carries no landscape profiles opens portrait WITHOUT
// clearing this: the preference records intent, the package decides capability.
constexpr const char* kPrefsLandKey = "rdLand";  // 1 = landscape

// Layout (logical portrait). The text viewport is part of the section.bin
// cache key (settings-in-header), so these only change with a cache rebuild.
constexpr int kMarginX = 24;
constexpr int kMarginTop = 24;
constexpr int kStatusH = 24;  // status line strip above the soft-key bar

// Right edge available to page content: in landscape the soft-key column
// occupies that side, so every full-page screen has to stop before it.
// The buttons do not move when the panel rotates, so the content viewport
// is the panel MINUS that column — this is the width every landscape
// profile is compiled for.
int contentRight(Gfx& gfx, int margin) {
  const int reserve = gfx.orientation() == Gfx::Orient::Landscape ? Scene::SOFTKEY_BAR_H : 0;
  return gfx.width() - reserve - margin;
}

// The partner rule for the BOTTOM edge. Portrait reserves the soft-key bar
// there; landscape does not, because the keys moved to the right — so a
// landscape screen that still subtracted SOFTKEY_BAR_H left a 44 px white
// band along the bottom and squeezed its content for nothing.
int contentBottom(Gfx& gfx, int margin) {
  const int reserve = gfx.orientation() == Gfx::Orient::Landscape ? 8 : Scene::SOFTKEY_BAR_H;
  return gfx.height() - reserve - margin;
}

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
// Stats band. 46, not 56 (Andrew, 2026-08-17: "in general the font in that
// area could be a little smaller, so it is overall tighter"). 10 pt IS the
// smallest face the firmware has — there are only three — so "smaller" is
// bought with tighter bars, a tighter letter pitch and a shorter band. The
// old 56 did not even fit its own contents: bars 26 + letters + the today
// underline came to 60 and the underline crossed the band rule. Every
// pixel freed here goes to the tiles, whose budget was equally tight.
constexpr int kStatsBandH = 46;
constexpr int kThumbW = 200;      // cover thumb target box (aspect-fit inside)
constexpr int kThumbH = 260;
constexpr int kSelInset = 3;      // selection border inset from the cover box
constexpr int kSelRadius = 10;
constexpr int kSelThick = 3;
constexpr int kThumbTop = 6;      // tile top -> thumb box (tightened for the stats band)
// Thumb box bottom -> title line. The tile budget is exact and it does not
// forgive: on the X3 a tile is (792 - 46 header - 56 band - 44 keys) / 2 = 323
// px, and the contents are kThumbTop + 260 cover + kTitleGap + 29 title + 24
// author. At the old 8/6 those came to 329 — six px MORE than the tile — so
// the author line ran under the next row, where the row below's selection
// border then sliced through it. Every one of these numbers is load-bearing.
constexpr int kTitleGap = 4;
constexpr int kTileTextPad = 14;  // horizontal padding for tile text

bool endsWithEpubCI(const char* name) {
  const size_t len = strlen(name);
  if (len < 6) return false;
  const char* ext = name + len - 5;
  return tolower(ext[0]) == '.' && tolower(ext[1]) == 'e' && tolower(ext[2]) == 'p' && tolower(ext[3]) == 'u' &&
         tolower(ext[4]) == 'b';
}

bool endsWithFbpCI(const char* name) {
  const size_t len = strlen(name);
  if (len < 5) return false;
  const char* ext = name + len - 4;
  return ext[0] == '.' && tolower(ext[1]) == 'f' && tolower(ext[2]) == 'b' && tolower(ext[3]) == 'p';
}

const char* baseName(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

// Filename -> display title, the apps' prettify rule: URL-decode, '+'/'_'
// become spaces, the Calibre " -- author" tail drops, the extension drops,
// runs of spaces collapse. For the fallback path only — real metadata wins.
void prettyFileTitle(const char* path, char* out, const size_t outSize) {
  const char* name = baseName(path);
  size_t w = 0;
  bool pendingSpace = false;
  for (size_t r = 0; name[r] != '\0' && w + 1 < outSize;) {
    char c = name[r];
    if (c == '%' && isxdigit(static_cast<uint8_t>(name[r + 1])) &&
        isxdigit(static_cast<uint8_t>(name[r + 2]))) {
      const char hex[3] = {name[r + 1], name[r + 2], '\0'};
      c = static_cast<char>(strtol(hex, nullptr, 16));
      r += 3;
    } else {
      r += 1;
    }
    if (c == '+' || c == '_') c = ' ';
    if (c == ' ') {
      if (w > 0) pendingSpace = true;
      continue;
    }
    if (pendingSpace && w + 2 < outSize) { out[w++] = ' '; pendingSpace = false; }
    out[w++] = c;
  }
  out[w] = '\0';
  char* dashes = strstr(out, " -- ");
  if (dashes) *dashes = '\0';
  // Strip EVERY container extension, not only the last one. A sideloaded
  // "alice.epub.fbp" used to reach the shelf as "alice.epub": the tile named
  // a file format at a reader, which is the app's job to hide. .pdf and the
  // like keep their extension on purpose — those are files the reader
  // recognises as files, not books we compiled.
  for (;;) {
    char* dot = strrchr(out, '.');
    if (!dot || dot == out) break;
    if (strcasecmp(dot, ".epub") != 0 && strcasecmp(dot, ".fbp") != 0 &&
        strcasecmp(dot, ".txt") != 0) {
      break;
    }
    *dot = '\0';
  }
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
    // Trim the space the chop can leave, so tiles never read "Alice ..."
    while (len > 0 && dst[len - 1] == ' ') dst[--len] = '\0';
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

// F6 (2026-08-19): publish a raw EPUB's cover thumb beside the book as
// <path>.cov, the same place and format a compiled package's lands in.
//
// The phone shows covers for books that live only on the card by fetching
// that sidecar over the file-transfer server. A package already writes one
// (FbpBook::ensureShelfSidecars); an EPUB's thumb only ever existed inside
// the reader's cache directory, which the server does not serve — so a
// sideloaded EPUB stayed a blank tile on the phone. One copy of about 6 KB,
// once per book, and only when the thumb has just been built.
void publishCoverSidecar(const char* bookPath, const char* thumbPath) {
  char cov[192];
  if (snprintf(cov, sizeof(cov), "%s.cov", bookPath) >= static_cast<int>(sizeof(cov))) return;
  if (SdMan.exists(cov)) return;
  FsFile src = SdMan.open(thumbPath, O_RDONLY);
  if (!src) return;
  FsFile dst = SdMan.open(cov, O_WRONLY | O_CREAT | O_TRUNC);
  if (!dst) {
    src.close();
    return;
  }
  uint8_t buf[256];
  int n;
  bool ok = true;
  while (ok && (n = src.read(buf, sizeof(buf))) > 0) ok = dst.write(buf, n) == n;
  src.close();
  dst.close();
  if (!ok) SdMan.remove(cov);
}

}  // namespace

// --- Lifecycle ---------------------------------------------------------------

ReaderScene::ReaderScene() = default;   // Epub/Section/TextMeasure complete here
ReaderScene::~ReaderScene() = default;

void ReaderScene::onEnter() {
  // Radio policy (rewritten 2026-08-18, Andrew's call): READING KEEPS THE
  // RADIO. Since glyph right-sizing a composed fbp page peaks ~11 KB and
  // the 32 KB inflate window is lent from the static framebuffer, a book
  // fits beside BLE+ANCS. Only two paths still take turns with the radio:
  // the shelf (the ~58 KB cover-decoder scratch) and the EPUB reader
  // (expat + indexing want the whole heap). The old whole-scene shutdown
  // predates both fixes; its numbers (1,352 B free beside the static dict)
  // no longer exist. Suspends now happen downstream: enterBookList() and
  // the EPUB branch of workOpenBook().

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
      _wantLandscape = prefs.getUChar(kPrefsLandKey, 0) != 0;
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
  // Capture the pagination BEFORE the book state is torn down below, for the
  // place we hand the phone at the end (item 6 step 3).
  const uint16_t exitPageCount = _fbp ? (uint16_t)_fbp->pageCount() : _lastPageCount;
  const uint16_t exitPage = _fbpPage;
  reader::ReadingStats::sessionEnd();  // covers home/sleep exits mid-book
  // Landscape belongs to the reader alone: hand the panel back in portrait
  // so the launcher (and every other scene) lays out as designed.
  if (_landscape && G_GFX) G_GFX->setOrientation(Gfx::Orient::Portrait);
  _landscape = false;
  _pendingOrient = -1;
  _pendingRestorePortrait = false;
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

  // Item 6 step 3: hand the phone where we got to. BLE was suspended while
  // reading, so book-close is the first moment it can hear us. Queue the
  // last-read place (already written to .pos on every turn); the pump sends
  // it once the radio is back and the link is encrypted.
  if (!_bookPath.empty() && endsWithFbpCI(_bookPath.c_str())) {
    char key[64];
    reader::FbpBook::canonicalKey(baseName(_bookPath.c_str()), key, sizeof(key));
    COMPANION_BLE.queueReaderPlace(key, exitPage, exitPageCount);
  }

  if (_radioSuspended) {
    _radioSuspended = false;
    COMPANION_BLE.resumeAfterReader();
  }
}

// --- Soft keys -----------------------------------------------------------------

const char* const* ReaderScene::softKeys() const {
  static constexpr const char* kHidden[4] = {nullptr, nullptr, nullptr, nullptr};
  // 0.7 chrome v2: CONFIRM opens the full-page MENU. Labels always tell
  // the truth about the button under them; BOOKS is the shelf whenever
  // the label reads BOOKS.
  // Slots 2 and 3 are the direction pair and are ARROW MARKS, not words —
  // a shape says "one step back" faster than four letters can, and it says
  // it in any language. Words are kept for the VERBS (BOOKS, MENU, SELECT,
  // GO, DONE, BACK, READ, OPEN), which have no direction to show.
  //
  // The landscape tables are the same screens with the pair turned vertical
  // and the up arrow on slot 3, the key that is physically the TOP one there
  // (see the direction-key rule in Scene.h). Where that means the two keys
  // trade jobs, dirSwap() below makes the input agree — the two are read from
  // the same predicate so they cannot drift apart.
  static constexpr const char* kReading[4] = {"BOOKS", "MENU", SoftKey::Left, SoftKey::Right};
  // Turning a page is a LEFT/RIGHT idea, not an up/down one (Andrew,
  // 2026-08-17), so the arrows stay horizontal when the panel turns —
  // only which key carries which action flips, so that "next" is the
  // lower key your thumb rests on.
  static constexpr const char* kReadingL[4] = {"BOOKS", "MENU", SoftKey::Right, SoftKey::Left};
  // Inside the menu, BACK always means "up one level" (from the menu page,
  // up is the book) and slot 1 always acts on the cursor. Leaving the book
  // is the explicit "Close book" row, not a hidden button meaning.
  // SELECT is six letters and will not stack legibly in a landscape tab, so
  // it abbreviates to OK there — the honest fallback, not a crushed word.
  static constexpr const char* kMenuPage[4] = {"BACK", "SELECT", SoftKey::Left, SoftKey::Right};
  static constexpr const char* kMenuPageL[4] = {"BACK", "OK", SoftKey::Right, SoftKey::Left};
  // Text size is the one pair that is NOT a direction, so it keeps its words
  // in both orientations — an arrow would only say "this way", where "A-" and
  // "A+" say what actually happens. Two characters stack fine in a landscape
  // tab. It is also the one pair that does not swap when the panel turns:
  // "+" belongs on the key that becomes the upper one.
  static constexpr const char* kSizeStrip[4] = {"BACK", "DONE", "A-", "A+"};
  static constexpr const char* kChapters[4] = {"BACK", "GO", SoftKey::Left, SoftKey::Right};
  static constexpr const char* kChaptersL[4] = {"BACK", "GO", SoftKey::Right, SoftKey::Left};
  static constexpr const char* kGoTo[4] = {"BACK", "GO", SoftKey::Left, SoftKey::Right};
  static constexpr const char* kGoToL[4] = {"BACK", "GO", SoftKey::Right, SoftKey::Left};
  static constexpr const char* kMarks[4] = {"BACK", "GO", SoftKey::Left, SoftKey::Right};
  static constexpr const char* kMarksL[4] = {"BACK", "GO", SoftKey::Right, SoftKey::Left};
  // With nothing in the list, GO and the cursor keys do nothing. A tab that
  // does nothing is a lie about the button under it.
  static constexpr const char* kMarksEmpty[4] = {"BACK", nullptr, nullptr, nullptr};
  static constexpr const char* kStatsBook[4] = {"BACK", "READ", nullptr, nullptr};
  static constexpr const char* kStatsLife[4] = {"BACK", "READ", nullptr, nullptr};
  static constexpr const char* kList[4] = {"BACK", "OPEN", SoftKey::Left, SoftKey::Right};
  static constexpr const char* kListStats[4] = {"BACK", "OPEN", SoftKey::Left, SoftKey::Right};
  static constexpr const char* kListEmpty[4] = {"BACK", nullptr, nullptr, nullptr};
  static constexpr const char* kLife[4] = {"BACK", nullptr, nullptr, nullptr};
  const bool land = _landscape;
  switch (_state) {
    case State::Reading:
      switch (_menu) {
        case MenuView::Page:      return land ? kMenuPageL : kMenuPage;
        case MenuView::SizeStrip: return kSizeStrip;
        case MenuView::Chapters:  return land ? kChaptersL : kChapters;
        case MenuView::GoTo:      return land ? kGoToL : kGoTo;
        case MenuView::Bookmarks:
          return _markCount == 0 ? kMarksEmpty : (land ? kMarksL : kMarks);
        case MenuView::StatsBook: return kStatsBook;
        case MenuView::StatsLife: return kStatsLife;
        case MenuView::None:      break;
      }
      return land ? kReadingL : kReading;
    case State::BookList:
      if (_lifeOpen) return kLife;
      if (_sel < 0) return kListStats;
      return _totalBooks > 0 ? kList : kListEmpty;
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
  // Leaving the grid: hand the ~58KB cover-decoder scratch back to the heap
  // before anything else claims it.
  reader::CoverThumb::releaseScratch();
  if (endsWithFbpCI(_bookPath.c_str())) {
    // Reading keeps the radio (2026-08-18). Bring the stack up NOW, into
    // the cleanest heap — BEFORE the book's state exists, never beside it
    // (BLE init beside ~60 KB of resident book state hard-hung an X4, see
    // the M4.2 note in main.cpp). resumeAfterReader() no-ops when the
    // radio is already up; this also revives the boot-deferred radios when
    // a restore lands straight in a book.
    _radioSuspended = false;
    COMPANION_BLE.resumeAfterReader();
    workOpenFbp();
    return;
  }
  suspendRadioForBookWork();  // EPUBs still need the quiet heap: expat + indexing
  const uint32_t t0 = millis();
  auto* e = new (std::nothrow) reader::Epub(_bookPath, reader::kReaderCacheRoot);
  if (!e) {
    failWith("Out of memory opening book");
    return;
  }
  _epub.reset(e);
  loadBookmarks();

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

void ReaderScene::workOpenFbp() {
  const uint32_t t0 = millis();
  auto* fb = new (std::nothrow) reader::FbpBook();
  if (!fb || !fb->open(_bookPath.c_str())) {
    delete fb;
    failWith("Couldn't open this book");
    return;
  }
  _fbp.reset(fb);
  // The saved position can be numbered in another build's pagination; it can
  // only be rescaled once the profile (and so the page count) is chosen, and
  // that happens at first render. Defer the read until then.
  _fbpPage = 0;
  _fbpPosPending = true;
  reader::ReadingStats::sessionStart(_bookPath);
  loadBookmarks();
  // Restore the reader's orientation. render() applies it (it holds the Gfx);
  // a package without landscape profiles simply opens portrait and the
  // preference is left alone for the next book that can honour it.
  if (_wantLandscape) {
    _pendingOrient = 1;
    _pendingOrientPersist = false;
  }
  Serial.printf("[xphone-os] reader: opened fbp '%s' (%lu ms) heap=%u\n", _fbp->title(),
                static_cast<unsigned long>(millis() - t0), ESP.getFreeHeap());
  _state = State::Reading;
  markDirty();
}

void ReaderScene::fbpTurn(const bool forward) {
  if (!_fbp) return;
  const uint16_t last = _fbp->pageCount() ? (uint16_t)(_fbp->pageCount() - 1) : 0;
  uint16_t next = _fbpPage;
  if (forward && _fbpPage < last) next++;
  else if (!forward && _fbpPage > 0) next--;
  if (next == _fbpPage) return;
  if (forward) noteTurnPace();
  _fbpPage = next;
  reader::ReadingStats::pageTurn();
  if (_fbp) _lastPageCount = (uint16_t)_fbp->pageCount();
  reader::FbpBook::savePos(_bookPath.c_str(), _fbpPage, _lastPageCount);
  markDirty();
}

// --- Reader MENU v2 (full page + live strips) ---------------------------------

// Menu rows. Go-to is FBP-only (an epub's page numbers are per-chapter
// and unstable), so epub books show one row fewer and the rows after it
// shift down — menuRow() maps a cursor index to the row identity.
enum MenuRow : uint8_t {
  kMenuRowSize = 0,
  kMenuRowChapters,
  kMenuRowGoTo,
  kMenuRowBookmark,
  kMenuRowBookmarks,
  kMenuRowOrientation,
  kMenuRowStats,
  kMenuRowCount,
};

// The visible rows, in order, for the book that is open.
//
// Orientation used to appear only when the package carried landscape
// pages. From 2026-08-20 the apps stop building those by default, because
// a landscape edition is a second full set of pre-composed pages and
// nearly doubles a book. Hiding the row would then quietly delete the
// feature for almost every book, so the row stays and its value column
// says why it cannot be used. An offer beats an absence.
static int menuRows(bool isFbp, bool landscapeReady, MenuRow* out) {
  (void)landscapeReady;
  int n = 0;
  out[n++] = kMenuRowSize;
  out[n++] = kMenuRowChapters;
  if (isFbp) out[n++] = kMenuRowGoTo;
  out[n++] = kMenuRowBookmark;
  out[n++] = kMenuRowBookmarks;
  if (isFbp) out[n++] = kMenuRowOrientation;
  out[n++] = kMenuRowStats;
  return n;
}

uint8_t ReaderScene::longPressSlots() const {
  // The only long press in the reader a person could not guess: hold GO on a
  // bookmark to delete it. Everything else the labels already say.
  if (_state == State::Reading && _menu == MenuView::Bookmarks && _markCount > 0) return 0x02;
  return 0;
}

// Which physical soft key currently means "one step back" / "one step on".
//
// Portrait: slot 2 is the left key and slot 3 the right one, so back = Left.
// Landscape: the tab column runs upward from slot 0, so slot 3 is the TOP key
// — and the up arrow has to sit on it or the mark points away from the button
// under your thumb. Everything therefore trades places EXCEPT the text-size
// pair, whose portrait order (smaller, bigger) already puts "bigger" on slot
// 3. softKeys() reads the same predicate, so label and action cannot drift.
bool ReaderScene::dirSwap() const {
  return _landscape && !(_state == State::Reading && _menu == MenuView::SizeStrip);
}
bool ReaderScene::backKey(Input& in) const {
  return in.wasPressed(dirSwap() ? Btn::Right : Btn::Left);
}
bool ReaderScene::fwdKey(Input& in) const {
  return in.wasPressed(dirSwap() ? Btn::Left : Btn::Right);
}

void ReaderScene::handleMenuInput(Input& in) {
  MenuRow rowIds[kMenuRowCount];
  const int rows = menuRows(_fbp != nullptr, _landscapeReady, rowIds);
  switch (_menu) {
    case MenuView::Page:
      if (in.wasPressed(Btn::Back)) {
        // Labeled BACK: up one level from the menu is the book itself.
        _menu = MenuView::None;
        _orientNote = false;
        markDirty();
      } else if (in.wasPressed(Btn::Up) || backKey(in)) {
        _menuSel = (_menuSel + rows - 1) % rows;
        _orientNote = false;  // the note belongs to one row, one press
        markDirty();
      } else if (in.wasPressed(Btn::Down) || fwdKey(in)) {
        _menuSel = (_menuSel + 1) % rows;
        _orientNote = false;
        markDirty();
      } else if (in.wasPressed(Btn::Confirm)) {
        menuSelect();
      }
      return;

    case MenuView::SizeStrip:
      if (in.wasPressed(Btn::Back)) {          // labeled MENU: back up a level
        _menu = MenuView::Page;
        markDirty();
      } else if (in.wasPressed(Btn::Confirm)) {  // DONE: back to the menu
        _menu = MenuView::Page;
        markDirty();
      } else if (in.wasPressed(Btn::Up) || fwdKey(in)) {
        sizeStep(+1);
      } else if (in.wasPressed(Btn::Down) || backKey(in)) {
        sizeStep(-1);
      }
      return;

    case MenuView::Chapters: {
      int count = 0, cur = 0;
      chapterCountAndSel(&count, &cur);
      if (in.wasPressed(Btn::Back)) {
        _menu = MenuView::Page;
        markDirty();
      } else if (in.wasPressed(Btn::Confirm)) {
        chapterJump(_tocSel);
      } else if (count > 0 && (in.wasPressed(Btn::Up) || backKey(in))) {
        _tocSel = (_tocSel + count - 1) % count;
        markDirty();
      } else if (count > 0 && (in.wasPressed(Btn::Down) || fwdKey(in))) {
        _tocSel = (_tocSel + 1) % count;
        markDirty();
      }
      return;
    }

    case MenuView::GoTo: {
      if (!_fbp) { _menu = MenuView::Page; return; }
      const int last = _fbp->pageCount() ? (int)_fbp->pageCount() - 1 : 0;
      int delta = 0;
      if (fwdKey(in)) delta = +1;
      else if (backKey(in)) delta = -1;
      else if (in.wasPressed(Btn::Down)) delta = +10;   // top pair: coarse
      else if (in.wasPressed(Btn::Up)) delta = -10;
      if (delta != 0) {
        int p = (int)_fbpPage + delta;
        if (p < 0) p = 0;
        if (p > last) p = last;
        if (p != (int)_fbpPage) {
          _fbpPage = (uint16_t)p;  // live preview: the page IS the answer
          markDirty();
        }
        return;
      }
      if (in.wasPressed(Btn::Confirm)) {  // GO: stay here and read
        reader::FbpBook::savePos(_bookPath.c_str(), _fbpPage, _fbp ? _fbp->pageCount() : 0);
        _menu = MenuView::None;
        markDirty();
      } else if (in.wasPressed(Btn::Back)) {  // labeled MENU: cancel, restore
        _fbpPage = (uint16_t)_gotoPage;
        _menu = MenuView::Page;
        markDirty();
      }
      return;
    }

    case MenuView::Bookmarks:
      if (in.wasPressed(Btn::Back)) {
        _menu = MenuView::Page;
        markDirty();
      } else if (in.wasPressed(Btn::Confirm)) {
        if (_markCount > 0) jumpToBookmark(_markSel);
      } else if (in.wasLongPressed(Btn::Confirm)) {
        // Long-press GO deletes the mark under the cursor.
        if (_markCount > 0) {
          const int n = reader::Bookmarks::remove(_bookPath.c_str(), _markSel);
          if (n >= 0) {
            loadBookmarks();
            if (_markSel >= _markCount) _markSel = _markCount > 0 ? _markCount - 1 : 0;
            markDirty();
          }
        }
      } else if (_markCount > 0 && (in.wasPressed(Btn::Up) || backKey(in))) {
        _markSel = (_markSel + _markCount - 1) % _markCount;
        markDirty();
      } else if (_markCount > 0 && (in.wasPressed(Btn::Down) || fwdKey(in))) {
        _markSel = (_markSel + 1) % _markCount;
        markDirty();
      }
      return;

    case MenuView::StatsBook:
    case MenuView::StatsLife:
      if (in.wasPressed(Btn::Confirm)) {
        _menu = MenuView::None;  // READ
        markDirty();
      } else if (in.wasPressed(Btn::Back)) {
        _menu = MenuView::Page;
        markDirty();
      }
      return;

    case MenuView::None:
      return;
  }
}

void ReaderScene::menuSelect() {
  MenuRow rowIds[kMenuRowCount];
  const int rows = menuRows(_fbp != nullptr, _landscapeReady, rowIds);
  if (_menuSel < 0 || _menuSel >= rows) return;
  switch (rowIds[_menuSel]) {
    case kMenuRowSize:
      _menu = MenuView::SizeStrip;
      markDirty();
      return;
    case kMenuRowChapters: {
      int count = 0, cur = 0;
      chapterCountAndSel(&count, &cur);
      if (count <= 0) return;  // no TOC: the row is inert, not broken
      _tocSel = cur;
      _menu = MenuView::Chapters;
      markDirty();
      return;
    }
    case kMenuRowGoTo:
      if (!_fbp) return;
      _gotoPage = _fbpPage;  // cancel restores this
      _menu = MenuView::GoTo;
      markDirty();
      return;
    case kMenuRowBookmark:
      toggleBookmarkHere();
      markDirty();  // the row's own label reports the result
      return;
    case kMenuRowBookmarks:
      // Opens even with nothing in it. The empty state names the row that
      // makes one; a SELECT that visibly does nothing just looks broken.
      _markSel = 0;
      _menu = MenuView::Bookmarks;
      markDirty();
      return;
    case kMenuRowOrientation:
      if (!_landscapeReady) {
        // This book has no rotated pages. Say where they come from
        // instead of ignoring the press.
        _orientNote = true;
        markDirty();
        return;
      }
      // Applied in render(), which has the Gfx. Persisted: this is the
      // reader's global orientation from now on.
      _pendingOrient = _landscape ? 0 : 1;
      _pendingOrientPersist = true;
      markDirty();
      return;
    case kMenuRowStats:
      _menu = MenuView::StatsBook;
      markDirty();
      return;
    default:
      return;
  }
}

// --- Bookmarks ----------------------------------------------------------------

void ReaderScene::loadBookmarks() {
  _markCount = _bookPath.empty()
                   ? 0
                   : reader::Bookmarks::load(_bookPath.c_str(), _marks, reader::Bookmarks::kMax);
  if (_markSel >= _markCount) _markSel = _markCount > 0 ? _markCount - 1 : 0;
  _markScroll = 0;
}

// Index of a mark at the current place, or -1.
bool ReaderScene::bookmarkHereIndex(int* idxOut) {
  const uint16_t spine = _fbp ? 0 : (uint16_t)_spine;
  const uint16_t page = _fbp ? _fbpPage : (uint16_t)(_section ? _section->currentPage : 0);
  for (int i = 0; i < _markCount; i++) {
    if (_marks[i].spine == spine && _marks[i].page == page) {
      if (idxOut) *idxOut = i;
      return true;
    }
  }
  if (idxOut) *idxOut = -1;
  return false;
}

// One row, two meanings: mark this page, or remove the mark that's here.
void ReaderScene::toggleBookmarkHere() {
  if (_bookPath.empty()) return;
  int here = -1;
  if (bookmarkHereIndex(&here)) {
    if (reader::Bookmarks::remove(_bookPath.c_str(), here) >= 0) loadBookmarks();
    return;
  }
  reader::Bookmarks::Mark m;
  m.spine = _fbp ? 0 : (uint16_t)_spine;
  m.page = _fbp ? _fbpPage : (uint16_t)(_section ? _section->currentPage : 0);
  if (reader::Bookmarks::add(_bookPath.c_str(), m) >= 0) loadBookmarks();
}

void ReaderScene::jumpToBookmark(int idx) {
  if (idx < 0 || idx >= _markCount) return;
  const reader::Bookmarks::Mark m = _marks[idx];
  _menu = MenuView::None;
  if (_fbp) {
    const uint16_t last = _fbp->pageCount() ? (uint16_t)(_fbp->pageCount() - 1) : 0;
    _fbpPage = m.page > last ? last : m.page;
    reader::FbpBook::savePos(_bookPath.c_str(), _fbpPage, _fbp ? _fbp->pageCount() : 0);
    markDirty();
    return;
  }
  if (!_epub) return;
  if ((int)m.spine == _spine && _section) {
    _section->currentPage = m.page < _section->pageCount ? m.page : 0;
    markDirty();
    return;
  }
  _spine = m.spine;
  _nextPage = m.page;
  ensureSectionOrIndex();
}

// Chapter list size and the reader's current chapter index.
void ReaderScene::chapterCountAndSel(int* count, int* selOut) {
  int n = 0, sel = 0;
  if (_fbp) {
    n = (int)_fbp->tocCount();
    // Current chapter = last TOC entry whose opening page <= current.
    for (int i = 0; i < n; i++) {
      uint32_t cid = 0;
      char tmp[2];
      if (!_fbp->tocEntry((uint32_t)i, tmp, sizeof(tmp), &cid)) break;
      if (_fbp->pageForContentId(cid) <= _fbpPage) sel = i;
      else break;
    }
  } else if (_epub) {
    n = _epub->getSpineItemsCount();
    sel = _spine;
  }
  if (count) *count = n;
  if (selOut) *selOut = sel;
}

void ReaderScene::chapterJump(int idx) {
  if (_fbp) {
    uint32_t cid = 0;
    char tmp[2];
    if (!_fbp->tocEntry((uint32_t)idx, tmp, sizeof(tmp), &cid)) return;
    _fbpPage = _fbp->pageForContentId(cid);
    reader::FbpBook::savePos(_bookPath.c_str(), _fbpPage, _fbp ? _fbp->pageCount() : 0);
    _menu = MenuView::None;
    markDirty();
    return;
  }
  if (!_epub) return;
  _menu = MenuView::None;
  if (idx == _spine) {
    markDirty();
    return;
  }
  _spine = idx;
  _nextPage = 0;
  ensureSectionOrIndex();
}

// Whole-book progress fraction, both engines.
float ReaderScene::bookProgress() {
  if (_fbp && _fbp->pageCount() > 0)
    return (float)(_fbpPage + 1) / (float)_fbp->pageCount();
  if (_epub && _section && _section->pageCount > 0) {
    const float chapterProgress = (float)(_section->currentPage + 1) / (float)_section->pageCount;
    return _epub->calculateProgress(_spine, chapterProgress);
  }
  return 0.0f;
}

// Minutes left at the measured session pace; -1 = no pace data yet.
int ReaderScene::estimateMinutesLeft(int pagesLeft) const {
  if (_avgTurnMs == 0 || pagesLeft <= 0) return pagesLeft <= 0 ? 0 : -1;
  const uint32_t min = ((uint32_t)pagesLeft * _avgTurnMs + 59999) / 60000;
  return (int)(min < 1 ? 1 : min);
}

// EMA of the gap between forward turns; only plausible reading gaps count
// (3 s – 3 min), so a coffee break or a fast flip-through doesn't skew the
// overlay's time-left line.
void ReaderScene::noteTurnPace() {
  const uint32_t now = millis();
  if (_lastTurnMs != 0) {
    const uint32_t d = now - _lastTurnMs;
    if (d >= 3000 && d <= 180000) {
      _avgTurnMs = _avgTurnMs ? (3 * _avgTurnMs + d) / 4 : d;
    }
  }
  _lastTurnMs = now;
}

void ReaderScene::renderFbp(Gfx& gfx) {
  if (!_fbp) {
    renderMessage(gfx, "Read", "No book open");
    return;
  }
  if (!_fbp->profileSelected()) {
    uint16_t preferPx = 0;
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, /*readOnly=*/true)) {
      preferPx = prefs.getUShort("fbpPx", 0);
      prefs.end();
    }
    // contentRight, not width(): in landscape the soft-key column eats the
    // right edge and the compiled landscape profiles are that much narrower.
    if (!_fbp->selectProfile((uint16_t)contentRight(gfx, 0), (uint16_t)gfx.height(), preferPx)) {
      renderMessage(gfx, "Read", "Package profile mismatch");
      return;
    }
  }
  if (_fbpPosPending) {
    _fbpPage = reader::FbpBook::loadPos(_bookPath.c_str(), _fbp->pageCount());
    _lastPageCount = (uint16_t)_fbp->pageCount();  // known from the moment the book opens
    _fbpPosPending = false;
  }
  if (_fbpPage >= _fbp->pageCount()) _fbpPage = _fbp->pageCount() ? _fbp->pageCount() - 1 : 0;
  {
    CpuBoost boost;  // blits are pure CPU
    const uint32_t t0 = millis();
    _fbp->renderPage(gfx, _fbpPage);
    Serial.printf("[xphone-os] fbp: page %u/%u composed in %lu ms heap=%u peak=%lu uniq=%lu\n",
                  _fbpPage + 1, _fbp->pageCount(), static_cast<unsigned long>(millis() - t0),
                  ESP.getFreeHeap(), (unsigned long)_fbp->lastPeakBytes(),
                  (unsigned long)_fbp->lastUniqGlyphs());
  }
  // Status strip ABOVE the soft-key bar, same slot as renderStatusLine.
  // The menu strips replace it while open (their sheets own that band).
  if (_menu == MenuView::None) {
    char footer[32];
    snprintf(footer, sizeof(footer), "%u / %u", _fbpPage + 1, _fbp->pageCount());
    const int stripY = contentBottom(gfx, 0) - kStatusH + 4;
    gfx.drawTextCentered(kFontSmall, contentRight(gfx, 0) / 2, stripY, footer);
  }
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
  for (int i = 0; i < perPage && _scroll + i < _totalBooks; i++) {
    if (!inWindow(_scroll + i)) continue;  // window mid-move; render will rescan
    if (entryAt(_scroll + i).meta == TileMeta::Unknown) {
      idx = _scroll + i;
      break;
    }
  }
  if (idx < 0) return;  // scroll moved past the tiles that armed us
  BookEntry& b = entryAt(idx);

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
  // A book that failed to build before is REMEMBERED (meta.unreadable
  // sentinel): without it, every shelf rescan forgot the verdict and every
  // visible tick re-ran the whole failing build — the tile said
  // "unreadable", then "...", then "unreadable" forever (audit I3).
  if (epub && SdMan.exists((epub->getCachePath() + "/meta.unreadable").c_str())) {
    b.meta = TileMeta::NotOpened;
    markDirty(tileRect(idx - _scroll));
    return;
  }
  if (!epub || !epub->load(/*buildIfMissing=*/true)) {
    b.meta = TileMeta::NotOpened;
    if (epub) {
      FsFile f = SdMan.open((epub->getCachePath() + "/meta.unreadable").c_str(),
                            O_WRONLY | O_CREAT | O_TRUNC);
      if (f) f.close();
    }
  } else {
    const std::string& title = epub->getTitle();
    if (title.empty()) prettyFileTitle(b.path, b.title, sizeof(b.title));
    else snprintf(b.title, sizeof(b.title), "%s", title.c_str());
    snprintf(b.author, sizeof(b.author), "%s", epub->getAuthor().c_str());
    b.meta = TileMeta::NoCover;  // coverless AND transient failures render text-only
    std::string thumbPath;
    if (reader::CoverThumb::ensure(*epub, kThumbW, kThumbH, &thumbPath) &&
        thumbPath.size() < sizeof(b.thumbPath) && readThumbDims(thumbPath.c_str(), &b.thumbW, &b.thumbH)) {
      memcpy(b.thumbPath, thumbPath.c_str(), thumbPath.size() + 1);
      b.meta = TileMeta::Cover;
      publishCoverSidecar(b.path, thumbPath.c_str());
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
      if (_menu != MenuView::None) {
        handleMenuInput(in);
        return;
      }
      // Long-press CONFIRM: size-cycle shortcut (the old SIZE behavior),
      // no menu trip. Long-press = shortcut only, per the 0.7 direction.
      if (in.wasLongPressed(Btn::Confirm)) {
        sizeStep(+1);
        return;
      }
      if (in.wasPressed(Btn::Confirm)) {
        _menu = MenuView::Page;  // "MENU" soft key: the book's home page
        markDirty();
        return;
      }
      if (in.wasPressed(Btn::Back)) {
        if (_fbp) _fbp.reset();  // frees the glyph cache before the grid repaints
        enterBookList();
        return;
      }
      if (fwdKey(in) || in.wasPressed(Btn::Down)) {
        if (_fbp) fbpTurn(true); else pageTurn(/*forward=*/true);
      } else if (backKey(in) || in.wasPressed(Btn::Up)) {
        if (_fbp) fbpTurn(false); else pageTurn(/*forward=*/false);
      }
      return;

    case State::BookList:
      if (_lifeOpen) {
        if (in.wasPressed(Btn::Back) || in.wasPressed(Btn::Confirm)) {
          _lifeOpen = false;
          markDirty();
        }
        return;
      }
      if (in.wasPressed(Btn::Back)) {
        if (_work == Work::GridMeta) _work = Work::None;  // don't scan behind another state
        // Always home, even with a book open — bouncing back into the book
        // trapped the user in a book <-> list loop. Progress is saved, so
        // reopening from the grid (or the resume path in onEnter) is cheap.
        showLauncher();
        return;
      }
      if (in.wasPressed(Btn::Confirm)) {
        if (_sel < 0) {  // the stats band: your reading, from the shelf
          if (_work == Work::GridMeta) _work = Work::None;
          _lifeOpen = true;
          markDirty();
          return;
        }
        if (_totalBooks > 0) {
          openSelectedBook();  // overwrites pending GridMeta — opens never wait on thumbs
          return;
        }
      }
      // Grid nav: front Left/Right step one book, top Up/Down one ROW.
      if (in.wasPressed(Btn::Up)) moveSelection(-kGridCols);
      if (in.wasPressed(Btn::Down)) moveSelection(+kGridCols);
      if (backKey(in)) moveSelection(-1);
      if (fwdKey(in)) moveSelection(+1);
      return;
  }
}

void ReaderScene::pageTurn(const bool forward) {
  if (!_epub || !_section) return;
  reader::ReadingStats::pageTurn();
  if (forward) {
    noteTurnPace();
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
  setFontSize((_settings.fontId + 1) % reader::kReaderFontCount);
}

// One live size step from the MENU overlay (or the long-CONFIRM shortcut).
// Wraps at the ends, same as the old SIZE cycle — with only three sizes a
// wrap is one step from anywhere, and the overlay shows where you landed.
void ReaderScene::sizeStep(const int dir) {
  if (_fbp) {
    // FBP profiles only cycle forward; with three sizes, one step down is
    // two steps forward. The content-ID search keeps the position.
    const int steps = dir > 0 ? 1 : 2;
    for (int i = 0; i < steps; i++) {
      uint16_t np = _fbpPage;
      if (!_fbp->profileSelected() || !_fbp->cycleSize(_fbpPage, &np)) return;
      _fbpPage = np;
    }
    reader::FbpBook::savePos(_bookPath.c_str(), _fbpPage, _fbp ? _fbp->pageCount() : 0);
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, /*readOnly=*/false)) {
      prefs.putUShort("fbpPx", _fbp->pxSize());
      prefs.end();
    }
    markDirty();
    return;
  }
  if (!_epub) return;
  const int count = reader::kReaderFontCount;
  setFontSize(((_settings.fontId + dir) % count + count) % count);
}

void ReaderScene::setFontSize(const int nextId) {
  if (!_epub || !_section || !_measure || nextId == _settings.fontId) return;
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
  // The shelf is the one screen that still trades the radio for heap: the
  // cover decoder's ~58 KB scratch does not fit beside BLE+ANCS.
  suspendRadioForBookWork();
  reader::ReadingStats::sessionEnd();  // flush before the grid repaints (band shows fresh numbers)

  // Hand back everything the closed book held BEFORE the shelf claims its
  // cover scratch. Leaving these mapped shredded the heap: the largest free
  // block fell 76 bytes short of the 59 KB scratch (audit I8), so covers
  // and even no-cover verdicts went unresolvable until a restart. Reopening
  // from book.bin/progress.bin is a proven ~43 ms, so holding nothing costs
  // almost nothing.
  _section.reset();
  _fbp.reset();
  _measure.reset();
  _epub.reset();
  _renderer.releaseCaches();
  InflateReader::releaseSharedDict();
  reader::CoverThumb::preacquireScratch();

  _menu = MenuView::None;  // the menu never survives leaving the book
  _menuSel = 0;
  _orientNote = false;
  _lifeOpen = false;
  _pendingRestorePortrait = _landscape;  // shelf + every other scene is portrait
  _landscape = false;
  _pendingOrient = -1;  // a pending flip must not follow us out of the book
  if (_sel < 0) _sel = 0;
  _lastTurnMs = 0;     // pace gaps don't span books (the EMA itself persists)
  scanBooks();
  _state = State::BookList;
  markDirty();
}

void ReaderScene::scanBooks(const int windowOffset) {
  _bookCount = 0;
  _totalBooks = 0;
  _skippedNames = 0;
  _windowOffset = windowOffset;
  if (windowOffset == 0) {
    _sel = 0;
    _scroll = 0;
  }
  _sdOk = SdMan.ready() || SdMan.begin();
  if (_sdOk) {
    scanDir("/books", 0);
    scanDir("/", 1);  // root: books allowed, no recursion into root dirs
  }
  if (_skippedNames)
    Serial.printf("[xphone-os] reader: scan %d books, %u skipped (see lines above)\n", _totalBooks,
                  static_cast<unsigned>(_skippedNames));

  // Reading state first: it drives the sort and the tiles' progress line.
  // The .pos trailer carries the pagination its page lives in, so the
  // percent needs no header peek and survives cross-build pushes.
  for (int i = 0; i < _bookCount; i++) {
    BookEntry& b = _books[i];
    b.lastReadDay = 0;
    b.readMinutes = 0;
    b.progressPct = 0;
    uint32_t pages = 0, minutes = 0, lastDay = 0;
    if (reader::ReadingStats::bookStats(b.path, &pages, &minutes, &lastDay)) {
      b.lastReadDay = lastDay;
      b.readMinutes = (uint16_t)(minutes > 0xFFFF ? 0xFFFF : minutes);
    }
    char side[192];
    snprintf(side, sizeof(side), "%s.pos", b.path);
    FsFile pf = SdMan.open(side, O_RDONLY);
    if (pf) {
      uint32_t page = 0, count = 0;
      pf.read(&page, 4);
      const bool hasCount = pf.read(&count, 4) == 4;
      pf.close();
      if (hasCount && count > 0 && page < count) {
        uint32_t pct = (uint32_t)(((uint64_t)(page + 1) * 100) / count);
        if (pct > 100) pct = 100;
        if (pct == 0) pct = 1;
        b.progressPct = (uint8_t)pct;
      }
    }
  }

  // Last-read first, then alphabetical (insertion sort) — only while the
  // window IS the whole library; a paged window can't know global order, so
  // big libraries read in card order (the app is the browsing surface
  // there). Never-read books keep the alphabetical shelf below the ones in
  // flight — the reading chair puts your open book on top.
  if (_windowOffset == 0 && _totalBooks <= kMaxBooks) {
    auto ordersBefore = [](const BookEntry& a, const BookEntry& b) -> bool {
      if (a.lastReadDay != b.lastReadDay) return a.lastReadDay > b.lastReadDay;
      if (a.readMinutes != b.readMinutes) return a.readMinutes > b.readMinutes;
      return strcasecmp(baseName(a.path), baseName(b.path)) < 0;
    };
    for (int i = 1; i < _bookCount; i++) {
      BookEntry key = _books[i];
      int j = i - 1;
      while (j >= 0 && ordersBefore(key, _books[j])) {
        _books[j + 1] = _books[j];
        j--;
      }
      _books[j + 1] = key;
    }
  }

  // Fast-pass: tiles whose caches already exist paint complete on the FIRST
  // render. Reopening the Reader used to visibly re-upgrade every tile
  // one-per-quiet-tick even though each was a ~30 ms cache hit; that belongs
  // before first paint. Anything needing real work (book.bin build, cover
  // decode) stays Unknown for the GridMeta tick.
  for (int i = 0; i < _bookCount; i++) {
    BookEntry& b = _books[i];
    if (endsWithFbpCI(b.path)) {
      // Compiled package: meta comes from the FBPK header, and the shelf
      // assets (cover thumb + shaped title strip) were pre-rendered by the
      // phone — extract them once into CoverThumb-format sidecars.
      bool focusEd = false;
      if (reader::FbpBook::readMeta(b.path, b.title, sizeof(b.title), b.author, sizeof(b.author),
                                    &focusEd)) {
        b.focusEdition = focusEd;
        b.meta = TileMeta::NoCover;
        // A package with an empty title in its header would otherwise wipe
        // the seeded name and leave the tile blank.
        if (b.title[0] == '\0') prettyFileTitle(b.path, b.title, sizeof(b.title));
        bool hasCover = false, hasStrip = false;
        reader::FbpBook::ensureShelfSidecars(b.path, &hasCover, &hasStrip);
        if (hasCover) {
          char cov[112];
          snprintf(cov, sizeof(cov), "%s.cov", b.path);
          if (strlen(cov) < sizeof(b.thumbPath) && readThumbDims(cov, &b.thumbW, &b.thumbH)) {
            memcpy(b.thumbPath, cov, strlen(cov) + 1);
            b.meta = TileMeta::Cover;
          }
        }
      }
      continue;
    }
    const std::unique_ptr<reader::Epub> epub(new (std::nothrow)
                                                 reader::Epub(b.path, reader::kReaderCacheRoot));
    if (!epub) continue;
    // A remembered failure paints "unreadable" on the FIRST render, exactly
    // like a remembered success paints its cover (audit I3).
    if (SdMan.exists((epub->getCachePath() + "/meta.unreadable").c_str())) {
      b.meta = TileMeta::NotOpened;
      continue;
    }
    if (!epub->load(/*buildIfMissing=*/false)) continue;
    const std::string& title = epub->getTitle();
    if (title.empty()) prettyFileTitle(b.path, b.title, sizeof(b.title));
    else snprintf(b.title, sizeof(b.title), "%s", title.c_str());
    snprintf(b.author, sizeof(b.author), "%s", epub->getAuthor().c_str());
    std::string thumbPath;
    const int hit = reader::CoverThumb::probe(*epub, kThumbW, kThumbH, &thumbPath);
    if (hit == 1 && thumbPath.size() < sizeof(b.thumbPath) &&
        readThumbDims(thumbPath.c_str(), &b.thumbW, &b.thumbH)) {
      memcpy(b.thumbPath, thumbPath.c_str(), thumbPath.size() + 1);
      b.meta = TileMeta::Cover;
      // F6: a thumb that was already in the cache still has to be published
      // beside the book, or a book the device has known for weeks stays a
      // blank tile on the phone. publishCoverSidecar returns at once when
      // the sidecar is already there.
      publishCoverSidecar(b.path, thumbPath.c_str());
    } else if (hit == 0) {
      b.meta = TileMeta::NoCover;
    }
  }
}

void ReaderScene::scanDir(const char* dir, const int depth) {
  // Directory iteration needs SdFat's FsFile::openNext — below the ReaderFs
  // shim's surface, so use the SDK manager directly (same pattern as
  // SdUpdate.cpp; all SD access stays on the main loop task).
  if (!SdMan.ready() && !SdMan.begin()) return;
  FsFile d = SdMan.open(dir, O_RDONLY);
  if (!d || !d.isDir()) return;

  const bool isRoot = (dir[0] == '/' && dir[1] == '\0');
  FsFile f;
  while (f.openNext(&d, O_RDONLY)) {
    char name[128];
    const int len = f.getName(name, sizeof(name));
    if (len <= 0) {
      // SdFat returns 0 when the LFN doesn't fit the buffer AT ALL — this is
      // the silent path that ate the 135-char bench file. Count it.
      _skippedNames++;
      Serial.printf("[xphone-os] reader: SKIP (name unreadable or > %u chars) in %s\n",
                    static_cast<unsigned>(sizeof(name) - 1), dir);
      f.close();
      continue;
    }
    if (name[0] == '.') {
      f.close();
      continue;
    }
    if (len >= static_cast<int>(sizeof(name)) - 1) {
      // Truncated: the real name overflows even the raised buffer. Count it
      // so the empty state / serial can say WHY a book is missing instead of
      // silently losing it (the PocketFullOfFun failure mode).
      _skippedNames++;
      Serial.printf("[xphone-os] reader: SKIP (name > %u chars) %.60s...\n",
                    static_cast<unsigned>(sizeof(name) - 1), name);
      f.close();
      continue;
    }
    if (f.isDir()) {
      // One level of nesting, /books only — Calibre-style "Author/book.epub"
      // works; deeper trees stay the app's job to flatten on sync.
      if (depth == 0) {
        char sub[160];
        const int n = snprintf(sub, sizeof(sub), "%s/%s", dir, name);
        f.close();  // close before recursing: SdFat file handles are scarce
        if (n > 0 && n < static_cast<int>(sizeof(sub))) scanDir(sub, depth + 1);
      } else {
        f.close();
      }
      continue;
    }
    f.close();
    if (!endsWithEpubCI(name) && !endsWithFbpCI(name)) continue;

    char full[160];
    const int n = snprintf(full, sizeof(full), "%s/%s", isRoot ? "" : dir, name);
    if (n <= 0 || n >= static_cast<int>(sizeof(full))) {
      _skippedNames++;
      Serial.printf("[xphone-os] reader: SKIP (path > %u chars) %s/%.40s...\n",
                    static_cast<unsigned>(sizeof(full) - 1), dir, name);
      continue;
    }

    // A source and its package are ONE book. The card keeps both from
    // 2026-08-20 (FileTransferServer no longer deletes the epub), so the
    // shelf hides the epub whenever its .fbp sits beside it. It has to be
    // this way round: the package is what the device can read quickly.
    if (endsWithEpubCI(name)) {
      char pkg[160];
      snprintf(pkg, sizeof(pkg), "%s", full);
      char* dot = strrchr(pkg, '.');
      if (dot) {
        snprintf(dot, sizeof(pkg) - static_cast<size_t>(dot - pkg), ".fbp");
        if (SdMan.exists(pkg)) continue;
      }
    }

    const int gidx = _totalBooks;  // absolute shelf index of this match
    _totalBooks++;
    if (gidx >= _windowOffset && _bookCount < kMaxBooks) {
      BookEntry& b = _books[_bookCount];
      // Fresh entry, meta reset to Unknown (rescan: paths may have changed;
      // grid work reloads titles/thumbs lazily).
      memset(&b, 0, sizeof(b));
      memcpy(b.path, full, static_cast<size_t>(n) + 1);
      // A placeholder tile is a tile a reader LOOKS AT. The shelf probes one
      // book per quiet tick, and an epub with no built cache stays Unknown
      // until it is built. Those tiles used to draw the bare filename —
      // "zz-plain-name.epub" — which was the one place the device still said
      // ".epub" out loud. Seed the pretty title here; metadata overwrites it.
      prettyFileTitle(b.path, b.title, sizeof(b.title));
      _bookCount++;
    }
  }
  d.close();
}

void ReaderScene::openSelectedBook() {
  if (_sel < 0 || _sel >= _totalBooks || !inWindow(_sel)) return;

  // Full reopen (even for the currently open book — progress.bin preserves
  // the position, and the path is one code path).
  _section.reset();
  _epub.reset();
  _measure.reset();
  _bookPath = entryAt(_sel).path;
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
  if (sel > _totalBooks - 1) sel = _totalBooks - 1;
  // -1 is the stats band above the grid: UP from the first row lands on it.
  if (sel < -1) sel = -1;
  if (sel == _sel) return;
  const int prev = _sel;
  _sel = sel;

  // Scroll by whole ROWS so _scroll stays a multiple of kGridCols.
  const int perPage = kGridCols * kGridRows;
  const int oldScroll = _scroll;
  while (_sel < _scroll) _scroll -= kGridCols;
  while (_sel >= _scroll + perPage) _scroll += kGridCols;
  if (_scroll < 0) _scroll = 0;

  // Window chase: if the visible page fell outside the stored window,
  // rescan around it (keeps RAM fixed while the library is unbounded; the
  // few-ms card walk replaces the old hard 32-book ceiling).
  if (_scroll < _windowOffset || _scroll + perPage > _windowOffset + _bookCount) {
    const int sav_sel = _sel, sav_scroll = _scroll;
    int base = _scroll - kGridCols * kGridRows;  // one page of back-margin
    if (base < 0) base = 0;
    scanBooks(base);
    _sel = sav_sel;
    _scroll = sav_scroll;
    markDirty(listRect());
    return;
  }
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
  if (_pendingRestorePortrait) {
    _pendingRestorePortrait = false;
    gfx.setOrientation(Gfx::Orient::Portrait);
  }
  if (_pendingOrient >= 0) applyOrientation(gfx, _pendingOrient == 1, _pendingOrientPersist);
  // Landscape is offered only when the package actually carries profiles for
  // the rotated viewport (bookc --landscape). Computed HERE, not inside
  // renderMenuPage: menuRows() decides how far the menu cursor wraps, so a
  // flag that is only fresh after the menu has been drawn once let the cursor
  // wrap at six rows while the page showed seven.
  //
  // Always ask about the LANDSCAPE profile whichever way the panel is turned.
  // Deriving it from the live width/height meant that once you were IN
  // landscape the question became "is there a portrait profile 44 px narrow",
  // the answer was no, the Orientation row disappeared — and there was no way
  // back to portrait short of leaving the book. Landscape content stops before
  // the soft-key column, so the profile is that bit narrower than the panel.
  {
    const uint16_t pw = static_cast<uint16_t>(_landscape ? gfx.height() : gfx.width());
    const uint16_t ph = static_cast<uint16_t>(_landscape ? gfx.width() : gfx.height());
    _landscapeReady =
        _fbp && _fbp->hasGeometry(static_cast<uint16_t>(ph - Scene::SOFTKEY_BAR_H), pw);
  }
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
      snprintf(info, sizeof(info), "ch %d of %d", _spine + 1, _epub ? _epub->getSpineItemsCount() : 0);
      renderMessage(gfx, "Indexing chapter...", info);
      return;
    }
    case State::Error:
      renderMessage(gfx, "Read", _errorMsg);
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
  const int cx = contentRight(gfx, 0) / 2;
  const int cy = gfx.height() / 2;
  if (line1 && line1[0]) gfx.drawTextCentered(kFontBold, cx, cy - gfx.lineHeight(kFontBold), line1);
  if (line2 && line2[0]) {
    char clipped[96];
    truncateToWidth(gfx, kFontRegular, line2, contentRight(gfx, 0) - 2 * kListMarginX, clipped, sizeof(clipped));
    gfx.drawTextCentered(kFontRegular, cx, cy + 6, clipped);
  }
}

// Turn the panel and re-select the package's profile for the new viewport,
// carrying the reading position across the re-pagination by content ID (the
// same anchor SIZE changes use). FBP only: an epub would need its section
// caches rebuilt at the new geometry.
//
// Two callers with different needs, one function:
//   - the menu row flips the current state and PERSISTS the result;
//   - opening a book restores the persisted state and must not re-write it.
// Returns false when the package cannot do the requested orientation, having
// left the panel exactly as it found it.
bool ReaderScene::applyOrientation(Gfx& gfx, const bool toLandscape, const bool persist) {
  _pendingOrient = -1;
  if (!_fbp) return false;

  const Gfx::Orient before = gfx.orientation();
  // Anchor only exists once a profile is live. At book-open time there is
  // none yet — and there is nothing to carry across either, because the
  // ".pos" page index was saved in this same orientation.
  const bool hadProfile = _fbp->profileSelected();
  uint32_t cid = 0;
  const bool haveAnchor = hadProfile && _fbp->pageFirstCidPublic(_fbpPage, &cid);

  gfx.setOrientation(toLandscape ? Gfx::Orient::Landscape : Gfx::Orient::Portrait);
  const uint16_t wantW = static_cast<uint16_t>(contentRight(gfx, 0));
  const uint16_t wantH = static_cast<uint16_t>(gfx.height());

  if (!hadProfile) {
    // Check capability only. renderFbp does the real selection, because it
    // is the one that knows the remembered text size — selecting here would
    // silently drop the reader back to the middle size stop.
    if (!_fbp->hasGeometry(wantW, wantH)) {
      gfx.setOrientation(before);
      return false;
    }
  } else if (!_fbp->selectProfileFresh(wantW, wantH)) {
    gfx.setOrientation(before);
    return false;  // package has no profile there — stay put
  }

  _landscape = toLandscape;
  if (haveAnchor) _fbpPage = _fbp->pageForContentId(cid);
  const uint16_t last = _fbp->pageCount() ? (uint16_t)(_fbp->pageCount() - 1) : 0;
  if (hadProfile) {
    if (_fbpPage > last) _fbpPage = last;
    reader::FbpBook::savePos(_bookPath.c_str(), _fbpPage, _fbp ? _fbp->pageCount() : 0);
  }
  if (persist) {
    _wantLandscape = toLandscape;
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, /*readOnly=*/false)) {
      prefs.putUChar(kPrefsLandKey, toLandscape ? 1 : 0);
      prefs.end();
    }
  }
  // pageCount() is 0 until renderFbp selects the profile, which is the case on
  // the restore path — print the page alone there rather than "27/0".
  if (hadProfile)
    Serial.printf("[xphone-os] reader: orientation -> %s (%dx%d) page %u/%u [saved]\n",
                  _landscape ? "landscape" : "portrait", gfx.width(), gfx.height(), _fbpPage + 1,
                  _fbp->pageCount());
  else
    Serial.printf("[xphone-os] reader: orientation -> %s (%dx%d) page %u [restored]\n",
                  _landscape ? "landscape" : "portrait", gfx.width(), gfx.height(), _fbpPage + 1);
  return true;
}

void ReaderScene::renderReading(Gfx& gfx) {
  // Full-page menu views replace the page; strip views draw over it.
  if (_menu == MenuView::Page) {
    renderMenuPage(gfx);
    return;
  }
  if (_menu == MenuView::Chapters) {
    renderChapters(gfx);
    return;
  }
  if (_menu == MenuView::Bookmarks) {
    renderBookmarks(gfx);
    return;
  }
  if (_menu == MenuView::StatsBook) {
    renderStatsBook(gfx);
    return;
  }
  if (_menu == MenuView::StatsLife) {
    renderStatsLife(gfx);
    return;
  }
  if (_fbp) {
    renderFbp(gfx);
    if (_menu == MenuView::SizeStrip) renderSizeStrip(gfx);
    else if (_menu == MenuView::GoTo) renderGoToStrip(gfx);
    return;
  }
  if (!_epub || !_section || !_measure) {  // defensive: should be unreachable
    renderMessage(gfx, "Read", "No book open");
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
        snprintf(info, sizeof(info), "ch %d of %d", _spine + 1, _epub->getSpineItemsCount());
        renderMessage(gfx, "Indexing chapter...", info);
        return;
      }
      failWith("Couldn't read this chapter");
      renderMessage(gfx, "Read", _errorMsg);
      return;
    }
    _pageLoadRetries = 0;

    const uint32_t t0 = millis();
    _renderer.renderPage(gfx, *page, *_measure, kMarginX, kMarginTop);
    Serial.printf("[xphone-os] reader: page %d/%u composed in %lu ms\n", _section->currentPage + 1,
                  _section->pageCount, static_cast<unsigned long>(millis() - t0));
  }

  if (_menu == MenuView::None) renderStatusLine(gfx);  // strips own that band
  saveProgress();     // crash-safe resume: 6 bytes, atomic, every page
  maybeArmPrefetch();  // silent next-chapter indexing near the chapter end
  if (_menu == MenuView::SizeStrip) renderSizeStrip(gfx);
  else if (_menu == MenuView::GoTo) renderGoToStrip(gfx);
}

// Shared: the current size stop (Small/Medium/Large). FBP packages
// report a pixel size (22/26/30 today) — snap to the nearest stop;
// epubs report their font id directly.
static int currentSizeStop(const reader::FbpBook* fbp, int fontId) {
  if (fbp) {
    const uint16_t px = fbp->pxSize();
    return px <= 22 ? 0 : (px >= 30 ? 2 : 1);
  }
  return fontId < 0 ? 0 : (fontId > 2 ? 2 : fontId);
}

// --- MENU v2: the book's home page ---------------------------------------------
// Full page (design B, 2026-08-16): title + author, progress bar, the
// action list with values on the right, time-left footer.
void ReaderScene::renderMenuPage(Gfx& gfx) {
  const int w = contentRight(gfx, 0);
  gfx.fillRect(0, 0, w, gfx.height(), false);

  // Header: what book, where in it.
  //
  // The UI fonts are ASCII+Latin subsets, so an Arabic or Chinese title
  // drawn as text is a row of "?" (seen on glass with kalila-wa-dimna).
  // FBP packages ship a phone-SHAPED title strip for exactly this — the
  // same bitmap the shelf tiles use. Prefer it; fall back to text.
  // Title/author, from the shelf window's parsed metadata (epub) or the
  // package header (FBP); filename only as a last resort.
  const char* title = nullptr;
  const char* author = "";
  if (_fbp) {
    title = _fbp->title();
    author = _fbp->author();
  } else {
    for (int i = 0; i < _bookCount; i++) {
      if (_bookPath == _books[i].path && _books[i].title[0]) {
        title = _books[i].title;
        author = _books[i].author;
        break;
      }
    }
  }
  static char prettyTitle[96];
  if (!title || !title[0]) {
    prettyFileTitle(_bookPath.c_str(), prettyTitle, sizeof(prettyTitle));
    title = prettyTitle;
  }

  char line[96];
  int y = 24;
  // Text is preferred — it uses the full page width and the real title.
  // But the UI fonts are ASCII+Latin subsets, so an Arabic or Chinese
  // title would draw as "????" (seen on glass with kalila-wa-dimna);
  // there, fall back to the FBP package's phone-SHAPED title strip,
  // which carries any script as a bitmap.
  if (!gfx.canRender(kFontBold, title) && _fbp) {
    char strip[112];
    snprintf(strip, sizeof(strip), "%s.str", _bookPath.c_str());
    uint16_t sw = 0, sh = 0;
    if (readThumbDims(strip, &sw, &sh) && sw > 0 && sh > 0 && sw <= w &&
        reader::CoverThumb::draw(gfx, strip, (w - sw) / 2, y)) {
      y += sh + 4 + 14;
      return renderMenuBody(gfx, y);  // strip carries title AND author
    }
  }
  truncateToWidth(gfx, kFontBold, title, w - 48, line, sizeof(line));
  gfx.drawTextCentered(kFontBold, w / 2, y, line);
  y += gfx.lineHeight(kFontBold) + 2;
  if (author && author[0] && gfx.canRender(kFontSmall, author)) {
    truncateToWidth(gfx, kFontSmall, author, w - 48, line, sizeof(line));
    gfx.drawTextCentered(kFontSmall, w / 2, y, line);
    y += gfx.lineHeight(kFontSmall);
  }
  y = renderFocusChip(gfx, y);
  y += 14;
  renderMenuBody(gfx, y);
}

// The FOCUS chip, in the same shape the shelf badges the artwork with. Two
// editions of one book are identical once open otherwise — you cannot tell
// which one you are reading, which is exactly when you want to know.
int ReaderScene::renderFocusChip(Gfx& gfx, int y) {
  if (!_fbp || !_fbp->isFocusEdition()) return y;
  const char* tag = "FOCUS";
  const int cw = gfx.textWidth(kFontSmall, tag) + 16;
  const int ch = gfx.lineHeight(kFontSmall) + 4;
  const int cx = contentRight(gfx, 0) / 2;
  gfx.drawRoundedRect(cx - cw / 2, y + 4, cw, ch, ch / 2, 2, true);
  gfx.drawTextCentered(kFontSmall, cx, y + 6, tag);
  return y + 4 + ch;
}

// Everything below the header: progress band, rows, footer. Split out so
// the header can choose text or a shaped strip without duplicating it.
void ReaderScene::renderMenuBody(Gfx& gfx, int y) {
  const int w = contentRight(gfx, 0);
  char line[96];

  // Progress band: bar + one honest line.
  const float frac = bookProgress();
  const int barX = 60, barW = w - 120, barH = 6;
  gfx.fillRect(barX, y, barW, barH, false);
  gfx.drawRect(barX, y, barW, barH, 1, true);
  gfx.fillRect(barX, y, (int)(barW * frac + 0.5f), barH, true);
  y += barH + 10;
  int page = 0, total = 0;
  if (_fbp) {
    page = _fbpPage + 1;
    total = _fbp->pageCount();
    snprintf(line, sizeof(line), "%d%%  -  page %d of %d", (int)(frac * 100 + 0.5f), page, total);
  } else if (_section) {
    page = _section->currentPage + 1;
    total = (int)_section->pageCount;
    snprintf(line, sizeof(line), "%d%%  -  ch %d of %d", (int)(frac * 100 + 0.5f),
             _spine + 1, _epub ? _epub->getSpineItemsCount() : 0);
  } else {
    line[0] = 0;
  }
  gfx.drawTextCentered(kFontSmall, w / 2, y, line);
  y += gfx.lineHeight(kFontSmall) + 16;
  gfx.drawLine(40, y, w - 40, y, 1, true);
  y += 14;

  // The action rows.
  int chapCount = 0, chapCur = 0;
  chapterCountAndSel(&chapCount, &chapCur);
  MenuRow rowIds[kMenuRowCount];
  const int rows = menuRows(_fbp != nullptr, _landscapeReady, rowIds);
  static constexpr const char* kStops[3] = {"Small", "Medium", "Large"};
  const bool markedHere = bookmarkHereIndex(nullptr);

  // The rows OWN the space between the header rule and the footer rule.
  // At a fixed 44 px they left a third of the page blank underneath, which
  // read as a screen that had failed to finish drawing. Capped at 64 so a
  // short list (an epub has no "Go to page") spreads without looking sparse,
  // and the block is centred in whatever is left over.
  const int footerRuleY = contentBottom(gfx, 0) - gfx.lineHeight(kFontSmall) - 28;
  const int avail = footerRuleY - 12 - y;
  int rowH = rows > 0 ? avail / rows : 44;
  if (rowH > 64) rowH = 64;
  // Floor is the text line plus a little, NOT a comfortable 44: the X4 turned
  // landscape has 480 px, and seven rows at 44 came to 308 in 263 px of space.
  // The centring below then went negative and pushed the first row up through
  // the header rule while the footer fell off the bottom edge.
  const int minRowH = gfx.lineHeight(kFontRegular) + 5;
  if (rowH < minRowH) rowH = minRowH;
  const int slack = avail - rows * rowH;
  if (slack > 0) y += slack / 2;  // only ever centre INTO spare room
  int hiliteH = 40;               // the selection bar stays a bar, not a slab
  if (hiliteH > rowH - 4) hiliteH = rowH - 4;
  for (int i = 0; i < rows; i++) {
    const char* name = "";
    char value[32];
    value[0] = 0;
    switch (rowIds[i]) {
      case kMenuRowSize:
        name = "Text size";
        snprintf(value, sizeof(value), "%s", kStops[currentSizeStop(_fbp.get(), _settings.fontId)]);
        break;
      case kMenuRowChapters:
        name = "Chapters";
        if (chapCount > 0) snprintf(value, sizeof(value), "ch %d of %d", chapCur + 1, chapCount);
        else snprintf(value, sizeof(value), "none");
        break;
      case kMenuRowGoTo: name = "Go to page"; break;
      case kMenuRowBookmark:
        name = markedHere ? "Remove bookmark" : "Bookmark this page";
        break;
      case kMenuRowBookmarks:
        name = "Bookmarks";
        if (_markCount > 0) snprintf(value, sizeof(value), "%d", _markCount);
        else snprintf(value, sizeof(value), "none");
        break;
      case kMenuRowOrientation:
        name = "Orientation";
        if (!_landscapeReady) {
          // SELECT on an unusable row that says nothing "just looks
          // broken" (the same reasoning as the Bookmarks row above), so
          // pressing it swaps the value for the thing to actually do.
          snprintf(value, sizeof(value), "%s",
                   _orientNote ? "Turn on in the app" : "Portrait only");
        } else {
          snprintf(value, sizeof(value), "%s", _landscape ? "Landscape" : "Portrait");
        }
        break;
      case kMenuRowStats: name = "Book stats"; break;
      default: break;
    }
    const bool sel = i == _menuSel;
    if (sel) gfx.fillRect(32, y + (rowH - hiliteH) / 2, w - 64, hiliteH, true);
    const int textY = y + (rowH - gfx.lineHeight(kFontRegular)) / 2;
    gfx.drawText(sel ? kFontBold : kFontRegular, 48, textY, name, !sel);
    if (value[0]) {
      const int vw = gfx.textWidth(kFontRegular, value);
      gfx.drawText(kFontRegular, w - 48 - vw, textY, value, !sel);
    }
    y += rowH;
  }

  // Footer: the one most-loved stat. PINNED just above the soft-key bar
  // rather than trailing the rows — following the list left a third of the
  // page blank underneath, which read as an unfinished screen.
  int footerY = footerRuleY + 14;
  if (footerY < y + 22) footerY = y + 22;  // a very long row list wins
  gfx.drawLine(40, footerY - 14, w - 40, footerY - 14, 1, true);
  y = footerY;
  int pagesLeft = total - page;
  if (pagesLeft < 0) pagesLeft = 0;
  const char* scope = _fbp ? "book" : "chapter";
  const int mins = estimateMinutesLeft(pagesLeft);
  if (mins > 0)
    // Long books read as "About 2072 min left" without this — nobody
    // thinks in thousands of minutes.
    if (mins >= 600) snprintf(line, sizeof(line), "About %d hours left in this %s", (mins + 30) / 60, scope);
    else if (mins >= 90) snprintf(line, sizeof(line), "About %dh %02dm left in this %s", mins / 60, mins % 60, scope);
    else snprintf(line, sizeof(line), "About %d min left in this %s", mins, scope);
  else if (pagesLeft > 0)
    snprintf(line, sizeof(line), "%d page%s left in this %s", pagesLeft,
             pagesLeft == 1 ? "" : "s", scope);
  else
    snprintf(line, sizeof(line), "Last page of this %s", scope);
  gfx.drawTextCentered(kFontSmall, w / 2, y, line);

}

// Text size: a thin strip over the LIVE page — steps re-render the book
// behind it, so you see exactly what you chose.
void ReaderScene::renderSizeStrip(Gfx& gfx) {
  const int w = contentRight(gfx, 0);  // landscape: stop before the key column
  // Two explanation lines need a taller panel than the three stops do.
  const bool oneSize = _fbp && _fbp->sizeCount() <= 1;
  const int kStripH = oneSize ? 116 : 84;  // label + stops; the soft keys say the rest
  const int y0 = contentBottom(gfx, 0) - kStripH;
  // A white gutter above the panel. Without it the strip's border landed
  // through the middle of a line of the live page and sliced the glyphs in
  // half, which reads as a drawing fault rather than as an overlay.
  gfx.fillRect(0, y0 - 8, w, 8, false);
  gfx.drawRoundedRect(8, y0, w - 16, kStripH - 2, 10, 2, true);
  int y = y0 + 12;
  gfx.drawText(kFontSmall, 26, y, "TEXT SIZE");
  y += gfx.lineHeight(kFontSmall) + 8;
  // A package built for another panel falls back to one size
  // (FbpBook::selectProfile). Showing three stops that do nothing read
  // as a broken device — Andrew chased exactly that on the X4
  // (2026-08-23, X3-built books). Say what is true instead.
  if (oneSize) {
    gfx.drawText(kFontRegular, 26, y, "This copy has one text size.");
    y += gfx.lineHeight(kFontRegular) + 2;
    gfx.drawText(kFontRegular, 26, y, "Sync in the app to rebuild it.");
    return;
  }
  const int active = currentSizeStop(_fbp.get(), _settings.fontId);
  static constexpr const char* kStops[3] = {"Small", "Medium", "Large"};
  int x = 26;
  for (int i = 0; i < 3; i++) {
    const XpFont& f = i == active ? kFontBold : kFontRegular;
    const int tw = gfx.textWidth(f, kStops[i]);
    if (i == active) gfx.drawRect(x - 8, y - 4, tw + 16, gfx.lineHeight(f) + 8, 1, true);
    gfx.drawText(f, x, y, kStops[i]);
    x += tw + 34;
  }
}

// Go to page: live preview — the book pages underneath as you step.
void ReaderScene::renderGoToStrip(Gfx& gfx) {
  if (!_fbp) return;
  const int w = contentRight(gfx, 0);  // landscape: stop before the key column
  constexpr int kStripH = 92;
  const int y0 = contentBottom(gfx, 0) - kStripH;
  gfx.fillRect(0, y0 - 8, w, 8, false);  // gutter, see renderSizeStrip
  gfx.drawRoundedRect(8, y0, w - 16, kStripH - 2, 10, 2, true);
  int y = y0 + 10;
  gfx.drawText(kFontSmall, 26, y, "GO TO PAGE");
  // Two truths this line got wrong: the fonts are ASCII subsets so the "\xb1"
  // it used to carry drew as "?", and the second pair of buttons is only on
  // the TOP edge while the device is held portrait — turned landscape they
  // are along a side.
  const char* jump = _landscape ? "SIDE KEYS JUMP 10" : "TOP KEYS JUMP 10";
  gfx.drawText(kFontSmall, w - 26 - gfx.textWidth(kFontSmall, jump), y, jump);
  y += gfx.lineHeight(kFontSmall) + 6;
  char line[48];
  snprintf(line, sizeof(line), "%u  of  %u", _fbpPage + 1, _fbp->pageCount());
  gfx.drawTextCentered(kFontBold, w / 2, y, line);
}

// Bookmarks: the places you kept, most recent last (add order).
void ReaderScene::renderBookmarks(Gfx& gfx) {
  const int w = contentRight(gfx, 0);
  gfx.fillRect(0, 0, w, gfx.height(), false);
  gfx.drawText(kFontBold, 20, 10, "Bookmarks");
  if (_markCount > 0) {
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "%d / %d", _markSel + 1, _markCount);
    gfx.drawText(kFontRegular, w - 20 - gfx.textWidth(kFontRegular, hdr), 10, hdr);
  }
  gfx.fillRect(0, 44, w, 2, true);

  if (_markCount == 0) {
    gfx.drawTextCentered(kFontRegular, w / 2, 120, "No bookmarks in this book yet");
    gfx.drawTextCentered(kFontSmall, w / 2, 156, "Menu > Bookmark this page keeps a place");
  }

  const int rowH = 46;
  const int listTop = 56;
  const int visible = (contentBottom(gfx, 0) - listTop - 30) / rowH;
  if (_markSel < _markScroll) _markScroll = _markSel;
  if (_markSel >= _markScroll + visible) _markScroll = _markSel - visible + 1;
  if (_markScroll > _markCount - visible) _markScroll = _markCount - visible;
  if (_markScroll < 0) _markScroll = 0;

  char line[64];
  for (int i = 0; i < visible && _markScroll + i < _markCount; i++) {
    const int idx = _markScroll + i;
    const int y = listTop + i * rowH;
    const bool sel = idx == _markSel;
    if (sel) gfx.fillRect(0, y, w, rowH - 4, true);
    const int textY = y + (rowH - 4 - gfx.lineHeight(kFontRegular)) / 2;
    char right[16];
    right[0] = 0;
    if (_fbp) {
      const int total = (int)_fbp->pageCount();
      const int pct = total > 0 ? (int)((_marks[idx].page + 1) * 100.0f / total + 0.5f) : 0;
      snprintf(right, sizeof(right), "%d%%", pct);
      // Name the chapter the mark sits in: "Page 268" says nothing about
      // WHERE you were. The TOC is already in the package.
      char chap[64];
      chap[0] = 0;
      const uint32_t n = _fbp->tocCount();
      for (uint32_t t = 0; t < n; t++) {
        uint32_t cid = 0;
        char title[64];
        if (!_fbp->tocEntry(t, title, sizeof(title), &cid)) break;
        if (_fbp->pageForContentId(cid) <= _marks[idx].page) {
          if (title[0] && gfx.canRender(kFontRegular, title)) snprintf(chap, sizeof(chap), "%s", title);
          else chap[0] = 0;
        } else {
          break;
        }
      }
      if (chap[0]) snprintf(line, sizeof(line), "%s  -  p%u", chap, _marks[idx].page + 1);
      else snprintf(line, sizeof(line), "Page %u", _marks[idx].page + 1);
    } else {
      snprintf(line, sizeof(line), "Chapter %u, page %u", _marks[idx].spine + 1,
               _marks[idx].page + 1);
    }
    char shown[64];
    const int reserve = right[0] ? gfx.textWidth(kFontRegular, right) + 24 : 20;
    truncateToWidth(gfx, sel ? kFontBold : kFontRegular, line, w - 40 - reserve, shown,
                    sizeof(shown));
    gfx.drawText(sel ? kFontBold : kFontRegular, 20, textY, shown, !sel);
    if (right[0])
      gfx.drawText(kFontRegular, w - 20 - gfx.textWidth(kFontRegular, right), textY, right, !sel);
  }

  if (_markCount > 0)
    gfx.drawTextCentered(kFontSmall, w / 2, contentBottom(gfx, 0) - 26, "Hold GO to remove");
}

// Reading stats, split in two (Andrew 2026-08-16: "we're mixing overall
// reading stats with book reading stats, they should not be crammed onto
// the same page"). Two pages, each answering ONE question:
//   THIS BOOK  — where am I in this book, and how long is left?
//   READING LIFE — how is my reading going overall?
// Every number is measured; nothing is projected or padded.

// Shared: "6h 12m" / "42 min" from a minute count.
static void formatDuration(uint32_t minutes, char* out, size_t cap) {
  if (minutes >= 60) snprintf(out, cap, "%luh %02lum", (unsigned long)(minutes / 60),
                              (unsigned long)(minutes % 60));
  else snprintf(out, cap, "%lu min", (unsigned long)minutes);
}

// One label/value row over a hairline, the page's typographic workhorse.
static int statRow(Gfx& gfx, int y, int left, int right, const char* label, const char* value,
                   int pad = 12) {
  gfx.fillRect(left, y, right - left, 1, true);
  const int textY = y + pad;
  gfx.drawText(kFontRegular, left, textY, label);
  if (value && value[0])
    gfx.drawText(kFontRegular, right - gfx.textWidth(kFontRegular, value), textY, value);
  return textY + gfx.lineHeight(kFontRegular) + pad;
}

// A stats page collects its rows first and lays them out afterwards, because
// the space underneath is a third of a page in landscape and most of one in
// portrait. Same rows either way: what changes is the number of columns.
struct StatRows {
  static constexpr int kMax = 10;
  char label[kMax][26];
  char value[kMax][26];
  int n = 0;
  void add(const char* l, const char* v) {
    if (n >= kMax || !l || !l[0]) return;
    snprintf(label[n], sizeof(label[0]), "%s", l);
    snprintf(value[n], sizeof(value[0]), "%s", v ? v : "");
    n++;
  }
};

// Draw the rows between y0 and yEnd across `left..right`, in one column if
// they fit and two if they do not. Padding tightens before anything is
// dropped; a row that STILL does not fit is not drawn, and the count of
// what was left out is returned so the caller can be honest about it.
static int drawStatRows(Gfx& gfx, const StatRows& r, int y0, int left, int right, int yEnd) {
  if (r.n == 0) return 0;
  const int lineH = gfx.lineHeight(kFontRegular);
  const int avail = yEnd - y0;
  if (avail < lineH) return r.n;
  // Roomy padding first; tighten only if that alone would not seat the list
  // in one column. Below 5 px the hairlines start to crowd the text.
  int pad = 12;
  if (r.n * (lineH + 2 * pad) > avail) {
    pad = (avail / r.n - lineH) / 2;
    if (pad < 5) pad = 5;
    if (pad > 12) pad = 12;
  }
  const int rowH = lineH + 2 * pad;
  int perCol = avail / rowH;
  if (perCol < 1) perCol = 1;
  // Columns are filled TOP-DOWN then left-to-right, so when the list is
  // longer than the space the rows that survive are the first ones added —
  // which is why the callers add them most-wanted first.
  int cols = (r.n + perCol - 1) / perCol;
  if (cols > 2) cols = 2;  // three columns of label+value do not read
  if (cols < 1) cols = 1;
  if (cols == 1 && perCol > r.n) perCol = r.n;
  const int gap = 28;
  const int colW = (right - left - (cols - 1) * gap) / cols;
  int drawn = 0;
  for (int c = 0; c < cols; c++) {
    const int cl = left + c * (colW + gap);
    int y = y0;
    for (int i = c * perCol; i < r.n && i < (c + 1) * perCol; i++) {
      y = statRow(gfx, y, cl, cl + colW, r.label[i], r.value[i], pad);
      drawn++;
    }
    gfx.fillRect(cl, y, colW, 1, true);  // close the column
  }
  return r.n - drawn;  // how many did not fit
}

void ReaderScene::renderStatsBook(Gfx& gfx) {
  const int w = gfx.width();
  const int left = 24, right = contentRight(gfx, 24);
  gfx.fillRect(0, 0, w, gfx.height(), false);
  char line[96];

  gfx.drawText(kFontSmall, left, 14, "THIS BOOK");
  gfx.fillRect(left, 40, right - left, 2, true);

  // Cover, at its natural size; the text column sits beside it.
  const int coverY = 60;
  int coverW = 0, coverH = 0;
  bool drewCover = false;
  {
    char cov[112];
    uint16_t cw = 0, ch = 0;
    cov[0] = 0;
    if (_fbp) snprintf(cov, sizeof(cov), "%s.cov", _bookPath.c_str());
    if (!cov[0] || !readThumbDims(cov, &cw, &ch)) {
      for (int i = 0; i < _bookCount; i++) {
        if (_bookPath == _books[i].path && _books[i].thumbPath[0]) {
          snprintf(cov, sizeof(cov), "%s", _books[i].thumbPath);
          cw = _books[i].thumbW;
          ch = _books[i].thumbH;
          break;
        }
      }
    }
    if (cw > 0 && ch > 0) {
      coverW = cw;
      coverH = ch;
      drewCover = reader::CoverThumb::draw(gfx, cov, left, coverY);
    }
  }
  if (!drewCover) {
    coverW = 150;
    coverH = 200;
    gfx.drawTextCentered(kFontSmall, left + coverW / 2, coverY + coverH / 2 - 8, "no cover");
  }
  gfx.drawRect(left, coverY, coverW, coverH, 2, true);

  // Title / author / the one number that matters.
  const int bx = left + coverW + 22;
  int by = coverY;
  const char* title = _fbp ? _fbp->title() : nullptr;
  const char* author = _fbp ? _fbp->author() : "";
  if (!title) {
    for (int i = 0; i < _bookCount; i++) {
      if (_bookPath == _books[i].path && _books[i].title[0]) {
        title = _books[i].title;
        author = _books[i].author;
        break;
      }
    }
  }
  static char prettyTitle[96];
  if (!title || !title[0]) {
    prettyFileTitle(_bookPath.c_str(), prettyTitle, sizeof(prettyTitle));
    title = prettyTitle;
  }
  if (gfx.canRender(kFontBold, title)) {
    // drawTextWrapped returns the LINE COUNT, not a y — advance by hand.
    const int titleLines = gfx.drawTextWrapped(kFontBold, bx, by, title, right - bx, 2);
    by += (titleLines > 0 ? titleLines : 1) * gfx.lineHeight(kFontBold) + 4;
    if (author && author[0] && gfx.canRender(kFontSmall, author)) {
      truncateToWidth(gfx, kFontSmall, author, right - bx, line, sizeof(line));
      gfx.drawText(kFontSmall, bx, by, line);
      by += gfx.lineHeight(kFontSmall);
    }
  } else {
    char strip[112];
    snprintf(strip, sizeof(strip), "%s.str", _bookPath.c_str());
    uint16_t sw = 0, sh = 0;
    if (_fbp && readThumbDims(strip, &sw, &sh) && sw > 0 && sw <= right - bx &&
        reader::CoverThumb::draw(gfx, strip, bx, by))
      by += sh;
  }

  uint32_t bookPages = 0, bookMinutes = 0, lastDay = 0, firstDay = 0;
  uint16_t daysRead = 0;
  reader::ReadingStats::bookStats(_bookPath, &bookPages, &bookMinutes, &lastDay, &firstDay,
                                  &daysRead);
  const float frac = bookProgress();
  int page = 0, total = 0;
  if (_fbp) {
    total = static_cast<int>(_fbp->pageCount());
    page = _fbpPage + 1;
  } else if (_section) {
    total = static_cast<int>(_section->pageCount);
    page = _section->currentPage + 1;
  }
  int pagesLeft = total - page;
  if (pagesLeft < 0) pagesLeft = 0;
  const int mins = estimateMinutesLeft(pagesLeft);

  by += 18;
  if (mins > 0) {
    formatDuration(static_cast<uint32_t>(mins), line, sizeof(line));
    gfx.drawTextScaled(kFontBold, bx, by, line, 2);
    by += gfx.lineHeightScaled(kFontBold, 2) + 2;
    gfx.drawText(kFontSmall, bx, by, "LEFT AT YOUR PACE");
  } else {
    snprintf(line, sizeof(line), "%d", pagesLeft);
    gfx.drawTextScaled(kFontBold, bx, by, line, 2);
    by += gfx.lineHeightScaled(kFontBold, 2) + 2;
    // EPUBs count pages per CHAPTER. Unlabeled, "0 PAGES LEFT" beside a 0%
    // progress bar read as a contradiction (pedantic walk, 2026-08-17).
    if (_fbp) gfx.drawText(kFontSmall, bx, by, pagesLeft == 1 ? "PAGE LEFT" : "PAGES LEFT");
    else gfx.drawText(kFontSmall, bx, by, "LEFT IN CHAPTER");
  }
  by += gfx.lineHeight(kFontSmall);

  // Progress spans the full width under both columns.
  int y = (by > coverY + coverH ? by : coverY + coverH) + 26;
  gfx.drawRect(left, y, right - left, 18, 2, true);
  gfx.fillRect(left, y, static_cast<int>((right - left) * frac + 0.5f), 18, true);
  y += 18 + 10;
  snprintf(line, sizeof(line), _fbp ? "%d%% through  -  page %d of %d"
                                    : "%d%% through  -  page %d of %d in this chapter",
           static_cast<int>(frac * 100 + 0.5f), page, total);
  gfx.drawText(kFontSmall, left, y, line);
  y += gfx.lineHeight(kFontSmall) + 22;

  // The book's own numbers — collected most-wanted first, then laid out to
  // whatever room this orientation left underneath.
  StatRows rows;
  char v[32];
  formatDuration(bookMinutes, line, sizeof(line));
  rows.add("Time with this book", line);
  snprintf(v, sizeof(v), "%lu", (unsigned long)bookPages);
  rows.add("Pages you have read", v);
  if (_avgTurnMs > 0) {
    const uint32_t sec = _avgTurnMs / 1000;
    if (sec >= 60) snprintf(v, sizeof(v), "%lum %02lus a page", (unsigned long)(sec / 60),
                            (unsigned long)(sec % 60));
    else snprintf(v, sizeof(v), "%lus a page", (unsigned long)sec);
    rows.add("Your pace", v);
  }
  int chapCount = 0, chapCur = 0;
  chapterCountAndSel(&chapCount, &chapCur);
  if (chapCount > 0) {
    snprintf(v, sizeof(v), "%d of %d", chapCur + 1, chapCount);
    rows.add("Chapter", v);
  }
  if (daysRead > 0) {
    snprintf(v, sizeof(v), daysRead == 1 ? "%u day" : "%u days", daysRead);
    rows.add("Days with this book", v);
  }
  static constexpr const char* kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const uint32_t today = reader::ReadingStats::todayYmdPublic();
  if (firstDay > 0) {
    const int mo = static_cast<int>((firstDay / 100u) % 100u);
    if (mo >= 1 && mo <= 12)
      snprintf(v, sizeof(v), "%s %lu", kMon[mo - 1], (unsigned long)(firstDay % 100u));
    else
      v[0] = 0;
    if (v[0]) rows.add("Started", v);
  }
  if (lastDay > 0) {
    if (lastDay == today) {
      snprintf(v, sizeof(v), "today");
    } else {
      const int mo = static_cast<int>((lastDay / 100u) % 100u);
      if (mo >= 1 && mo <= 12)
        snprintf(v, sizeof(v), "%s %lu", kMon[mo - 1], (unsigned long)(lastDay % 100u));
      else
        v[0] = 0;
    }
    if (v[0]) rows.add("Last read", v);
  }
  // Landscape has no bottom key bar — the column is on the right — so the
  // rows own the panel all the way down there, and only portrait reserves it.
  const int yEnd = contentBottom(gfx, 0);
  const int dropped = drawStatRows(gfx, rows, y, left, right, yEnd);
  if (dropped > 0) Serial.printf("[xphone-os] stats: %d book rows did not fit\n", dropped);
}

void ReaderScene::renderStatsLife(Gfx& gfx) {
  const int w = gfx.width();
  const int left = 24, right = contentRight(gfx, 24);
  gfx.fillRect(0, 0, w, gfx.height(), false);
  char line[96];

  const reader::ReadingStats::Band b = reader::ReadingStats::band();
  const reader::ReadingStats::Summary sum = reader::ReadingStats::summary();

  gfx.drawText(kFontSmall, left, 14, "READING LIFE");
  if (b.clockValid) {
    static constexpr const char* kMonths[12] = {"JANUARY",   "FEBRUARY", "MARCH",    "APRIL",
                                                "MAY",       "JUNE",     "JULY",     "AUGUST",
                                                "SEPTEMBER", "OCTOBER",  "NOVEMBER", "DECEMBER"};
    const uint32_t today = reader::ReadingStats::todayYmdPublic();
    const int mo = static_cast<int>((today / 100u) % 100u);
    if (mo >= 1 && mo <= 12) {
      snprintf(line, sizeof(line), "%s %lu", kMonths[mo - 1], (unsigned long)(today / 10000u));
      gfx.drawText(kFontSmall, right - gfx.textWidth(kFontSmall, line), 14, line);
    }
  } else {
    // No clock since boot: a small pill, the same affordance Notifications
    // uses — not two lines of meta text eating the top of the page.
    const char* label = "NEEDS SYNC";
    const int capMid = 14 + gfx.capTopOffset(kFontSmall) + gfx.capHeight(kFontSmall) / 2;
    const int pillW = gfx.textWidth(kFontSmall, label) + 22;
    const int pillH = gfx.capHeight(kFontSmall) + 12;
    const int pillX = right - pillW;
    gfx.drawRoundedRect(pillX, capMid - pillH / 2, pillW, pillH, pillH / 2, 1, true);
    gfx.drawTextCentered(kFontSmall, pillX + pillW / 2,
                         capMid - gfx.capHeight(kFontSmall) / 2 - gfx.capTopOffset(kFontSmall),
                         label, true);
  }
  gfx.fillRect(left, 40, right - left, 2, true);

  int y = 62;
  bool heroIsBooks = false;
  if (b.clockValid) {
    // Never lead with a giant "0" — it is the loudest mark on the page and
    // it would be a scold. Show whatever is true and encouraging.
    char heroNum[24];
    const char* heroLabel;
    char heroSub[48];
    heroSub[0] = 0;
    const uint16_t todayMin = reader::ReadingStats::todayMinutes();
    if (b.streakDays > 0) {
      snprintf(heroNum, sizeof(heroNum), "%u", b.streakDays);
      heroLabel = b.streakDays == 1 ? "day in a row" : "days in a row";
      if (sum.longestStreak > 0)
        snprintf(heroSub, sizeof(heroSub), sum.longestStreak == 1 ? "BEST EVER %u DAY"
                                                                  : "BEST EVER %u DAYS",
                 sum.longestStreak);
    } else if (todayMin > 0 || b.todayPages > 0) {
      snprintf(heroNum, sizeof(heroNum), "%u", todayMin > 0 ? todayMin : b.todayPages);
      heroLabel = todayMin > 0 ? "minutes today" : "pages today";
      snprintf(heroSub, sizeof(heroSub), "%s",
               sum.longestStreak > 0 ? "READING AGAIN TODAY" : "YOUR FIRST DAY");
    } else {
      snprintf(heroNum, sizeof(heroNum), "%u", sum.booksThisYear);
      heroLabel = sum.booksThisYear == 1 ? "book this year" : "books this year";
      heroIsBooks = true;
      if (sum.longestStreak > 0)
        snprintf(heroSub, sizeof(heroSub), sum.longestStreak == 1 ? "BEST STREAK %u DAY"
                                                                  : "BEST STREAK %u DAYS",
                 sum.longestStreak);
    }
    gfx.drawTextScaled(kFontBold, left, y, heroNum, 3);
    const int textX = left + gfx.textWidthScaled(kFontBold, heroNum, 3) + 22;
    gfx.drawText(kFontRegular, textX, y + 14, heroLabel);
    if (heroSub[0]) gfx.drawText(kFontSmall, textX, y + 50, heroSub);
    y += gfx.lineHeightScaled(kFontBold, 3) + 14;

    // The month, two-state: filled = read, hollow = not. Magnitude by AREA,
    // never by shade — a 1-bit panel has no grays to grade with.
    const int days = sum.monthDays ? sum.monthDays : 31;
    int gap = 4;
    int cell = (right - left - (days - 1) * gap) / days;
    if (cell > 14) cell = 14;
    if (cell < 4) {  // narrow panel: give the gap back before the cell
      cell = 4;
      gap = 3;
    }
    // Centre the strip on whatever it actually measures, so it can never
    // run a cell past the right margin.
    const int stripW = days * cell + (days - 1) * gap;
    int sx = left + ((right - left) - stripW) / 2;
    if (sx < left) sx = left;
    for (int d = 0; d < days; d++) {
      if (sum.monthMask & (1u << d)) gfx.fillRect(sx, y, cell, cell, true);
      else gfx.drawRect(sx, y, cell, cell, 2, true);
      sx += cell + gap;
    }
    y += cell + 12;
    snprintf(line, sizeof(line), "%u of %u days this month", sum.monthRead, days);
    gfx.drawText(kFontSmall, left, y, line);
    y += gfx.lineHeight(kFontSmall) + 24;
  }

  // One chart: the week, scaled to its own peak day (KOReader's rule).
  // With no date the bars would be seven empty boxes — a scaffold with no
  // data in it — so the chart only appears when it can say something.
  if (b.clockValid) {
    gfx.fillRect(left, y, right - left, 2, true);
    y += 14;
    // Every number on this line carries its own scope word. It used to read
    // "PAGES THIS WEEK ... 16 pages - 62 MIN TODAY", where "16" could have
    // been either.
    gfx.drawText(kFontSmall, left, y, "THIS WEEK");
    uint32_t weekPages = 0;
    for (int i = 0; i < 7; i++) weekPages += b.weekPages[i];
    const uint16_t todayMin = reader::ReadingStats::todayMinutes();
    if (todayMin > 0)
      snprintf(line, sizeof(line), "%lu PAGES  -  %u MIN TODAY", (unsigned long)weekPages, todayMin);
    else
      snprintf(line, sizeof(line), "%lu PAGES", (unsigned long)weekPages);
    gfx.drawText(kFontSmall, right - gfx.textWidth(kFontSmall, line), y, line);
    y += gfx.lineHeight(kFontSmall) + 12;

    // Fill the page: the chart takes whatever the totals block does not.
    const int totalsBlock = gfx.lineHeightScaled(kFontBold, 2) + gfx.lineHeight(kFontSmall) * 2 +
                            40 + 16;
    const int labelsH = gfx.lineHeight(kFontSmall) + 8 + 24;
    int chartH = (contentBottom(gfx, 0) - 34) - y - labelsH - totalsBlock;
    if (chartH > 240) chartH = 240;
    if (chartH < 70) chartH = 70;
    uint16_t peak = 1;
    for (int i = 0; i < 7; i++)
      if (b.weekPages[i] > peak) peak = b.weekPages[i];
    const int slot = (right - left) / 7;
    const int barWidth = slot - 10;
    static constexpr const char* kDayLetters[7] = {"M", "T", "W", "T", "F", "S", "S"};
    // Bars rise from ONE baseline. Each day used to get a full-height hollow
    // box, so a week with a single reading day showed six empty gauges — a
    // scaffold that looked like six zeros deliberately drawn at full size.
    gfx.fillRect(left, y + chartH, right - left, 2, true);
    for (int i = 0; i < 7; i++) {
      const int x = left + i * slot;
      const int h = static_cast<int>(b.weekPages[i] * (float)chartH / peak + 0.5f);
      if (h > 0) gfx.fillRect(x, y + chartH - h, barWidth, h, true);
      else gfx.fillRect(x, y + chartH - 3, barWidth, 3, true);  // a day with nothing still counts
      // Direct-label the bar. A chart with no axis and no numbers only says
      // "one day was bigger"; the number says which day and by how much.
      if (b.weekPages[i] > 0) {
        char n[8];
        snprintf(n, sizeof(n), "%u", b.weekPages[i]);
        const int lh = gfx.lineHeight(kFontSmall);
        const bool inside = h > lh + 8;  // tall enough to hold its own label
        gfx.drawTextCentered(kFontSmall, x + barWidth / 2,
                             inside ? y + chartH - h + 4 : y + chartH - h - lh - 2, n, !inside);
      }
      const int weekday = (b.todayWeekday + 7 - (6 - i)) % 7;
      gfx.drawTextCentered(kFontSmall, x + barWidth / 2, y + chartH + 8, kDayLetters[weekday]);
    }
    y += chartH + 8 + gfx.lineHeight(kFontSmall) + 24;
  }

  // Nothing recorded at all (first run, or right after a format change):
  // an invitation, not a wall of zeros. An empty state earns its words —
  // this is the one place on the page where a sentence is the content.
  if (sum.lifetimePages == 0 && sum.lifetimeMinutes == 0) {
    const int midY = (contentBottom(gfx, 0)) / 2 - 40;
    gfx.drawTextCentered(kFontBold, w / 2, midY, "Nothing to show yet");
    gfx.drawTextCentered(kFontSmall, w / 2, midY + gfx.lineHeight(kFontBold) + 10,
                         "Read a few pages and your streak,");
    gfx.drawTextCentered(kFontSmall, w / 2,
                         midY + gfx.lineHeight(kFontBold) + 10 + gfx.lineHeight(kFontSmall) + 4,
                         "your week and your totals appear here.");
    gfx.drawTextCentered(kFontSmall, w / 2, contentBottom(gfx, 0) - 24,
                         _lifeOpen ? "BACK returns to your books" : "BACK returns to the menu");
    return;
  }

  // Without a date the calendar, the streak and the week chart all have
  // nothing to stand on, and two lonely numbers in the middle of the panel
  // read as a screen that failed to draw. Show the four totals that need no
  // clock at all, as a 2x2 of heroes that fills the page honestly.
  if (!b.clockValid) {
    // Exactly three things are true without a date — every other number on
    // this page is a per-DAY record and would be a giant honest zero, which
    // is the loudest mark on a 1-bit panel and reads as a scold. Three big
    // tiles under one heading, centred, is the whole page.

    const int headH = gfx.lineHeight(kFontSmall) + 18;
    const int tileH = gfx.lineHeightScaled(kFontBold, 3) + gfx.lineHeight(kFontSmall) + 6;
    const int spaceTop = y;
    const int spaceBottom = contentBottom(gfx, 0) - 20;
    int ty = spaceTop + (spaceBottom - spaceTop - headH - tileH) / 2;
    if (ty < spaceTop) ty = spaceTop;
    gfx.drawText(kFontSmall, left, ty, "ALL TIME");
    ty += headH;
    const int colW = (right - left) / 3;
    const char* labels[3];
    char nums[3][24];
    snprintf(nums[0], sizeof(nums[0]), "%lu", (unsigned long)sum.lifetimePages);
    labels[0] = "PAGES";
    if (sum.lifetimeMinutes >= 60)
      snprintf(nums[1], sizeof(nums[1]), "%luh", (unsigned long)(sum.lifetimeMinutes / 60));
    else
      snprintf(nums[1], sizeof(nums[1]), "%lum", (unsigned long)sum.lifetimeMinutes);
    labels[1] = "READING";
    snprintf(nums[2], sizeof(nums[2]), "%u", sum.booksAllTime);
    labels[2] = sum.booksAllTime == 1 ? "BOOK" : "BOOKS";
    // Drop a step rather than let a wide number run into its neighbour: on
    // the narrower panel a three-digit page count at scale 3 already touches
    // the next column.
    int scale = 3;
    while (scale > 1) {
      int widest = 0;
      for (int i = 0; i < 3; i++) {
        const int tw = gfx.textWidthScaled(kFontBold, nums[i], scale);
        if (tw > widest) widest = tw;
      }
      if (widest <= colW - 14) break;
      scale--;
    }
    for (int i = 0; i < 3; i++) {
      const int tx = left + i * colW;
      gfx.drawTextScaled(kFontBold, tx, ty, nums[i], scale);
      gfx.drawText(kFontSmall, tx, ty + gfx.lineHeightScaled(kFontBold, scale) + 6, labels[i]);
    }

    return;
  }
  {
    gfx.fillRect(left, y, right - left, 2, true);
    y += 16;
  }
  const int col2 = left + (right - left) / 2;
  const bool pagesInTile = heroIsBooks;  // the no-clock page returned above
  if (pagesInTile) {
    snprintf(line, sizeof(line), "%lu", (unsigned long)sum.lifetimePages);
    gfx.drawTextScaled(kFontBold, left, y, line, 2);
    gfx.drawText(kFontSmall, left, y + gfx.lineHeightScaled(kFontBold, 2) + 4, "PAGES ALL TIME");
  } else {
    snprintf(line, sizeof(line), "%u", sum.booksThisYear);
    gfx.drawTextScaled(kFontBold, left, y, line, 2);
    gfx.drawText(kFontSmall, left, y + gfx.lineHeightScaled(kFontBold, 2) + 4, "BOOKS THIS YEAR");
  }
  if (sum.lifetimeMinutes >= 60)
    snprintf(line, sizeof(line), "%luh", (unsigned long)(sum.lifetimeMinutes / 60));
  else
    snprintf(line, sizeof(line), "%lum", (unsigned long)sum.lifetimeMinutes);
  gfx.drawTextScaled(kFontBold, col2, y, line, 2);
  gfx.drawText(kFontSmall, col2, y + gfx.lineHeightScaled(kFontBold, 2) + 4, "READING ALL TIME");
  y += gfx.lineHeightScaled(kFontBold, 2) + gfx.lineHeight(kFontSmall) + 20;

  char best[40];
  if (sum.bestDayMinutes >= 60)
    snprintf(best, sizeof(best), "Best day %uh %02um", sum.bestDayMinutes / 60,
             sum.bestDayMinutes % 60);
  else if (sum.bestDayMinutes > 0)
    snprintf(best, sizeof(best), "Best day %u min", sum.bestDayMinutes);
  else
    best[0] = 0;
  if (best[0] && !pagesInTile)
    snprintf(line, sizeof(line), "%s  -  %lu pages all time", best,
             (unsigned long)sum.lifetimePages);
  else if (best[0])
    snprintf(line, sizeof(line), "%s", best);
  else if (!pagesInTile)
    snprintf(line, sizeof(line), "%lu pages all time", (unsigned long)sum.lifetimePages);
  else
    line[0] = 0;
  if (line[0]) gfx.drawText(kFontSmall, left, y, line);

}

// Chapters: a full page of its own (the one list that can be long).
void ReaderScene::renderChapters(Gfx& gfx) {
  const int w = contentRight(gfx, 0);
  gfx.fillRect(0, 0, w, gfx.height(), false);
  int count = 0, cur = 0;
  chapterCountAndSel(&count, &cur);

  gfx.drawText(kFontBold, 20, 10, "Chapters");
  char hdr[24];
  snprintf(hdr, sizeof(hdr), "%d / %d", _tocSel + 1, count > 0 ? count : 1);
  gfx.drawText(kFontRegular, w - 20 - gfx.textWidth(kFontRegular, hdr), 10, hdr);
  gfx.fillRect(0, 44, w, 2, true);

  const int rowH = 46;
  const int listTop = 56;
  const int visible = (contentBottom(gfx, 0) - listTop - 30) / rowH;
  if (_tocSel < _tocScroll) _tocScroll = _tocSel;
  if (_tocSel >= _tocScroll + visible) _tocScroll = _tocSel - visible + 1;
  // Clamp DOWN as well. The window only ever grew before, so a scroll set
  // while the list was long (or the panel turned) stuck there — the first
  // chapter simply could not be reached again.
  if (_tocScroll > count - visible) _tocScroll = count - visible;
  if (_tocScroll < 0) _tocScroll = 0;

  char title[64];
  char shown[64];
  for (int i = 0; i < visible && _tocScroll + i < count; i++) {
    const int idx = _tocScroll + i;
    const int y = listTop + i * rowH;
    if (_fbp) {
      if (!_fbp->tocEntry((uint32_t)idx, title, sizeof(title), nullptr)) continue;
      if (!title[0]) snprintf(title, sizeof(title), "Chapter %d", idx + 1);
    } else {
      snprintf(title, sizeof(title), "Chapter %d", idx + 1);
    }
    const bool sel = idx == _tocSel;
    if (sel) gfx.fillRect(0, y, w, rowH - 4, true);
    const int textY = y + (rowH - 4 - gfx.lineHeight(kFontRegular)) / 2;
    // The reader's current chapter gets a quiet marker. A bare "<" used to
    // sit here; now that arrows are the button language it read as a stray
    // key mark. A word cannot be mistaken for one.
    truncateToWidth(gfx, sel ? kFontBold : kFontRegular, title, w - 96, shown, sizeof(shown));
    gfx.drawText(sel ? kFontBold : kFontRegular, 20, textY, shown, !sel);
    if (idx == cur) {
      const int nw = gfx.textWidth(kFontSmall, "NOW");
      gfx.drawText(kFontSmall, w - 24 - nw, textY + 3, "NOW", !sel);
    }
  }

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
  snprintf(right, sizeof(right), "ch %d of %d", _spine + 1, _epub->getSpineItemsCount());

  const int lineH = gfx.lineHeight(kFontSmall);
  const int y = contentBottom(gfx, 0) - kStatusH + (kStatusH - lineH) / 2;
  constexpr int kDot = 3;
  constexpr int kGap = 12;
  const int wl = gfx.textWidth(kFontSmall, left);
  const int wr = gfx.textWidth(kFontSmall, right);
  const int total = wl + kGap + kDot + kGap + wr;
  const int x = (contentRight(gfx, 0) - total) / 2;
  gfx.drawText(kFontSmall, x, y, left);
  gfx.fillRect(x + wl + kGap, y + lineH / 2 - kDot / 2, kDot, kDot, true);
  gfx.drawText(kFontSmall, x + wl + kGap + kDot + kGap, y, right);
}

void ReaderScene::renderBookList(Gfx& gfx) {
  if (_lifeOpen) {
    renderStatsLife(gfx);
    return;
  }
  const int w = gfx.width();
  const int perPage = kGridCols * kGridRows;
  // Clamp against a shrunken list (rescan may have removed entries); the
  // scroll stays row-aligned.
  if (_sel > _totalBooks - 1) _sel = _totalBooks > 0 ? _totalBooks - 1 : 0;
  if (_scroll > _sel) _scroll = (_sel / kGridCols) * kGridCols;
  if (_sel >= _scroll + perPage) _scroll = (_sel / kGridCols - (kGridRows - 1)) * kGridCols;
  if (_scroll < 0) _scroll = 0;

  // Header.
  char line[64];
  // Just "Reader": the right-hand range already ends in the same total, and
  // "Reader (8)   1-4 of 8" said eight twice on one line.
  gfx.drawText(kFontBold, kListMarginX, 8, "Read");
  if (_totalBooks > perPage) {
    snprintf(line, sizeof(line), "%d-%d of %d", _scroll + 1,
             (_scroll + perPage < _totalBooks) ? _scroll + perPage : _totalBooks, _totalBooks);
    gfx.drawText(kFontRegular, w - kListMarginX - gfx.textWidth(kFontRegular, line), 8, line);
  } else if (_totalBooks > 0) {
    snprintf(line, sizeof(line), _totalBooks == 1 ? "%d book" : "%d books", _totalBooks);
    gfx.drawText(kFontRegular, w - kListMarginX - gfx.textWidth(kFontRegular, line), 8, line);
  }
  gfx.fillRect(0, kHeaderH - 2, w, 2, true);

  // ── Stats band (concept A, compact): "N DAY STREAK   N PAGES TODAY"
  // inline on one baseline, THIS calendar week Mon–Sun as bars on the right
  // (trailing-7 ordering read as alphabet soup — "TWTFSSM" — on hardware).
  {
    const reader::ReadingStats::Band sb = reader::ReadingStats::band();
    const int bandY = kHeaderH;
    const int numY = bandY + 5;
    // Selected (cursor above the first book): a border makes it read as a
    // target rather than decoration, and OPEN goes to your reading.
    if (_sel < 0) gfx.drawRoundedRect(4, bandY + 2, gfx.width() - 8, kStatsBandH - 8, 8, 3, true);
    // Number and label share a cap BASELINE (their cap bottoms line up); the
    // old nudge was a line-height difference, which is not the same thing.
    const int capNudge = gfx.capHeight(kFontBold) - gfx.capHeight(kFontSmall) +
                         gfx.capTopOffset(kFontBold) - gfx.capTopOffset(kFontSmall);
    char num[16];

    if (!sb.clockValid) {
      // No phone time since boot: days can't be attributed, but the
      // lifetime totals are still true — show those rather than nagging.
      // (A small NO DATE pill on the right carries the "why", the same
      // way Notifications carries SYNC.)
      const reader::ReadingStats::Summary sum = reader::ReadingStats::summary();
      // Nothing recorded yet: say nothing. "0 PAGES ALL TIME" is noise on
      // a shelf, and the band's rule alone is a clean separator.
      if (sum.lifetimePages > 0) {
        int x = kListMarginX;
        char num[16];
        snprintf(num, sizeof(num), "%lu", (unsigned long)sum.lifetimePages);
        gfx.drawText(kFontBold, x, numY, num);
        x += gfx.textWidth(kFontBold, num) + 6;
        gfx.drawText(kFontSmall, x, numY + capNudge, "PAGES ALL TIME");
        // "NO DATE" named the symptom; "NEEDS SYNC" names what to do about it.
        // Centred on the CAP of the number beside it, and the label centred in
        // the pill — both were off by a few px because the old code centred on
        // line boxes, which carry different descender slack per face.
        const char* pill = "NEEDS SYNC";
        const int capMid = numY + gfx.capTopOffset(kFontBold) + gfx.capHeight(kFontBold) / 2;
        const int pillW = gfx.textWidth(kFontSmall, pill) + 20;
        const int pillH = gfx.capHeight(kFontSmall) + 12;
        const int pillX = w - kListMarginX - pillW;
        gfx.drawRoundedRect(pillX, capMid - pillH / 2, pillW, pillH, pillH / 2, 1, true);
        gfx.drawTextCentered(kFontSmall, pillX + pillW / 2,
                             capMid - gfx.capHeight(kFontSmall) / 2 - gfx.capTopOffset(kFontSmall),
                             pill);
      }
    } else {
      // The week strip owns the right end of the band; the stats fill from the
      // left and stop 20 px short of it. On the 480 px panel "0 PAGES TODAY"
      // ran up against the M of MTWTFSS with nothing between them.
      const int stripLeft = w - kListMarginX - (7 * 9 + 6 * 8) - 20;
      int x = kListMarginX;
      // Full label if it fits, short one if not, nothing at all rather than a
      // collision. The 480 px panel has 329 px before the strip and the two
      // full labels want 340 — so the X3 reads "0 PAGES TODAY" and the X4
      // reads "0 TODAY", instead of the X4 silently losing the stat.
      auto stat = [&](uint16_t n, const char* full, const char* shortLabel) {
        char v[16];
        snprintf(v, sizeof(v), "%u", n);
        const int numW = gfx.textWidth(kFontBold, v) + 6;
        const char* label = full;
        if (x + numW + gfx.textWidth(kFontSmall, full) > stripLeft) label = shortLabel;
        if (x + numW + gfx.textWidth(kFontSmall, label) > stripLeft) return;
        gfx.drawText(kFontBold, x, numY, v);
        x += numW;
        gfx.drawText(kFontSmall, x, numY + capNudge, label);
        x += gfx.textWidth(kFontSmall, label) + 22;
      };
      stat(sb.streakDays, "DAY STREAK", "STREAK");
      stat(sb.todayPages, "PAGES TODAY", "TODAY");

      // Mon..Sun of the current week; future days render as hairline stubs.
      // One type weight for all seven letters, centered under their bars:
      // the old 13 px pitch let adjacent W/M glyphs touch, and the faux-bold
      // today letter outgrew its cell and overlapped. Today is marked with a
      // 2 px underline instead — emphasis that cannot change glyph metrics.
      // Pitch is set by the WIDEST letter, not by the bar: W is 19 px and M
      // is 16 in the 10 pt face, which is the smallest the firmware has (there
      // are three fonts and no downscale). Below a ~17 px pitch adjacent
      // W/M glyphs touch — tried at 12 and they ran together. So the band
      // gets tighter VERTICALLY (46 not 56, bars 16 not 26) and keeps the
      // horizontal pitch the letters need to stay legible.
      const int barW = 9, barGap = 8, barMaxH = 16;
      const int blockW = 7 * barW + 6 * barGap;
      const int barX0 = w - kListMarginX - blockW;
      const int barBase = bandY + 4 + barMaxH;
      // Letters sit on a CAP grid, not a line-height grid: drawText's y is the
      // top of a 24 px line whose 15 px cap starts 5 px down, and assuming
      // otherwise is what pushed the today underline through the band rule.
      const int letterTop = barBase + 3 - gfx.capTopOffset(kFontSmall);
      const int letterBottom = barBase + 3 + gfx.capHeight(kFontSmall);
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
        gfx.drawText(kFontSmall, bx + (barW - lw) / 2, letterTop, letter);
        if (slot == sb.todayWeekday) gfx.fillRect(bx, letterBottom + 2, barW, 2, true);
      }
    }

    gfx.fillRect(0, kHeaderH + kStatsBandH - 2, w, 2, true);
  }

  if (_totalBooks == 0) {
    // Honest empty states: say what is actually wrong, not "add books" when
    // the card is missing or every file was skipped (0.6 workstream B).
    const int cy = gfx.height() / 2;
    if (!_sdOk) {
      gfx.drawTextCentered(kFontBold, w / 2, cy - gfx.lineHeight(kFontBold), "No SD card found");
      gfx.drawTextCentered(kFontRegular, w / 2, cy + 6, "Insert the card, then restart");
    } else if (_skippedNames > 0) {
      char msg[64];
      snprintf(msg, sizeof(msg), "%u file%s skipped", static_cast<unsigned>(_skippedNames),
               _skippedNames == 1 ? "" : "s");
      gfx.drawTextCentered(kFontBold, w / 2, cy - gfx.lineHeight(kFontBold), msg);
      gfx.drawTextCentered(kFontRegular, w / 2, cy + 6, "File names too long - shorten them");
    } else {
      gfx.drawTextCentered(kFontBold, w / 2, cy - gfx.lineHeight(kFontBold), "No books found");
      gfx.drawTextCentered(kFontRegular, w / 2, cy + 6, "Add books from the Flowe app");
    }
    return;
  }

  bool tilesNeedWork = false;
  for (int i = 0; i < perPage && _scroll + i < _totalBooks; i++) {
    if (!inWindow(_scroll + i)) continue;
    renderTile(gfx, i);
    if (entryAt(_scroll + i).meta == TileMeta::Unknown) tilesNeedWork = true;
  }
  // Lazy metadata + thumbs: one tile per quiet tick via the standard
  // arm-then-run dance. Only claims the Work slot when it is free (an
  // OpenBook queued this same tick must win).
  if (tilesNeedWork && _work == Work::None) _work = Work::GridMeta;
}

void ReaderScene::renderTile(Gfx& gfx, const int visibleIndex) {
  const int idx = _scroll + visibleIndex;
  const BookEntry& b = entryAt(idx);
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
    // FBP without a cover: the shaped title strip goes INSIDE the box —
    // never the raw title (the UI font would stamp "?????" for Arabic/CJK).
    bool drewStrip = false;
    if (endsWithFbpCI(b.path)) {
      char strip[112];
      snprintf(strip, sizeof(strip), "%s.str", b.path);
      uint16_t sw, sh;
      if (readThumbDims(strip, &sw, &sh))
        drewStrip = reader::CoverThumb::draw(gfx, strip, cx - sw / 2,
                                             thumbTop + (kThumbH - sh) / 2);
    }
    if (!drewStrip && b.meta == TileMeta::NoCover && b.title[0] != '\0') {
      const int maxLines = (kThumbH - 40) / gfx.lineHeight(kFontRegular);
      gfx.drawTextWrapped(kFontRegular, fx + 14, thumbTop + 20, b.title, kThumbW - 28, maxLines);
    }
  }

  // A focus edition looked EXACTLY like its normal twin on the shelf (two
  // "Meditations" tiles, nothing to tell them apart). Badge the artwork.
  // Must come BEFORE the strip path below, which returns early — that is
  // precisely the branch a compiled focus edition takes.
  if (b.focusEdition) {
    const char* tag = "FOCUS";
    const int chipW = gfx.textWidth(kFontSmall, tag) + 14;
    const int chipH = gfx.lineHeight(kFontSmall) + 6;
    const int chipX = cx - kThumbW / 2 + 8;
    const int chipY = thumbTop + kThumbH - chipH - 8;
    gfx.fillRect(chipX, chipY, chipW, chipH, false);  // knock out the artwork
    gfx.drawRoundedRect(chipX, chipY, chipW, chipH, chipH / 2, 2, true);
    gfx.drawTextCentered(kFontSmall, chipX + chipW / 2, chipY + 3, tag);
  }

  // FBP packages carry a phone-shaped title strip — Arabic/CJK titles render
  // pixel-perfect where the ASCII UI fonts would stamp "?????".
  //
  // Two conditions the shelf used to skip, both visible on glass: the strip
  // was preferred even for an ASCII title the UI font renders perfectly well
  // (and, being a fixed-width bitmap, it was CLIPPED by the tile with no
  // ellipsis — "Alice's Adventures in" simply stopped), and it was drawn at
  // whatever width it happened to be, overrunning into the next tile.
  const int tileTextW = r.w - 2 * kTileTextPad;
  const char* wantTitle = b.title[0] ? b.title : baseName(b.path);
  if (endsWithFbpCI(b.path) && !gfx.canRender(kFontRegular, wantTitle)) {
    char strip[112];
    snprintf(strip, sizeof(strip), "%s.str", b.path);
    uint16_t sw, sh;
    if (readThumbDims(strip, &sw, &sh) &&
        reader::CoverThumb::draw(gfx, strip, cx - sw / 2, thumbTop + kThumbH + kTitleGap, nullptr,
                                 nullptr, cx - tileTextW / 2, cx + tileTextW / 2)) {
      return;
    }
  }

  // Text block under the cover box: title over a small author/status line.
  // Regular weight on purpose (Andrew, 2026-08-15): bold sans titles next to
  // the FBP books' quiet serif strips made the shelf read as two different
  // products. Uniform look truly arrives when every book is a compiled
  // package; until then the epub tiles stop shouting.
  const XpFont& titleFont = kFontRegular;
  const char* titleSrc = wantTitle;
  // A book you are inside tells you where you are; the author only matters
  // before you start.
  char subBuf[24];
  const char* sub = b.meta == TileMeta::Unknown     ? "..."
                    : b.meta == TileMeta::NotOpened ? "unreadable"
                                                    : b.author;
  if (b.meta != TileMeta::Unknown && b.meta != TileMeta::NotOpened && b.progressPct > 0) {
    snprintf(subBuf, sizeof(subBuf), "%u%% read", b.progressPct);
    sub = subBuf;
  }
  const int textW = tileTextW;
  const int titleY = thumbTop + kThumbH + kTitleGap;
  const int tileBottom = r.y + r.h;
  char clipped[96];
  if (titleY + gfx.lineHeight(titleFont) <= tileBottom) {
    truncateToWidth(gfx, titleFont, titleSrc, textW, clipped, sizeof(clipped));
    gfx.drawTextCentered(titleFont, cx, titleY, clipped);
  }
  // The author is the first thing to go. A line that does not fit inside its
  // own tile is not "slightly tight" — it is drawn over the next row, and the
  // row below's selection border cuts it in half. Belt as well as braces: the
  // constants above are sized to fit, this guarantees it on any panel.
  const int subY = titleY + gfx.lineHeight(titleFont);
  if (sub[0] != '\0' && subY + gfx.lineHeight(kFontSmall) <= tileBottom) {
    truncateToWidth(gfx, kFontSmall, sub, textW, clipped, sizeof(clipped));
    gfx.drawTextCentered(kFontSmall, cx, subY, clipped);
  }
}

// Bench "where" v2: the launcher-level scene name is not enough — the Reader
// is a state machine, and blind navigation from "scene=reader" once opened a
// book instead of a menu. One line says exactly where the UI is.
void ReaderScene::debugShelfDump() const {
  Serial.printf("[xphone-os] shelfdump: total=%d win=%d count=%d state=%d\n", _totalBooks,
                _windowOffset, _bookCount, static_cast<int>(_state));
  for (int i = 0; i < _bookCount; i++) {
    const BookEntry& b = _books[i];
    Serial.printf("[xphone-os] shelfdump: %2d meta=%d '%s'\n", _windowOffset + i,
                  static_cast<int>(b.meta), baseName(b.path));
  }
}

void ReaderScene::debugWhere(char* out, const size_t n) const {
  switch (_state) {
    case State::BookList:
      if (_lifeOpen) {
        snprintf(out, n, "shelf:reading-life");
        return;
      }
      snprintf(out, n, "shelf sel=%d/%d win=%d", _sel, _totalBooks, _windowOffset);
      return;
    case State::Reading: {
      // menuSel and the orientation are here so a bench sweep can navigate by
      // fact: without them every menu drive is dead reckoning from whatever
      // the cursor happened to be, and the direction keys swap in landscape.
      MenuRow ids[kMenuRowCount];
      const int rows = menuRows(_fbp != nullptr, _landscapeReady, ids);
      snprintf(out, n, "reading '%s' %s%s sel=%d/%d marks=%d", baseName(_bookPath.c_str()),
               _landscape ? "landscape" : "portrait",
               _menu == MenuView::Page        ? " menu"
               : _menu == MenuView::SizeStrip  ? " menu:size"
               : _menu == MenuView::Chapters   ? " menu:chapters"
               : _menu == MenuView::GoTo       ? " menu:goto"
               : _menu == MenuView::Bookmarks  ? " menu:bookmarks"
               : _menu == MenuView::StatsBook  ? " menu:stats-book"
               : _menu == MenuView::StatsLife  ? " menu:stats-life"
                                               : "",
               _menuSel, rows, _markCount);
      return;
    }
    case State::Opening:
      snprintf(out, n, "opening '%s'", baseName(_bookPath.c_str()));
      return;
    case State::Indexing:
      snprintf(out, n, "indexing '%s'", baseName(_bookPath.c_str()));
      return;
    default:
      snprintf(out, n, "error");
      return;
  }
}
