#pragma once

// FBPK v2 package reader — the device half of the bookc compiler (bookc/src/
// fbp.h is the format's source of truth; struct layouts must match).
//
// Everything is a range read. Resident: header, the geometry's profile list
// (up to 4 sizes), per-font atlas offsets, and a heap glyph cache that lives
// only while the book is open. A page render is one index read, one record
// read, cache-missed glyph reads, blits. SIZE switches profiles in place and
// keeps the position exactly via the page records' first content IDs.

#include <SDCardManager.h>

#include <cstdint>

class Gfx;

namespace reader {

class FbpBook {
 public:
  ~FbpBook() { close(); }

  bool open(const char* path);
  void close();
  bool isOpen() const { return _open; }

  // Pick the profile set for this viewport; prefer_px 0 = middle size.
  bool selectProfile(uint16_t w, uint16_t h, uint16_t prefer_px);
  bool profileSelected() const { return _profSelected; }

  uint16_t pageCount() const { return _geo[_profIdx].page_count; }
  uint16_t pxSize() const { return _geo[_profIdx].px_size; }
  // How many sizes this geometry offers. 1 usually means a foreign package
  // (built for another panel) opened through the fallback in selectProfile.
  int sizeCount() const { return _nGeo; }
  // Focus edition (bookc --focus): word-prefix runs are pre-bolded into the
  // page records. ProfileDir.reserved[0] bit0, already resident once a
  // profile is selected. The shelf badges these; the reader needs to say so
  // too, or two editions of the same book are indistinguishable once open.
  bool isFocusEdition() const { return _profSelected && (_geo[_profIdx].reserved[0] & 1); }
  const char* title() const { return _title; }
  const char* author() const { return _author; }

  bool renderPage(Gfx& gfx, uint16_t page);

  // Switch to the next size in this geometry. Returns the page in the new
  // profile that holds the same first paragraph (content ID) the current
  // page showed — the position survives the re-pagination exactly.
  bool cycleSize(uint16_t cur_page, uint16_t* new_page);

  // Reader menu "Orientation": does this package carry profiles for the
  // given viewport? Landscape editions (bookc --landscape) add them; a
  // package without them simply has no landscape mode.
  bool hasGeometry(uint16_t w, uint16_t h);
  // Re-pick the profile set for a NEW viewport (orientation change), keeping
  // the closest pixel size to the one in use.
  bool selectProfileFresh(uint16_t w, uint16_t h);
  // The page's paragraph anchor, for carrying a position across a
  // re-pagination (orientation or size change).
  bool pageFirstCidPublic(uint16_t page, uint32_t* cid);

  // Reader menu "Chapters": the package's table of contents (compiler-
  // written FbpTocEnt records: paragraph content_id + title[48]).
  uint32_t tocCount() const { return _hdr.toc_count; }
  bool tocEntry(uint32_t i, char* title, size_t title_cap, uint32_t* content_id);
  // Last page (current profile) whose first paragraph ID <= cid — the
  // same monotonic search cycleSize uses, exposed for TOC jumps.
  uint16_t pageForContentId(uint32_t cid);

  // Shelf metadata without holding the file open.
  static bool readMeta(const char* path, char* title, size_t title_cap, char* author,
                       size_t author_cap, bool* focus_edition = nullptr);

  // Ensure the phone-prepared shelf sidecars exist next to the package
  // ("<path>.cov" / "<path>.str", CoverThumb XT bin format). Cheap when
  // already extracted. Outputs say which assets exist.
  static bool ensureShelfSidecars(const char* path, bool* has_cover, bool* has_strip);

  // What the LAST renderPage cost: every buffer alive at once (page record,
  // glyph hash, uniq table, sort index, glyph arena) and the unique-glyph
  // count. Read by the reader's page log — the numbers that answer whether
  // the radio could stay up while reading
  // (docs/plans/2026-08-17-buttons-and-orientation.md P6).
  uint32_t lastPeakBytes() const { return _lastPeak; }
  uint32_t lastUniqGlyphs() const { return _lastUniq; }

  // Progress sidecar ("<path>.pos", 4 bytes LE page index).
  static uint16_t loadPos(const char* path, uint16_t pageCount);
  static void savePos(const char* path, uint16_t page);
  static void savePos(const char* path, uint16_t page, uint16_t pageCount);

  // Canonical book key: drop a container extension, keep ASCII alphanumerics,
  // lowercase. The ONE identity two phones agree on even when they spell a
  // file differently. Mirrors ReaderLibraryManager.libraryKey on the apps and
  // library_key() in tools/convergence-check.py. (Item 6 / delete-siblings.)
  static void canonicalKey(const char* name, char* out, size_t outSize);
  // Find the /books file whose canonical key matches. Writes the full path
  // into out (needs ~160 bytes). Returns false if no book matches.
  static bool findByKey(const char* key, char* outPath, size_t outCap);

 private:
#pragma pack(push, 1)
  struct Header {
    char magic[4];
    uint16_t fmt_ver, min_reader;
    uint8_t book_hash[16];
    uint32_t profile_count, image_count, idmap_count, toc_count;
    uint64_t images_dir_off, idmap_off, toc_off, meta_off, shelf_off;
  };
  // v4 (2026-08-20) added three fields, so the directory grew from 32
  // bytes to 48. Both layouts are read: v3 packages already on cards must
  // keep working after a flash, or every book on the device dies at once.
  struct ProfileDir {
    uint16_t width, height, px_size, page_count;
    uint64_t page_index_off, atlas_off;
    uint64_t dict_off;
    uint32_t dict_size;
    uint32_t max_raw_page;
    uint8_t font_count;
    uint8_t reserved[7];
  };
  struct ProfileDirV3 {
    uint16_t width, height, px_size, page_count;
    uint64_t page_index_off, atlas_off;
    uint8_t font_count;
    uint8_t reserved[7];
  };
  struct GlyphMeta {
    uint16_t w, h;
    int16_t bearing_x, bearing_y;
    uint32_t bits_off;
  };
  struct ImageDirEnt {
    uint64_t off;
    uint32_t size;
    uint16_t w, h;
  };
#pragma pack(pop)

  // 8 since v5 (2026-08-20) gave books their emphasis back. The slots are
  // base, CJK, Arabic, Hebrew, bold, italic, bold-italic, spare. Older
  // packages carry 4 (or 5 for focus editions) and still open: the atlas
  // walk reads font_count from the file either way. Three extra slots cost
  // 60 bytes of BSS across the two offset arrays and the count array.
  static constexpr int kMaxFonts = 8;
  static constexpr int kMaxSizes = 4;
  // Per-page working set (see renderPage): a dense CJK page uses ~450 unique
  // glyphs — far past any small LRU — so each render loads exactly the
  // page's glyphs with disk-order (elevator) reads and frees them after.
  static constexpr int kMaxPageGlyphs = 768;
  static constexpr uint32_t kMaxGlyphBits = 512;   // per-glyph clip cap
  static constexpr uint32_t kMaxRecordSize = 32768;
  static constexpr uint32_t kArenaCap = 80 * 1024;
  // v4 sanity caps. A body is smaller than the v3 record it replaces, so
  // the same 32 KB ceiling holds; the dictionary is 32 KB by definition.
  // These exist so a corrupt directory cannot ask for a huge malloc.
  static constexpr uint32_t kMaxRawPage = 32768;
  static constexpr uint32_t kDictCap = 32768;

  struct PageGlyph {
    uint32_t key;       // (font+1)<<16 | atlas_idx
    GlyphMeta meta;
    uint32_t arena_off; // bits location in the render arena
  };

  bool readAt(uint64_t off, void* dst, size_t n);
  bool readProfile(uint32_t i, ProfileDir* out);  // v3 stride is 32, v4 is 48
  bool applyProfile(int idx);  // parse this profile's atlas offsets
  bool pageFirstCid(const ProfileDir& d, uint16_t page, uint32_t* cid);
  // v4: the profile's dictionary followed by room for one inflated page.
  // The dictionary sits at the FRONT and inflate writes after it, with
  // uzlib's output base pointed at the dictionary's first byte — so a
  // back-reference simply reaches backwards into it and the dictionary is
  // never overwritten. One allocation per open book, no per-page malloc.
  uint8_t* _page = nullptr;
  uint32_t _pageCap = 0;   // dict_size + max_raw_page
  uint32_t _dictLen = 0;
  bool inflatePage(uint64_t rec_off, uint32_t clen, uint32_t raw_len);
  uint32_t _lastPeak = 0;
  uint32_t _lastUniq = 0;
  void drawGlyph(Gfx& gfx, const GlyphMeta& m, const uint8_t* bits, int x, int baseline);
  void drawImage(Gfx& gfx, uint32_t idx, int x, int y, uint16_t w, uint16_t h);

  FsFile _f;
  bool _open = false, _profSelected = false;
  Header _hdr;
  ProfileDir _geo[kMaxSizes];  // this geometry's profiles, ascending px
  int _nGeo = 0, _profIdx = 0;
  uint64_t _fontMetaOff[kMaxFonts] = {0};
  uint64_t _fontBlobOff[kMaxFonts] = {0};
  uint32_t _fontCount[kMaxFonts] = {0};
  uint8_t _nfonts = 0;
  char _title[64] = {0};
  char _author[48] = {0};
};

}  // namespace reader
