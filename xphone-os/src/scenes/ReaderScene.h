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
#include "../reader/ReaderSettings.h"

namespace reader {
class Epub;
class Section;
class TextMeasure;
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
  const char* const* softKeys() const override;
  void handleInput(Input& in) override;
  void render(Gfx& gfx) override;

 private:
  enum class State : uint8_t { Opening, Indexing, Reading, BookList, Error };
  // Deferred blocking work, run from handleInput() once the announcing frame
  // is on glass (armed by render(), gated on the flush worker being idle).
  // GridMeta (like PrefetchNext) is silent and yields to pending input.
  enum class Work : uint8_t { None, OpenBook, BuildSection, PrefetchNext, GridMeta };

  static constexpr uint16_t kLastPageSentinel = 0xFFFF;  // "prev chapter, last page"
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
    char path[96];            // full SD path; display name = basename(path)
    char title[64];           // from book.bin (basename fallback)
    char author[48];
    char thumbPath[64];       // cover_<w>x<h>.bin path (meta == Cover only)
    uint16_t thumbW, thumbH;  // actual thumb dims (aspect-fit, can undershoot)
    TileMeta meta;
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

  // Book list (2x2 cover grid).
  void scanBooks();
  void scanDir(const char* dir);
  void enterBookList();
  void openSelectedBook();
  void moveSelection(int delta);
  XpRect listRect() const;
  XpRect tileRect(int visibleIndex) const;

  // Input/render helpers.
  void pageTurn(bool forward);
  void cycleFontSize();
  void renderBody(Gfx& gfx);
  void renderReading(Gfx& gfx);
  void renderStatusLine(Gfx& gfx);
  void renderBookList(Gfx& gfx);
  void renderTile(Gfx& gfx, int visibleIndex);
  void renderMessage(Gfx& gfx, const char* line1, const char* line2);

  State _state = State::BookList;
  Work _work = Work::None;
  bool _workArmed = false;  // render() composed the frame announcing _work

  // Engine objects — created on open, freed in onExit (CrossPoint activity
  // model: reopening from the book.bin cache is fast).
  std::shared_ptr<reader::Epub> _epub;
  std::unique_ptr<reader::Section> _section;
  std::unique_ptr<reader::TextMeasure> _measure;
  reader::BookTextRenderer _renderer;
  reader::ReaderSettings _settings;
  std::string _bookPath;

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

  BookEntry _books[kMaxBooks];  // ~9KB BSS (scene is a static instance)
  int _bookCount = 0;
  int _sel = 0;
  int _scroll = 0;
  int16_t _wCache = 0;
  int16_t _hCache = 0;
};
