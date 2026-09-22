#include "FbpBook.h"

#include <Arduino.h>

#include <cstdlib>
#include <cctype>
#include <cstring>
#include <uzlib.h>
#include <esp_heap_caps.h>

#include "../Gfx.h"

// Sort record for the render passes: by key for meta order, by bits_off for
// the elevator bitmap reads.
struct FbpBook_PageGlyphSort {
  uint32_t key;
  uint32_t bits_off;
  uint32_t uniq_idx;
};

// v4 glyph x is a zigzag varint delta from the previous glyph on the line.
// These must match bookc/src/fbp.h byte for byte — a disagreement here is
// text that drifts sideways across the page, which is why the host tool
// fbp-diff renders both formats and compares every pixel.
static inline int32_t readVarint(const uint8_t** p, const uint8_t* end) {
  uint32_t u = 0;
  int shift = 0;
  while (*p < end) {
    const uint8_t b = *(*p)++;
    u |= (uint32_t)(b & 0x7F) << shift;
    if (!(b & 0x80)) break;
    shift += 7;
    if (shift > 28) break;
  }
  return (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
}

static inline void skipVarint(const uint8_t** p, const uint8_t* end) {
  while (*p < end && (*(*p)++ & 0x80)) {
  }
}

namespace reader {

// Bench: force the no-arena render path (devcon "arenaoff"), so the
// never-blank fallback is provable on glass instead of waited for.
bool gBenchNoArena = false;
}  // namespace reader
bool benchSetNoArena(bool on) { reader::gBenchNoArena = on; return on; }
namespace reader {

// A page that fails to render paints BLANK, and the caller's "composed"
// line prints regardless — so every exit names itself here, with the heap
// shape, or a field report of blank pages has nothing to go on.
static void pageFail(const char* what, uint16_t page, size_t need) {
  Serial.printf("[xphone-os] fbp: page %u NOT rendered: %s (need=%u heap=%u largest=%u)\n",
                static_cast<unsigned>(page + 1), what, static_cast<unsigned>(need), ESP.getFreeHeap(),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

bool FbpBook::readAt(uint64_t off, void* dst, size_t n) {
  if (off > _f.size() || n > _f.size() - off || !_f.seekSet(off)) return false;
  return _f.read(dst, n) == (int)n;
}

bool FbpBook::open(const char* path) {
  close();
  if (!SdMan.ready() && !SdMan.begin()) return false;
  _f = SdMan.open(path, O_RDONLY);
  if (!_f) return false;
  if (!readAt(0, &_hdr, sizeof(_hdr)) || memcmp(_hdr.magic, "FBPK", 4) != 0) {
    Serial.printf("[xphone-os] fbp: bad magic in %s\n", path);
    close();
    return false;
  }
  if (_hdr.fmt_ver < 3 || _hdr.fmt_ver > 9 || _hdr.min_reader > 6 ||
      (_hdr.fmt_ver == 9 && _hdr.min_reader != 6) ||
      !_hdr.profile_count || _hdr.profile_count > FBP_PROFILE_CAP) {
    Serial.printf("[xphone-os] fbp: format v%u (reader speaks v6) — recompile in the app\n",
                  _hdr.fmt_ver);
    close();
    return false;
  }
  uint64_t mo = _hdr.meta_off;
  char* dsts[2] = {_title, _author};
  size_t caps[2] = {sizeof(_title), sizeof(_author)};
  for (int i = 0; i < 2; i++) {
    uint16_t len = 0;
    if (!readAt(mo, &len, 2)) break;
    size_t take = len < caps[i] - 1 ? len : caps[i] - 1;
    if (take && _f.read(dsts[i], take) != (int)take) break;
    dsts[i][take] = 0;
    mo += 2 + len;
  }
  // v8: the footnote table follows the TOC entries. Older files put the
  // shelf there, so the version gate matters as much as the tag.
  _noteOff = 0;
  _noteCount = 0;
  if (_hdr.fmt_ver >= 8) {
    const uint64_t off = _hdr.toc_off + (uint64_t)_hdr.toc_count * 52;
    char tag[4];
    uint32_t n = 0;
    if (readAt(off, tag, 4) && memcmp(tag, "FBPN", 4) == 0 && readAt(off + 4, &n, 4)) {
      _noteOff = off + 8;
      _noteCount = n;
    }
  }
  _open = true;
  return true;
}

bool FbpBook::noteEntry(uint32_t i, uint32_t* from_cid, uint32_t* to_cid) {
  if (!_noteOff || i >= _noteCount) return false;
  uint32_t e[2];
  if (!readAt(_noteOff + (uint64_t)i * 8, e, 8)) return false;
  if (from_cid) *from_cid = e[0];
  if (to_cid) *to_cid = e[1];
  return true;
}

// Marks on `page`: notes whose from_cid lies in [first paragraph of this
// page, first paragraph of the next page). A paragraph that runs over the
// page break belongs to the page it started on. Binary search on from_cid;
// the table is sorted by the compiler.
uint32_t FbpBook::notesInCidRange(uint32_t lo, uint32_t hi, uint32_t* first) {
  if (first) *first = 0;
  if (!_noteOff || !_noteCount || hi < lo) return 0;
  // First note with from_cid >= lo.
  uint32_t a = 0, b = _noteCount;
  while (a < b) {
    const uint32_t mid = (a + b) / 2;
    uint32_t f = 0;
    if (!noteEntry(mid, &f, nullptr)) return 0;
    if (f < lo) a = mid + 1; else b = mid;
  }
  uint32_t n = 0;
  for (uint32_t i = a; i < _noteCount; i++) {
    uint32_t f = 0;
    if (!noteEntry(i, &f, nullptr) || f > hi) break;
    n++;
  }
  if (first) *first = a;
  return n;
}

// The page-anchor guess: notes of paragraphs whose id falls between this
// page's first paragraph and the next page's. A paragraph that runs over
// the break is counted where it ENDS (the next page's anchor is that
// paragraph). pageMarkCids is exact; this is the fallback for a page the
// reader has not drawn yet or a book without word boxes.
uint32_t FbpBook::notesOnPage(uint16_t page, uint32_t* first) {
  if (first) *first = 0;
  if (!_noteOff || !_noteCount || !_profSelected) return 0;
  const ProfileDir& d = _geo[_profIdx];
  uint32_t lo = 0, hi = 0xFFFFFFFFu;
  if (!pageFirstParaId(d, page, &lo)) return 0;
  if ((uint32_t)page + 1 < d.page_count && !pageFirstParaId(d, (uint16_t)(page + 1), &hi)) return 0;
  if (hi <= lo) hi = lo + 1;  // a page that starts inside the same paragraph as the next
  return notesInCidRange(lo, hi - 1, first);
}

uint16_t FbpBook::pageMarkCids(uint32_t* out, uint16_t cap) {
  if (!_wordTail || !out || !cap) return 0;
  const uint8_t* p = _wordTail;
  const uint8_t* end = _wordTailEnd;
  uint16_t n = 0;
  for (uint16_t l = 0; l < _wordTailLines && p < end; l++) {
    const int32_t count = readVarint(&p, end);
    for (int32_t w = 0; w < count && p + 2 <= end; w++) {
      readVarint(&p, end);  // x0 delta
      readVarint(&p, end);  // width
      if (p + 2 > end) return n;
      const uint8_t flags = *p++;
      const uint8_t tl = *p++;
      if (!_wordTextColumn) {
        if (p + tl > end) return n;
        p += tl;
      }
      if ((flags & 2) && l < _lineCidCount) {
        const uint32_t cid = _lineCids[l];
        bool seen = false;
        for (uint16_t i = 0; i < n; i++)
          if (out[i] == cid) { seen = true; break; }
        if (!seen && n < cap) out[n++] = cid;
      }
    }
  }
  return n;
}

// One profile directory, whatever the file's version. v3 entries are 32
// bytes and carry no dictionary; v4 entries are 48. Reading a v3 file with
// the v4 stride would walk off into the page records.
bool FbpBook::readProfile(uint32_t i, ProfileDir* out) {
  if (i >= _hdr.profile_count) return false;
  memset(out, 0, sizeof(*out));
  if (_hdr.fmt_ver >= 4) {
    if (!readAt(sizeof(Header) + (uint64_t)i * sizeof(ProfileDir), out, sizeof(ProfileDir))) return false;
    return _hdr.fmt_ver == 9 ? out->reserved[6] == FBP_COMPACT_CODEC : out->reserved[6] == 0;
  }
  ProfileDirV3 v3;
  if (!readAt(sizeof(Header) + (uint64_t)i * sizeof(ProfileDirV3), &v3, sizeof(v3))) return false;
  out->width = v3.width;
  out->height = v3.height;
  out->px_size = v3.px_size;
  out->page_count = v3.page_count;
  out->page_index_off = v3.page_index_off;
  out->atlas_off = v3.atlas_off;
  out->font_count = v3.font_count;
  memcpy(out->reserved, v3.reserved, 7);
  return true;
}

bool FbpBook::selectProfile(uint16_t w, uint16_t h, uint16_t prefer_px) {
  _nGeo = 0;
  _profSelected = false;
  _oom = false;
  for (uint32_t i = 0; i < _hdr.profile_count && _nGeo < kMaxSizes; i++) {
    ProfileDir d;
    if (!readProfile(i, &d)) return false;
    if (d.width == w && d.height == h) _geo[_nGeo++] = d;
  }
  if (_nGeo == 0) {
    // No geometry match (foreign package): take the first profile and scale
    // nothing — better a readable page than a refusal.
    if (!readProfile(0, &_geo[0])) return false;
    _nGeo = 1;
  }
  // Nearest px to the preference, not exact-match: the preference may come
  // from a book in another size family (apps compile 22/26/30, the Press
  // 14/18/22), and "closest to what you chose" beats silently ignoring it.
  int pick = _nGeo / 2;
  if (prefer_px) {
    int best = 1 << 20;
    for (int i = 0; i < _nGeo; i++) {
      const int d = _geo[i].px_size > prefer_px ? _geo[i].px_size - prefer_px
                                                : prefer_px - _geo[i].px_size;
      if (d < best) {
        best = d;
        pick = i;
      }
    }
  }
  // Claim the largest matching profile while the opening heap is still clean.
  // Later size steps reload bytes into these same allocations. If reservation
  // fails, applyProfile can still open a smaller profile and retain its capacity.
  uint32_t pageCap = 0, predictorCap = 0;
  for (int i = 0; i < _nGeo; ++i) {
    uint32_t page, predictors;
    if (!profileCapacity(_geo[i], &page, &predictors)) continue;
    if (page > pageCap) pageCap = page;
    if (predictors > predictorCap) predictorCap = predictors;
  }
  ensureCapacity(pageCap, predictorCap);
  // A profile whose page buffer cannot allocate right now must not refuse
  // the whole book: try the others before giving up.
  bool applied = applyProfile(pick);
  for (int i = 0; !applied && i < _nGeo; i++) {
    if (i != pick) applied = applyProfile(i);
  }
  if (!applied) return false;
  _profSelected = true;
  Serial.printf("[xphone-os] fbp: %d sizes @%ux%u, using %upx pages=%u fonts=%u\n", _nGeo, w, h,
                pxSize(), pageCount(), _nfonts);
  return true;
}

bool FbpBook::profileCapacity(const ProfileDir& d, uint32_t* page, uint32_t* predictors) {
  *page = *predictors = 0;
  if (_hdr.fmt_ver >= 4) {
    if (!d.max_raw_page || d.max_raw_page > kMaxRawPage || d.dict_size > kDictCap) return false;
    *page = d.dict_size + d.max_raw_page + 1;
  }
  if (d.reserved[6] == FBP_COMPACT_CODEC) {
    uint8_t h[8]; uint64_t first = 0;
    const uint64_t at = d.dict_off + d.dict_size;
    if (at < d.dict_off || !readAt(at, h, 8) || memcmp(h, "PRED", 4) ||
        !readAt(d.page_index_off, &first, 8)) return false;
    *predictors = fc_u32(h + 4);
    if (*predictors > FBP_PREDICTOR_CAP || first != at + 8 + (uint64_t)*predictors * 8) return false;
  }
  return true;
}

bool FbpBook::ensureCapacity(uint32_t page, uint32_t predictors) {
  // Growth is atomic: a failed reservation leaves the old allocations intact.
  // Neither allocation grows beyond the validated format caps.
  if (page > kDictCap + kMaxRawPage + 1 || predictors > FBP_PREDICTOR_CAP) return false;
  uint8_t* nextPage = nullptr;
  FcPredictor* nextPredictors = nullptr;
  if (page > _pageCap) {
    nextPage = (uint8_t*)malloc(page);
    if (!nextPage) { _oom = true; return false; }
  }
  if (predictors > _predictorCap) {
    nextPredictors = (FcPredictor*)malloc(predictors * sizeof(FcPredictor));
    if (!nextPredictors) { free(nextPage); _oom = true; return false; }
  }
  if (nextPage) {
    free(_page); _page = nextPage; _pageCap = page;
    _wordTail = _wordTextColumn = nullptr;
  }
  if (nextPredictors) {
    free(_predictors); _predictors = nextPredictors; _predictorCap = predictors;
  }
  return true;
}

bool FbpBook::applyProfile(int idx) {
  _profSelected = false;
  _wordTail = _wordTextColumn = nullptr;
  _lineCidCount = 0;
  const ProfileDir& d = _geo[idx];
  uint32_t pageCap, predictorCount;
  if (!profileCapacity(d, &pageCap, &predictorCount)) return false;
  if (!ensureCapacity(pageCap, predictorCount)) {
    Serial.printf("[xphone-os] fbp: no room for profile buffers (page=%lu predictors=%lu heap=%u)\n",
                  (unsigned long)pageCap, (unsigned long)(predictorCount * sizeof(FcPredictor)), ESP.getFreeHeap());
    return false;
  }
  if (!d.page_count || d.page_index_off > _f.size() ||
      (uint64_t)d.page_count * 8 > _f.size() - d.page_index_off ||
      d.atlas_off != d.page_index_off + (uint64_t)d.page_count * 8) return false;
  uint64_t off = d.atlas_off;
  uint8_t fc = 0;
  if (!readAt(off, &fc, 1)) return false;
  off += 1;
  if (fc > kMaxFonts) return false;
  _nfonts = fc;
  for (uint8_t fi = 0; fi < _nfonts; fi++) {
    uint32_t cnt = 0, blen = 0;
    if (!readAt(off, &cnt, 4) || !readAt(off + 4, &blen, 4)) return false;
    if (cnt > 65536 || off + 8 + (uint64_t)cnt * sizeof(GlyphMeta) + blen > _f.size()) return false;
    _fontCount[fi] = cnt;
    _fontBlobSize[fi] = blen;
    _fontMetaOff[fi] = off + 8;
    _fontBlobOff[fi] = off + 8 + (uint64_t)cnt * sizeof(GlyphMeta);
    off = _fontBlobOff[fi] + blen;
  }

  // Reload this profile's dictionary/table without releasing reserved capacity.
  // A caller that rolls back after a read failure must render the old page again.
  _predictorCount = 0;
  _dictLen = 0;
  if (d.dict_size && !readAt(d.dict_off, _page, d.dict_size)) return false;
  _dictLen = d.dict_size;
  if (predictorCount &&
      (!readAt(d.dict_off + d.dict_size + 8, _predictors, predictorCount * sizeof(FcPredictor)) ||
       !fc_table_valid(_predictors, predictorCount))) return false;
  _predictorCount = predictorCount;
  _profIdx = idx;
  _profSelected = true;
  _oom = false;
  return true;
}

// Inflate one v4 page body to _page + _dictLen.
//
// uzlib is given NO ring buffer. Instead its output base (dest_start) is
// the dictionary's first byte and output begins after the dictionary, so
// a back-reference of distance N reads N bytes back — into the page's own
// output, or past it into the dictionary. That is exactly what a preset
// dictionary means, and it needs no second buffer and no copy per page.
bool FbpBook::inflatePage(uint64_t rec_off, uint32_t clen, uint32_t raw_len) {
  if (!_page || raw_len == 0 || raw_len > _geo[_profIdx].max_raw_page ||
      _dictLen + raw_len + 1 > _pageCap) return false;
  if (clen == 0 || clen > kMaxRecordSize) return false;
  uint8_t* comp = (uint8_t*)malloc(clen);
  if (!comp) return false;
  if (!readAt(rec_off, comp, clen)) {
    free(comp);
    return false;
  }
  uzlib_uncomp u;
  memset(&u, 0, sizeof(u));
  uzlib_uncompress_init(&u, nullptr, 0);
  u.source = comp;
  u.source_limit = comp + clen;
  u.source_read_cb = nullptr;
  u.dest_start = _page;
  u.dest = _page + _dictLen;
  u.dest_limit = u.dest + raw_len + 1;
  const int r = uzlib_uncompress(&u);
  const bool full = (u.dest == _page + _dictLen + raw_len) && u.source == u.source_limit;
  free(comp);
  if (r != TINF_DONE || !full) {
    Serial.printf("[xphone-os] fbp: inflate failed r=%d got=%u want=%lu\n", r,
                  (unsigned)(u.dest - (_page + _dictLen)), (unsigned long)raw_len);
    return false;
  }
  return true;
}

bool FbpBook::hasGeometry(const uint16_t w, const uint16_t h) {
  for (uint32_t i = 0; i < _hdr.profile_count; i++) {
    ProfileDir d;
    if (!readProfile(i, &d)) return false;
    if (d.width == w && d.height == h) return true;
  }
  return false;
}

bool FbpBook::selectProfileFresh(const uint16_t w, const uint16_t h) {
  const uint16_t want = _profSelected ? pxSize() : 0;
  const bool hadProfile = _profSelected;
  const int savedGeo = _nGeo, savedIdx = _profIdx;
  ProfileDir saved[kMaxSizes];
  memcpy(saved, _geo, sizeof(saved));
  if (!hasGeometry(w, h)) return false;
  if (selectProfile(w, h, want)) return true;
  memcpy(_geo, saved, sizeof(saved));  // restore on failure
  _nGeo = savedGeo;
  _profIdx = savedIdx;
  _profSelected = hadProfile && applyProfile(savedIdx);
  return false;
}

bool FbpBook::pageFirstCidPublic(const uint16_t page, uint32_t* cid) {
  if (!_profSelected) return false;
  return pageFirstParaId(_geo[_profIdx], page, cid);
}

bool FbpBook::pageFirstSidPublic(const uint16_t page, uint32_t* sid) {
  if (!_profSelected) return false;
  return pageFirstSentId(_geo[_profIdx], page, sid);
}

// Reader menu "Chapters": one TOC record. The title field is fixed-width
// and NOT NUL-terminated at max length — copy defensively.
bool FbpBook::tocEntry(uint32_t i, char* title, size_t title_cap, uint32_t* content_id) {
  if (i >= _hdr.toc_count || title_cap == 0) return false;
  struct __attribute__((packed)) {
    uint32_t cid;
    char title[48];
  } e;
  if (!readAt(_hdr.toc_off + (uint64_t)i * sizeof(e), &e, sizeof(e))) return false;
  if (content_id) *content_id = e.cid;
  size_t take = title_cap - 1 < sizeof(e.title) ? title_cap - 1 : sizeof(e.title);
  memcpy(title, e.title, take);
  title[take] = 0;
  return true;
}

// Last page whose anchor of the given kind is <= id (both counters are
// monotonic across pages by construction).
uint16_t FbpBook::pageForAnchor(const uint8_t off, const uint32_t id) {
  const ProfileDir& d = _geo[_profIdx];
  uint32_t lo = 0, hi = d.page_count ? d.page_count - 1 : 0, best = 0;
  while (lo <= hi) {
    uint32_t mid = (lo + hi) / 2;
    uint32_t mc = 0;
    if (!pageAnchor(d, (uint16_t)mid, off, &mc)) break;
    if (mc <= id) {
      best = mid;
      if (mid == hi) break;
      lo = mid + 1;
    } else {
      if (mid == 0) break;
      hi = mid - 1;
    }
  }
  return (uint16_t)best;
}

// TOC jumps, bookmarks and goto speak PARAGRAPH content ids (rec+0).
uint16_t FbpBook::pageForContentId(uint32_t cid) { return pageForAnchor(0, cid); }
// Size/orientation carries speak the finer SENTENCE ids (rec+4).
uint16_t FbpBook::pageForSentenceId(uint32_t sid) { return pageForAnchor(4, sid); }

// The record header's two anchors (identical layout in v3 and v4):
// rec+0 = first PARAGRAPH content id, rec+4 = first SENTENCE id. Sentence
// ids grow faster. The old single accessor read rec+4 while several
// callers passed paragraph ids — the lands-early chapter-jump bug.
bool FbpBook::pageAnchor(const ProfileDir& d, uint16_t page, uint8_t off, uint32_t* out) {
  uint64_t rec = 0;
  if (!readAt(d.page_index_off + (uint64_t)page * 8, &rec, 8)) return false;
  return readAt(rec + off, out, 4);
}
bool FbpBook::pageFirstParaId(const ProfileDir& d, uint16_t page, uint32_t* cid) {
  return pageAnchor(d, page, 0, cid);
}
bool FbpBook::pageFirstSentId(const ProfileDir& d, uint16_t page, uint32_t* sid) {
  return pageAnchor(d, page, 4, sid);
}

// Pages inside a long paragraph all share its content ID, so the run of
// same-ID pages measures how deep into the paragraph a page is; expanding
// a run costs a handful of 12-byte reads (runs are short).
bool FbpBook::stepSize(int dir, uint16_t cur_page, uint16_t* new_page) {
  if (!_profSelected || _nGeo <= 1) return false;
  const int target = (_profIdx + (dir > 0 ? 1 : _nGeo - 1)) % _nGeo;
  uint32_t cid = 0;
  if (!pageFirstSentId(_geo[_profIdx], cur_page, &cid)) return false;

  // Depth within the current paragraph: page k of n sharing this ID.
  uint32_t old_start = cur_page, old_len = 1;
  {
    const ProfileDir& od = _geo[_profIdx];
    uint32_t c;
    while (old_start > 0 && pageFirstSentId(od, (uint16_t)(old_start - 1), &c) && c == cid) old_start--;
    uint32_t e = cur_page;
    while (e + 1 < od.page_count && pageFirstSentId(od, (uint16_t)(e + 1), &c) && c == cid) e++;
    old_len = e - old_start + 1;
  }
  const uint32_t k = cur_page - old_start;

  const int previous = _profIdx;
  if (!applyProfile(target)) {
    _profSelected = applyProfile(previous);
    if (!_profSelected) Serial.printf("%s", "[xphone-os] fbp: size rollback failed\n");
    return false;
  }

  // Last page whose first content ID is <= cid (monotonic by construction).
  const ProfileDir& d = _geo[_profIdx];
  uint32_t lo = 0, hi = d.page_count ? d.page_count - 1 : 0, best = 0;
  while (lo <= hi) {
    uint32_t mid = (lo + hi) / 2;
    uint32_t mc = 0;
    if (!pageFirstSentId(d, (uint16_t)mid, &mc)) break;
    if (mc <= cid) {
      best = mid;
      if (mid == hi) break;
      lo = mid + 1;
    } else {
      if (mid == 0) break;
      hi = mid - 1;
    }
  }

  // Proportional landing inside the paragraph's run at the new size — the
  // long-paragraph (classical Arabic) case where paragraph-start landing
  // felt like losing the place.
  uint32_t c;
  if (old_len > 1 && pageFirstSentId(d, (uint16_t)best, &c) && c == cid) {
    uint32_t new_start = best, e2 = best;
    while (new_start > 0 && pageFirstSentId(d, (uint16_t)(new_start - 1), &c) && c == cid) new_start--;
    while (e2 + 1 < d.page_count && pageFirstSentId(d, (uint16_t)(e2 + 1), &c) && c == cid) e2++;
    uint32_t new_len = e2 - new_start + 1;
    uint32_t depth = (k * new_len + old_len / 2) / old_len;
    if (depth >= new_len) depth = new_len - 1;
    best = new_start + depth;
  }
  *new_page = (uint16_t)best;
  return true;
}

void FbpBook::drawGlyph(Gfx& gfx, const GlyphMeta& m, const uint8_t* bits, int x, int baseline) {
  int rowbytes = (m.w + 7) / 8;
  int x0 = x + m.bearing_x, y0 = baseline - m.bearing_y;
  for (int r = 0; r < m.h; r++) {
    const uint8_t* row = bits + r * rowbytes;
    for (int c = 0; c < m.w; c++)
      if (row[c >> 3] & (0x80 >> (c & 7))) gfx.drawPixel(x0 + c, y0 + r, true);
  }
}

void FbpBook::drawImage(Gfx& gfx, uint32_t idx, int x, int y, uint16_t w, uint16_t h) {
  if (idx >= _hdr.image_count) return;
  ImageDirEnt ent;
  if (!readAt(_hdr.images_dir_off + (uint64_t)idx * sizeof(ImageDirEnt), &ent, sizeof(ent))) return;
  uint32_t rowbytes = ((uint32_t)ent.w + 7) / 8;
  uint8_t rowbuf[128];
  if (rowbytes > sizeof(rowbuf)) return;
  if (!_f.seekSet(ent.off)) return;
  for (uint16_t r = 0; r < ent.h && r < h; r++) {
    if (_f.read(rowbuf, rowbytes) != (int)rowbytes) return;
    for (uint16_t c = 0; c < ent.w && c < w; c++)
      if (rowbuf[c >> 3] & (0x80 >> (c & 7))) gfx.drawPixel(x + c, y + r, true);
  }
}

// Sort helper: PageGlyph by (font, bits_off) ascending.
static int cmpGlyphOffset(const void* a, const void* b) {
  const FbpBook_PageGlyphSort* ga = (const FbpBook_PageGlyphSort*)a;
  const FbpBook_PageGlyphSort* gb = (const FbpBook_PageGlyphSort*)b;
  if ((ga->key >> 16) != (gb->key >> 16)) return (int)(ga->key >> 16) - (int)(gb->key >> 16);
  if (ga->bits_off < gb->bits_off) return -1;
  return ga->bits_off > gb->bits_off ? 1 : 0;
}

#if defined(FLOWE_BENCH_COMPACT)
bool FbpBook::benchProfile(uint32_t profile, uint16_t* width, uint16_t* height) {
  if (!_open || !readProfile(profile, &_geo[0])) return false;
  _nGeo = 1; _profSelected = applyProfile(0);
  *width = _geo[0].width; *height = _geo[0].height;
  return _profSelected;
}
bool FbpBook::benchBodyCrc(uint16_t page, uint32_t* crc) {
  if (!_profSelected || page >= pageCount() || _hdr.fmt_ver < 8) return false;
  const ProfileDir& d = _geo[_profIdx];
  uint64_t at, end = d.page_index_off; uint8_t head[12];
  if (!readAt(d.page_index_off + (uint64_t)page * 8, &at, 8) ||
      (page + 1 < pageCount() && !readAt(d.page_index_off + (uint64_t)(page + 1) * 8, &end, 8)) ||
      end < at + 12 || end - at > kMaxRecordSize || !readAt(at, head, 12) ||
      !inflatePage(at + 12, (uint32_t)(end - at - 12), fc_u32(head + 8))) return false;
  uint32_t h = fc_crc_bytes(UINT32_MAX, head, 8);
  if (d.reserved[6] == FBP_COMPACT_CODEC) {
    FcPage v;
    if (!fc_page(&v, _page + _dictLen, fc_u32(head + 8), _predictors, _predictorCount) ||
        !fc_body_crc(&v, _predictors, _predictorCount, &h)) return false;
  } else h = fc_crc_bytes(h, _page + _dictLen, fc_u32(head + 8));
  *crc = ~h; return true;
}
#endif

bool FbpBook::renderPage(Gfx& gfx, uint16_t page) {
  if (!_open || !_profSelected || page >= pageCount()) return false;
  const ProfileDir& d = _geo[_profIdx];
  uint64_t rec_off = 0, next_off = 0;
  if (!readAt(d.page_index_off + (uint64_t)page * 8, &rec_off, 8)) return false;
  if (page + 1 < d.page_count) {
    if (!readAt(d.page_index_off + (uint64_t)(page + 1) * 8, &next_off, 8)) return false;
  } else {
    next_off = d.page_index_off;  // records precede the index
  }
  _wordTail = _wordTextColumn = nullptr;
  _lineCidCount = 0;
  if (next_off < rec_off || next_off - rec_off > kMaxRecordSize ||
      next_off > d.page_index_off || (_hdr.fmt_ver >= 4 && rec_off < d.dict_off + d.dict_size)) return false;
  uint32_t rec_size = (uint32_t)(next_off - rec_off);
  if (rec_size < 12 || rec_size > kMaxRecordSize) return false;

  // Both formats end up as `body` .. `body_end`, so the two walks below
  // differ only in how a glyph's x is stored.
  const bool v4 = _hdr.fmt_ver >= 4;
  uint8_t* owned = nullptr;  // v3 only: the record buffer this call must free
  const uint8_t* body = nullptr;
  const uint8_t* body_end = nullptr;
  if (v4) {
    // u32 first_cid, u32 first_sid, u32 raw_len, then the deflate stream.
    uint32_t raw_len = 0;
    if (!readAt(rec_off + 8, &raw_len, 4)) { pageFail("record header read", page, 4); return false; }
    if (!inflatePage(rec_off + 12, rec_size - 12, raw_len)) { pageFail("inflate", page, raw_len); return false; }
    body = _page + _dictLen;
    body_end = body + raw_len;
  } else {
    // ONE read for the whole page record.
    owned = (uint8_t*)malloc(rec_size);
    if (!owned) { pageFail("record buffer", page, rec_size); return false; }
    if (!readAt(rec_off, owned, rec_size)) {
      free(owned);
      pageFail("record read", page, rec_size);
      return false;
    }
    body = owned + 8;  // past first_cid + first_sid
    body_end = owned + rec_size;
  }
  const bool compact = d.reserved[6] == FBP_COMPACT_CODEC;
  FcPage columns;
  FcGlyphCursor cursor;
  if (body_end - body < 4) { free(owned); return false; }
  if (compact) {
    if (!fc_page(&columns, body, (size_t)(body_end - body), _predictors, _predictorCount)) return false;
    for (uint32_t g = 0; g < columns.glyphs; g++) {
      const uint8_t f = columns.fonts[g];
      const uint16_t ix = (uint16_t)(columns.low[g] | ((uint16_t)columns.high[g] << 8));
      if (f >= _nfonts || ix >= _fontCount[f]) return false;
    }
    for (uint16_t im = 0; im < columns.nimages; im++) {
      const uint32_t ix = fc_u32(columns.tail + (uint32_t)im * 12);
      ImageDirEnt image;
      if (ix >= _hdr.image_count || !readAt(_hdr.images_dir_off + (uint64_t)ix * sizeof(image), &image, sizeof(image)) ||
          image.off > _f.size() || image.size > _f.size() - image.off ||
          (uint64_t)((image.w + 7) / 8) * image.h > image.size) return false;
    }
    body += 6;
  }
  uint16_t nlines, nimgs;
  memcpy(&nlines, body, 2);
  memcpy(&nimgs, body + 2, 2);
  body += 4;

  // Pass 1: unique glyphs on this page. Two scans so the PageGlyph table is
  // sized to THIS page, not to the 768-glyph worst case: a Latin page keeps
  // ~60 glyphs, and the old fixed 15.4 KB table was the single cost that
  // pushed reading past the free heap with the radio up
  // (docs/plans/2026-08-17-bluetooth-while-reading.html).
  //
  // Scan A: a transient key-set (8 KB) counts the uniques. It is freed
  // before the arena exists, so it never adds to the render peak.
  // 1024 slots, not 2048: the key-set was the single biggest contiguous
  // block a page needed (8 KB) and it decided blank pages on a fragmented
  // reading heap (field report 2026-09-02: largest free block 9.2 KB with
  // page peaks of 9.1 KB). 1024 still holds the 768-glyph worst case.
  const uint32_t kHash = 1024;
  // STATIC, not heap: these two tables were the fixed contiguous blocks a
  // page could fail to get on a fragmented reading heap (blank pages,
  // 2026-09-02). 6 KB of BSS that exists from boot cannot be refused and
  // cannot be fragmented. The page-sized tables (uniq/order/arena) stay on
  // the heap: they are small for Latin pages and the arena has a fallback.
  static uint32_t sKeyset[1024];
  static uint16_t sHmap[1024];
  uint32_t* keyset = sKeyset;
  memset(keyset, 0xFF, kHash * sizeof(uint32_t));  // key high bits <= 5: 0xFFFFFFFF is free
  uint32_t nuniq = 0;
  const uint8_t* p = body;
  const uint8_t* rec_end = compact ? columns.tail_end : body_end;
  uint32_t columnGlyph = 0;
  for (uint16_t l = 0; l < nlines && p + 4 <= rec_end; l++) {
    uint16_t count;
    memcpy(&count, p + 2, 2);
    p += 4;
    for (uint16_t g = 0; g < count && (compact || p + 3 <= rec_end); g++) {
      uint32_t key; uint16_t idx;
      if (compact) {
        key = (uint32_t)(columns.fonts[columnGlyph] + 1) << 16;
        idx = (uint16_t)(columns.low[columnGlyph] | ((uint16_t)columns.high[columnGlyph] << 8));
        columnGlyph++;
      } else {
        key = (uint32_t)(p[0] + 1) << 16;
        memcpy(&idx, p + 1, 2); p += 3;
        if (v4) skipVarint(&p, rec_end);
        else { if (p + 2 > rec_end) break; p += 2; }
      }
      key |= idx;
      uint32_t h = (key * 2654435761u) & (kHash - 1);
      while (keyset[h] != 0xFFFFFFFFu && keyset[h] != key) h = (h + 1) & (kHash - 1);
      if (keyset[h] == 0xFFFFFFFFu && nuniq < kMaxPageGlyphs) {
        keyset[h] = key;
        nuniq++;
      }
    }
  }

  // Right-sized tables: exactly this page's uniques, plus the 4 KB index map
  // the draw pass probes.
  PageGlyph* uniq = (PageGlyph*)malloc((nuniq ? nuniq : 1) * sizeof(PageGlyph));
  uint16_t* hmap = sHmap;
  if (!uniq) {
    free(owned);
    pageFail("glyph table", page, nuniq * sizeof(PageGlyph));
    return false;
  }
  memset(hmap, 0xFF, kHash * sizeof(uint16_t));
  uint32_t filled = 0;
  for (uint32_t s = 0; s < kHash && filled < nuniq; s++) {
    if (keyset[s] == 0xFFFFFFFFu) continue;
    const uint32_t key = keyset[s];
    uniq[filled].key = key;
    uint32_t h = (key * 2654435761u) & (kHash - 1);
    while (hmap[h] != 0xFFFF) h = (h + 1) & (kHash - 1);
    hmap[h] = (uint16_t)filled++;
  }

  // Pass 2: metas in ascending index order per font (metas are contiguous —
  // ascending reads share sectors), recording each glyph's bits size.
  // uniq[] is appended in page order; sort a light index by key (font,idx).
  FbpBook_PageGlyphSort* order =
      (FbpBook_PageGlyphSort*)malloc((nuniq ? nuniq : 1) * sizeof(FbpBook_PageGlyphSort));
  if (!order) {
    free(uniq);
    free(owned);
    return false;
  }
  for (uint32_t i = 0; i < nuniq; i++) order[i] = (FbpBook_PageGlyphSort){uniq[i].key, uniq[i].key, i};
  qsort(order, nuniq, sizeof(order[0]), cmpGlyphOffset);  // key==bits_off proxy: (font,idx) order
  uint32_t arena_need = 0;
  for (uint32_t o = 0; o < nuniq; o++) {
    PageGlyph* g = &uniq[order[o].uniq_idx];
    uint8_t font = (uint8_t)((g->key >> 16) - 1);
    uint16_t idx = (uint16_t)(g->key & 0xFFFF);
    if (font >= _nfonts || idx >= _fontCount[font]) {
      g->meta.w = g->meta.h = 0;
      continue;
    }
    if (!readAt(_fontMetaOff[font] + (uint64_t)idx * sizeof(GlyphMeta), &g->meta, sizeof(GlyphMeta))) {
      g->meta.w = g->meta.h = 0;
      continue;
    }
    uint32_t rowbytes = ((uint32_t)g->meta.w + 7) / 8;
    uint32_t need = rowbytes * g->meta.h;
    if (g->meta.bits_off > _fontBlobSize[font] || need > _fontBlobSize[font] - g->meta.bits_off) {
      free(order); free(uniq); free(owned); return false;
    }
    if (need > kMaxGlyphBits) {
      g->meta.h = (uint16_t)(kMaxGlyphBits / (rowbytes ? rowbytes : 1));
      need = rowbytes * g->meta.h;
    }
    if (arena_need + need > kArenaCap) {
      g->meta.w = g->meta.h = 0;  // arena overflow: drop glyph (logged below)
      continue;
    }
    g->arena_off = arena_need;
    arena_need += need;
  }

  // P6 instrumentation (docs/plans/2026-08-17-buttons-and-orientation.md):
  // what a page render actually costs, so the question "can the radio stay up
  // while reading" is answered with numbers instead of a guess. PEAK is every
  // buffer alive at once — the record, the glyph hash, the fixed uniq table,
  // the sort index and the arena.
  // Two candidate peaks now: scan A (record + the 8 KB key-set) and the
  // render proper (record + index map + right-sized uniq + sort + arena).
  // HEAP peak only: the key-set and index map are static now (2026-09-02)
  // and no longer count — the number must say what a page can fail to get.
  const uint32_t resident = residentBytes();
  const uint32_t scanPeak = resident + rec_size;
  const uint32_t renderPeak = resident + (owned ? rec_size : 0) + nuniq * (uint32_t)sizeof(PageGlyph) +
                              nuniq * (uint32_t)sizeof(FbpBook_PageGlyphSort) + arena_need;
  const uint32_t peak = renderPeak > scanPeak ? renderPeak : scanPeak;
  _lastPeak = peak;
  _lastUniq = nuniq;

  // Pass 3: bitmap bits in ascending disk order (elevator), one arena.
  // The arena is an OPTIMIZATION, not a requirement. When the reading heap
  // is too fragmented to hold it, the page is drawn anyway: pass 4 reads
  // each glyph's bits from the card on demand into a static buffer. Slower
  // (one read per glyph occurrence, ~200 ms a page) — but a slow page beats
  // a blank one that stays blank (field report 2026-09-02).
  uint8_t* arena = gBenchNoArena ? nullptr : (uint8_t*)malloc(arena_need ? arena_need : 1);
  if (!arena) {
    Serial.printf("[xphone-os] fbp: page %u degraded: no %lu byte arena (largest=%u); glyphs read per use\n",
                  static_cast<unsigned>(page + 1), static_cast<unsigned long>(arena_need),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  } else {
    for (uint32_t i = 0; i < nuniq; i++)
      order[i] = (FbpBook_PageGlyphSort){uniq[i].key, uniq[i].meta.bits_off, i};
    qsort(order, nuniq, sizeof(order[0]), cmpGlyphOffset);
    for (uint32_t o = 0; o < nuniq; o++) {
      PageGlyph* g = &uniq[order[o].uniq_idx];
      if (!g->meta.w || !g->meta.h) continue;
      uint8_t font = (uint8_t)((g->key >> 16) - 1);
      uint32_t rowbytes = ((uint32_t)g->meta.w + 7) / 8;
      uint32_t need = rowbytes * g->meta.h;
      if (!readAt(_fontBlobOff[font] + g->meta.bits_off, arena + g->arena_off, need))
        g->meta.w = g->meta.h = 0;
    }
  }
  static uint8_t sGlyphScratch[kMaxGlyphBits];  // the no-arena path's one glyph

  // Pass 4: draw everything from RAM.
  p = body;
  if (compact) fc_cursor(&cursor, &columns);
  for (uint16_t l = 0; l < nlines && p + 4 <= rec_end; l++) {
    int16_t baseline;
    uint16_t count;
    memcpy(&baseline, p, 2);
    memcpy(&count, p + 2, 2);
    p += 4;
    if (l < kMaxPageLines) _lineBase[l] = baseline;  // R1: retained for the highlight box
    int32_t x = 0;
    if (compact) fc_line(&cursor);
    for (uint16_t g = 0; g < count && (compact || p + 3 <= rec_end); g++) {
      uint32_t key; uint16_t idx;
      if (compact) {
        uint8_t font;
        // fc_page validated this exact cursor before any draw.
        if (!fc_glyph(&cursor, _predictors, _predictorCount, &font, &idx, &x)) break;
        key = (uint32_t)(font + 1) << 16;
      } else {
        key = (uint32_t)(p[0] + 1) << 16;
        memcpy(&idx, p + 1, 2); p += 3;
        if (v4) { int32_t dx = readVarint(&p, rec_end); if (!fc_add(x, dx, &x)) break; }
        else { if (p + 2 > rec_end) break; int16_t ax; memcpy(&ax, p, 2); x = ax; p += 2; }
      }
      key |= idx;
      uint32_t h = (key * 2654435761u) & (kHash - 1);
      while (hmap[h] != 0xFFFF && uniq[hmap[h]].key != key) h = (h + 1) & (kHash - 1);
      if (hmap[h] == 0xFFFF) continue;
      PageGlyph* pg = &uniq[hmap[h]];
      if (pg->meta.w && pg->meta.h) {
        if (arena) {
          drawGlyph(gfx, pg->meta, arena + pg->arena_off, (int)x, baseline);
        } else {
          const uint8_t font = (uint8_t)((pg->key >> 16) - 1);
          const uint32_t need = (((uint32_t)pg->meta.w + 7) / 8) * pg->meta.h;
          if (readAt(_fontBlobOff[font] + pg->meta.bits_off, sGlyphScratch, need))
            drawGlyph(gfx, pg->meta, sGlyphScratch, (int)x, baseline);
        }
      }
    }
  }
  if (compact) p = columns.tail;
  for (uint16_t im = 0; im < nimgs && p + 12 <= rec_end; im++, p += 12) {
    uint32_t idx;
    int16_t x, y;
    uint16_t w, h;
    memcpy(&idx, p, 4);
    memcpy(&x, p + 4, 2);
    memcpy(&y, p + 6, 2);
    memcpy(&w, p + 8, 2);
    memcpy(&h, p + 10, 2);
    drawImage(gfx, idx, x, y, w, h);
  }

  // v7 tail: one zigzag varint per line — the delta of that line's
  // paragraph content id from the previous line's, seeded by the page's
  // first_cid. This is what lets a highlight cursor know which paragraph
  // each LINE belongs to. Absent (pre-v7 book) => count 0 and the R1 UI
  // says "sync this book again".
  _lineCidCount = 0;
  _wordTail = nullptr;
  if (_hdr.fmt_ver >= 7) {
    uint32_t firstCid = 0;
    if (pageFirstParaId(_geo[_profIdx], page, &firstCid)) {
      uint32_t cid = firstCid;
      uint16_t l = 0;
      for (; l < nlines && p < rec_end && _lineCidCount < kMaxPageLines; l++) {
        cid += (uint32_t)readVarint(&p, rec_end);
        _lineCids[_lineCidCount++] = cid;
      }
      // v8 word tail follows, and only lives on while the body sits in
      // the page buffer (v4+ books; a v3 record buffer is freed below).
      if (_hdr.fmt_ver >= 8 && l == nlines && !owned && p < rec_end) {
        _wordTail = p;
        _wordTextColumn = compact ? columns.text : nullptr;
        _wordTailEnd = rec_end;
        _wordTailLines = nlines;
        _wordTailPage = page;
      }
    }
  }

  free(arena);
  free(order);
  free(uniq);
  free(owned);
  return true;
}

uint16_t FbpBook::pageWords(WordBox* out, uint16_t cap) {
  if (!_wordTail || !out || !cap) return 0;
  const uint8_t* p = _wordTail;
  const uint8_t* end = _wordTailEnd;
  const uint8_t* body = _page + _dictLen;
  const uint8_t* text = _wordTextColumn;
  uint16_t n = 0;
  for (uint16_t l = 0; l < _wordTailLines && p < end; l++) {
    const int32_t count = readVarint(&p, end);
    int32_t prev = 0;
    for (int32_t w = 0; w < count && p + 2 <= end; w++) {
      const int32_t x0 = prev + readVarint(&p, end);
      const int32_t width = readVarint(&p, end);
      if (p + 2 > end) return n;
      const uint8_t flags = *p++;
      const uint8_t tl = *p++;
      if (!text && p + tl > end) return n;
      if (n < cap && l < kMaxPageLines) {
        out[n].x = (int16_t)x0;
        out[n].w = (int16_t)width;
        out[n].line = (uint8_t)l;
        out[n].flags = flags;
        out[n].textOff = (uint16_t)((text ? text : p) - body);
        out[n].textLen = tl;
        n++;
      }
      if (text) text += tl; else p += tl;
      prev = x0 + width;
    }
  }
  return n;
}

void FbpBook::wordText(const WordBox& w, char* dst, size_t cap) const {
  if (!dst || !cap) return;
  dst[0] = 0;
  if (!_wordTail || !_page) return;
  if ((uint32_t)w.textOff + w.textLen > _pageCap - _dictLen) return;
  const uint8_t* src = _page + _dictLen + w.textOff;
  size_t take = w.textLen < cap - 1 ? w.textLen : cap - 1;
  memcpy(dst, src, take);
  dst[take] = 0;
}

bool FbpBook::readMeta(const char* path, char* title, size_t title_cap, char* author,
                       size_t author_cap, bool* focus_edition) {
  FbpBook b;
  if (!b.open(path)) return false;
  snprintf(title, title_cap, "%s", b._title);
  snprintf(author, author_cap, "%s", b._author);
  if (focus_edition) {
    // reserved[0] bit0, written by bookc for --focus packages. Read through
    // readProfile: reserved sits at a different offset in v3 and v4.
    ProfileDir d;
    *focus_edition = b.readProfile(0, &d) && (d.reserved[0] & 1);
  }
  return true;
}

// The smallest contiguous block any profile in this package needs to open:
// min over profiles of (dict_size + max_raw_page + 1), plus the compact
// codec's predictor table (predictor_count * sizeof(FcPredictor), the count
// read from the PRED block at dict_off + dict_size). Reuses profileCapacity
// so the number is exactly what ensureCapacity will later try to malloc — if
// even the lightest profile cannot fit the heap's largest block, the reader
// knows the book cannot open at all and can hand the heap back to it (the
// radio yields) before workOpenFbp() reserves anything.
//
// 0 = cannot decide (pre-v4 format, unreadable header, or no valid profile).
// Callers treat 0 as "no decision": today's behaviour stays untouched.
uint32_t FbpBook::minPageBufferBytes(const char* path) {
  FbpBook b;
  if (!b.open(path) || b._hdr.fmt_ver < 4) return 0;
  uint32_t min = 0;
  for (uint32_t i = 0; i < b._hdr.profile_count; i++) {
    ProfileDir d;
    uint32_t page = 0, predictors = 0;
    if (!b.readProfile(i, &d) || !b.profileCapacity(d, &page, &predictors)) continue;
    const uint32_t need = page + predictors * static_cast<uint32_t>(sizeof(FcPredictor));
    if (min == 0 || need < min) min = need;
  }
  return min;
}

// Write one XT-format bin (CoverThumb.h) from 1-bpp packed bits.
static bool writeXtBin(const char* path, uint16_t w, uint16_t h, FsFile& src, uint32_t size) {
  FsFile out = SdMan.open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!out) return false;
  uint8_t hdr[8] = {0x54, 0x58, 1, 0, (uint8_t)(w & 0xFF), (uint8_t)(w >> 8), (uint8_t)(h & 0xFF),
                    (uint8_t)(h >> 8)};
  bool ok = out.write(hdr, 8) == 8;
  uint8_t buf[256];
  uint32_t left = size;
  while (ok && left) {
    uint32_t take = left < sizeof(buf) ? left : sizeof(buf);
    ok = src.read(buf, take) == (int)take && out.write(buf, take) == (int)take;
    left -= take;
  }
  out.close();
  return ok;
}

bool FbpBook::ensureShelfSidecars(const char* path, bool* has_cover, bool* has_strip) {
  char cov[192], str[192];
  snprintf(cov, sizeof(cov), "%s.cov", path);
  snprintf(str, sizeof(str), "%s.str", path);
  *has_cover = SdMan.exists(cov);
  *has_strip = SdMan.exists(str);
  // BOTH, not either: a book that ever got one sidecar but not the other
  // (interrupted transfer, full card) used to be stuck that way forever.
  if (*has_cover && *has_strip) return true;  // extracted on a previous scan

  FbpBook b;
  if (!b.open(path) || !b._hdr.shelf_off) return false;
  uint8_t shdr[16];
  if (!b.readAt(b._hdr.shelf_off, shdr, 16)) return false;
  uint16_t tw, th, sw, sh;
  uint32_t ts, ss;
  memcpy(&tw, shdr, 2);
  memcpy(&th, shdr + 2, 2);
  memcpy(&ts, shdr + 4, 4);
  memcpy(&sw, shdr + 8, 2);
  memcpy(&sh, shdr + 10, 2);
  memcpy(&ss, shdr + 12, 4);
  uint64_t bits = b._hdr.shelf_off + 16;
  if (ts && !*has_cover) {
    b._f.seekSet(bits);
    *has_cover = writeXtBin(cov, tw, th, b._f, ts);
  }
  if (ss && !*has_strip) {
    b._f.seekSet(bits + ts);
    *has_strip = writeXtBin(str, sw, sh, b._f, ss);
  }
  return *has_cover || *has_strip;
}

void FbpBook::canonicalKey(const char* name, char* out, const size_t outSize) {
  size_t len = std::strlen(name);
  static const char* const kExt[] = {".fbp", ".epub", ".txt"};
  for (size_t e = 0; e < sizeof(kExt) / sizeof(kExt[0]); ++e) {
    const size_t el = std::strlen(kExt[e]);
    if (len >= el && strcasecmp(name + len - el, kExt[e]) == 0) {
      len -= el;
      break;
    }
  }
  size_t w = 0;
  for (size_t i = 0; i < len && w + 1 < outSize; ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (c < 128 && isalnum(c)) out[w++] = static_cast<char>(tolower(c));
  }
  out[w] = '\0';
}

bool FbpBook::findByKey(const char* key, char* outPath, const size_t outCap) {
  if (!key || key[0] == '\0') return false;
  FsFile dir = SdMan.open("/books", O_RDONLY);
  if (!dir || !dir.isDir()) return false;
  FsFile f;
  char name[96];
  bool found = false;
  while (!found && f.openNext(&dir, O_RDONLY)) {
    const size_t got = f.getName(name, sizeof(name));
    f.close();
    if (got == 0 || name[0] == '.') continue;
    const size_t nl = std::strlen(name);
    const bool isFbp = nl >= 4 && strcasecmp(name + nl - 4, ".fbp") == 0;
    if (!isFbp) continue;  // a place resumes a compiled book, not a raw source
    char k[64];
    canonicalKey(name, k, sizeof(k));
    if (std::strcmp(k, key) == 0) {
      const int n = snprintf(outPath, outCap, "/books/%s", name);
      found = n > 0 && n < static_cast<int>(outCap);
    }
  }
  dir.close();
  return found;
}

uint16_t FbpBook::loadPos(const char* path, uint16_t pageCount) {
  char side[192];
  snprintf(side, sizeof(side), "%s.pos", path);
  FsFile f = SdMan.open(side, O_RDONLY);
  if (!f) return 0;
  uint32_t page = 0, count = 0;
  f.read(&page, 4);
  const bool hasCount = f.read(&count, 4) == 4;
  f.close();
  // A .pos written by another build (a phone push, or an older profile) is
  // numbered in THAT pagination. The trailer says which one; rescale into
  // ours or the reader resumes on the wrong page. Caught live 2026-08-18:
  // {92, 1070} opened as raw page 92 of 1137.
  if (hasCount && count > 0 && pageCount > 0 && count != pageCount)
    page = (uint32_t)(((uint64_t)page * pageCount + count / 2) / count);
  if (pageCount > 0 && page >= pageCount) page = pageCount - 1;
  return (uint16_t)page;
}

void FbpBook::savePos(const char* path, uint16_t page) {
  savePos(path, page, 0);
}

void FbpBook::savePos(const char* path, uint16_t page, uint16_t pageCount) {
  char side[192];
  snprintf(side, sizeof(side), "%s.pos", path);
  FsFile f = SdMan.open(side, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return;
  uint32_t p32 = page;
  f.write(&p32, 4);
  // Convergence trailer (2026-08-18): the page count of the pagination this
  // page lives in, so a phone can turn the position into a fraction of the
  // book and compare it ACROSS devices and builds. Older firmware reads only
  // the first 4 bytes and is unaffected.
  if (pageCount) {
    uint32_t c32 = pageCount;
    f.write(&c32, 4);
  }
  f.close();
}

void FbpBook::close() {
  if (_f) _f.close();
  _open = _profSelected = false;
  _nfonts = 0;
  _nGeo = 0;
  free(_predictors); _predictors = nullptr; _predictorCount = _predictorCap = 0;
  _wordTextColumn = nullptr;
  free(_page);  // the dictionary + page buffer lives only while a book is open
  _page = nullptr;
  _pageCap = _dictLen = 0;
  _noteOff = 0;
  _noteCount = 0;
  _wordTail = nullptr;
  _lineCidCount = 0;
}

}  // namespace reader
