#pragma once

// xphone-os — Reader (R2a). EPUB reading on the CrossPoint engine port in
// src/reader: paginate-once section.bin caches, replay one Page per turn.
//
// State machine (see ReaderScene.cpp header comment for the full flow):
//   Opening  -> "Opening book..." frame, then deferred Epub::load + resume
//   Indexing -> "Indexing chapter..." frame, then deferred section build
//   Reading  -> deserialize + blit the current page each render; CONFIRM
//               ("SIZE") cycles 12/14/16 pt, keeping the position by ratio
//   BookList -> 2x2 cover-thumbnail grid over SD /books (+ root) *.epub
//   Error    -> message + BACK to the book list
//
// Blocking engine work (zip inflate + expat parse + DP line breaking, cover
// decode — up to seconds) NEVER runs inside onEnter()/render(): render()
// composes the progress frame, and the work runs on a later handleInput()
// tick once the flush worker has put that frame on glass. The book grid's
// metadata + cover thumbnails load the same way, one tile per quiet tick.

#include <cstdint>
#include <memory>
#include <string>

#include "../Scene.h"
#include "../reader/BookTextRenderer.h"
#include "../reader/Bookmarks.h"
#include "../reader/ReaderSettings.h"

namespace reader {
class Epub;
class Section;
class TextMeasure;
class FbpBook;
}  // namespace reader

class ReaderScene : public Scene {
 public:
  // Out-of-line (defaulted in the .cpp): the unique_ptr members hold types
  // that are only forward-declared here, and both the constructor and the
  // destructor of the enclosing class must see them complete.
  ReaderScene();
  ~ReaderScene();

  void onEnter() override;
  void onExit() override;
  // Bench dev console ("where" v2): sub-state + selection one-liner —
  // dead-reckoning from "scene=reader" alone opened a real book once.
  void debugWhere(char* out, size_t n) const;
  // Bench: print the live shelf order (index, meta state, name) so order
  // instability is measurable instead of anecdotal (audit 2026-08-18, I2).
  void debugShelfDump() const;
  const char* const* softKeys() const override;
  uint8_t longPressSlots() const override;
  void handleInput(Input& in) override;
  void render(Gfx& gfx) override;

 private:
  enum class State : uint8_t { Opening, Indexing, Reading, BookList, Error };
  // Deferred blocking work, run from handleInput() once the announcing frame
  // is on glass (armed by render(), gated on the flush worker being idle).
  // GridMeta (like PrefetchNext) is silent and yields to pending input.
  enum class Work : uint8_t { None, OpenBook, BuildSection, PrefetchNext, GridMeta };

  static constexpr uint16_t kLastPageSentinel = 0xFFFF;  // "prev chapter, last page"
  // In-RAM WINDOW of the shelf, not a library cap (0.6 workstream B): the
  // scan counts every book on the card and stores only the window around
  // the current scroll; crossing the window edge rescans. BSS stays fixed
  // no matter how many books the card holds.
  static constexpr int kMaxBooks = 32;
  // Per-tile lazy-load progress for the cover grid (workGridMeta). One work
  // unit takes a tile from Unknown to one of the three terminal states.
  enum class TileMeta : uint8_t {
    Unknown = 0,  // not probed yet — placeholder tile, grid work pending
    NotOpened,    // load+build failed — the epub is unreadable/corrupt
    NoCover,      // title/author loaded; no usable cover (text-only tile)
    Cover,        // title/author loaded; thumbPath/thumbW/thumbH valid
  };
  struct BookEntry {
    char path[160];           // full SD path incl. one /books subdir level; display name = basename(path)
    char title[64];           // from book.bin (basename fallback)
    char author[48];
    char thumbPath[64];       // cover_<w>x<h>.bin path (meta == Cover only)
    uint16_t thumbW, thumbH;  // actual thumb dims (aspect-fit, can undershoot)
    bool focusEdition;        // compiled with focus reading (bookc --focus)
    TileMeta meta;
    uint32_t lastReadDay;     // yyyymmdd from ReadingStats, 0 = never read
    uint16_t readMinutes;     // total minutes in this book (sort tiebreak)
    uint8_t progressPct;      // from the .pos sidecar, 0 = none/unknown
  };

  // Deferred work.
  void runWork();
  void workOpenBook();
  void workBuildSection();
  void workPrefetchNext();
  void workGridMeta();

  // Engine plumbing.
  void ensureSectionOrIndex();  // load section cache or schedule Indexing
  void applyPendingPage();
  void loadProgress();
  void saveProgress();
  void maybeArmPrefetch();
  void failWith(const char* msg);

  // Book list (2x2 cover grid; windowed over an unbounded library).
  void scanBooks(int windowOffset = 0);
  void scanDir(const char* dir, int depth);
  bool inWindow(int absIdx) const { return absIdx >= _windowOffset && absIdx < _windowOffset + _bookCount; }
  BookEntry& entryAt(int absIdx) { return _books[absIdx - _windowOffset]; }
  void enterBookList();
  void openSelectedBook();
  void moveSelection(int delta);
  XpRect listRect() const;
  // True when the open book has genuine chapters (fbp always; epub only when
  // it declared a TOC). Drives "Chapters" vs "Sections" wording — see #42.
  bool hasRealToc() const;
  XpRect tileRect(int visibleIndex) const;

  // Input/render helpers.
  void pageTurn(bool forward);
  void cycleFontSize();
  // 0.7 reader chrome v2: full-page MENU (the book's home) with two
  // strip sub-views that keep the page visible for live preview.
  // setFontSize is the retargetable core of cycleFontSize; sizeStep is
  // the strip's live UP/DOWN; noteTurnPace feeds time-left estimates.
  void setFontSize(int fontId);
  void sizeStep(int dir);
  void noteTurnPace();
  void handleMenuInput(Input& in);
  void menuSelect();
  void chapterCountAndSel(int* count, int* selOut);
  void chapterJump(int idx);
  float bookProgress();
  int estimateMinutesLeft(int pagesLeft) const;
  void renderMenuPage(Gfx& gfx);
  void renderMenuBody(Gfx& gfx, int y);
  int renderFocusChip(Gfx& gfx, int y);
  void renderSizeStrip(Gfx& gfx);
  void renderChapters(Gfx& gfx);
  void renderGoToStrip(Gfx& gfx);
  void renderBookmarks(Gfx& gfx);
  void renderStatsBook(Gfx& gfx);
  void renderStatsLife(Gfx& gfx);
  // Bookmarks: current place -> a mark; jumping restores it.
  bool applyOrientation(Gfx& gfx, bool toLandscape, bool persist);
  // The direction pair (soft-key slots 2 and 3). See Scene.h for the rule and
  // ReaderScene.cpp for why the text-size strip is the one exception.
  bool dirSwap() const;
  bool backKey(Input& in) const;
  bool fwdKey(Input& in) const;
  void toggleBookmarkHere();
  bool bookmarkHereIndex(int* idxOut);
  void loadBookmarks();
  void jumpToBookmark(int idx);
  void renderBody(Gfx& gfx);
  void renderReading(Gfx& gfx);
  void renderStatusLine(Gfx& gfx);
  void renderBookList(Gfx& gfx);
  void renderTile(Gfx& gfx, int visibleIndex);
  void renderMessage(Gfx& gfx, const char* line1, const char* line2);
  void renderCoverageNotice(Gfx& gfx);

  State _state = State::BookList;
  Work _work = Work::None;
  bool _workArmed = false;  // render() composed the frame announcing _work

  // Engine objects — created on open, freed in onExit (CrossPoint activity
  // model: reopening from the book.bin cache is fast).
  std::shared_ptr<reader::Epub> _epub;
  std::unique_ptr<reader::Section> _section;
  std::unique_ptr<reader::TextMeasure> _measure;
  // FBP mode: a compiled package is open instead of an Epub. The phone did
  // all layout; this path is page reads + blits only.
  std::unique_ptr<reader::FbpBook> _fbp;
  uint16_t _fbpPage = 0;
  uint16_t _lastPageCount = 0;  // last known pagination, for the place we hand the phone at close
  bool _fbpPosPending = false;  // .pos not read yet: needs the profile's page count
  void workOpenFbp();
  void fbpTurn(bool forward);
  void renderFbp(Gfx& gfx);
  reader::BookTextRenderer _renderer;
  reader::ReaderSettings _settings;
  std::string _bookPath;

  // True once BLE is suspended and the 32 KB dict + cover scratch are claimed
  // for book work (open / tile builds). The cached grid runs radio-up; this
  // flips at the first real work and onExit resumes the radio.
  bool _radioSuspended = false;
  // Framebuffer pointer captured in render() for the runWork() dict loan
  // (the static BSS framebuffer doubles as the 32 KB inflate window while
  // blocking work runs — flush is idle and render() repaints after).
  uint8_t* _fbForLoan = nullptr;
  void suspendRadioForBookWork();

  int _spine = 0;
  uint16_t _nextPage = 0;  // page to apply when the section (re)loads
  // Font-size cycle position restore: ratio -> page needs the NEW pageCount,
  // which only exists after the section (re)loads (applyPendingPage).
  float _pendingRatio = 0.0f;
  bool _hasPendingRatio = false;
  bool _hadProgress = false;
  int _prefetchAttemptedSpine = -1;  // never retry a failed/checked prefetch
  int _pageLoadRetries = 0;
  const char* _errorMsg = "";
  // Glyph-coverage notice (flowe-os#3 UX): when the first indexed chapter of
  // a raw epub is largely outside the built-in fonts (Hebrew, Arabic, CJK),
  // one full-screen notice points the user at the app's compile path instead
  // of pages of replacement boxes. Any button reads anyway.
  bool _coverageChecked = false;
  bool _coverageNotice = false;

  // 0.7 reader chrome v2: the MENU is a full page (CONFIRM opens while
  // reading); Text size and Go-to-page are strips OVER the book page so
  // the result previews live; Chapters is its own full page.
  enum class MenuView : uint8_t { None, Page, SizeStrip, Chapters, GoTo, Bookmarks,
                                 StatsBook, StatsLife };
  MenuView _menu = MenuView::None;
  int _menuSel = 0;    // cursor on the menu page (persists while book open)
  // Orientation row, pressed on a book with no landscape pages: the value
  // column swaps to name the fix. Cleared whenever the menu closes or the
  // cursor moves, so it never becomes a permanent label.
  bool _orientNote = false;
  int _tocSel = 0;     // cursor in the Chapters list
  int _tocScroll = 0;  // first visible Chapters row
  // Chapters auto-repeat (see handleInput): which direction is held, when the
  // next repeat is due, and whether a held scroll moved without painting yet.
  Btn _tocRepeatFrom = Btn::COUNT;
  uint32_t _tocRepeatNextMs = 0;
  int _gotoPage = 0;   // 0-based target while the Go-to strip is open
  // Bookmarks for the open book (loaded on open, kept in sync on edit).
  reader::Bookmarks::Mark _marks[reader::Bookmarks::kMax];
  int _markCount = 0;
  int _markSel = 0;
  int _markScroll = 0;
  // Shelf: the stats band is selectable (_sel == -1, the Notifications
  // header-pill idiom) and opens READING LIFE — your reading belongs to
  // the shelf, not to whichever book happens to be open.
  bool _lifeOpen = false;
  // Landscape reading (FBP only — the package must carry the profiles).
  bool _landscape = false;
  // The reader's GLOBAL orientation preference, mirrored from NVS "rdLand".
  // Distinct from _landscape, which is what the panel is doing right now:
  // a portrait-only book reads portrait without disturbing the preference.
  bool _wantLandscape = false;
  // Set in render() (where the panel size is known) and read by the menu
  // input handlers, which have no Gfx of their own.
  bool _landscapeReady = false;
  // -1 = nothing pending, 0 = go portrait, 1 = go landscape. render() applies
  // it because that is where the Gfx is. Persist only when the READER asked
  // (the menu row) — never when we are merely restoring what was saved.
  int8_t _pendingOrient = -1;
  bool _pendingOrientPersist = false;
  bool _pendingRestorePortrait = false;
  // Session reading pace for "time left": EMA of ms between FORWARD page
  // turns (3 s – 3 min window so pauses and skimming don't poison it).
  // 0 = no data yet this session.
  uint32_t _lastTurnMs = 0;
  uint32_t _avgTurnMs = 0;

  BookEntry _books[kMaxBooks];  // ~9KB BSS (scene is a static instance)
  int _bookCount = 0;       // entries STORED in the window (<= kMaxBooks)
  int _sel = 0;             // absolute index into the whole library
  int _scroll = 0;          // absolute, row-aligned
  int _totalBooks = 0;      // every match on the card, stored or not
  int _windowOffset = 0;    // absolute index of _books[0]
  uint16_t _skippedNames = 0;  // files skipped: name/path too long (logged on serial)
  bool _sdOk = true;        // card mounted when the scan ran
  int16_t _wCache = 0;
  int16_t _hCache = 0;
};
