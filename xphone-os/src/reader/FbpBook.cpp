#include "FbpBook.h"

#include <Arduino.h>

#include <cstdlib>
#include <cstring>
#include <uzlib.h>

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

bool FbpBook::readAt(uint64_t off, void* dst, size_t n) {
  if (!_f.seekSet(off)) return false;
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
  if (_hdr.fmt_ver < 3 || _hdr.min_reader > 5) {
    Serial.printf("[xphone-os] fbp: format v%u (reader speaks v5) — recompile in the app\n",
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
  _open = true;
  return true;
}

// One profile directory, whatever the file's version. v3 entries are 32
// bytes and carry no dictionary; v4 entries are 48. Reading a v3 file with
// the v4 stride would walk off into the page records.
bool FbpBook::readProfile(uint32_t i, ProfileDir* out) {
  memset(out, 0, sizeof(*out));
  if (_hdr.fmt_ver >= 4)
    return readAt(sizeof(Header) + (uint64_t)i * sizeof(ProfileDir), out, sizeof(ProfileDir));
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
  int pick = _nGeo / 2;
  if (prefer_px)
    for (int i = 0; i < _nGeo; i++)
      if (_geo[i].px_size == prefer_px) pick = i;
  if (!applyProfile(pick)) return false;
  _profSelected = true;
  Serial.printf("[xphone-os] fbp: %d sizes @%ux%u, using %upx pages=%u fonts=%u\n", _nGeo, w, h,
                pxSize(), pageCount(), _nfonts);
  return true;
}

bool FbpBook::applyProfile(int idx) {
  const ProfileDir& d = _geo[idx];
  uint64_t off = d.atlas_off;
  uint8_t fc = 0;
  if (!readAt(off, &fc, 1)) return false;
  off += 1;
  _nfonts = fc < kMaxFonts ? fc : kMaxFonts;
  for (uint8_t fi = 0; fi < _nfonts; fi++) {
    uint32_t cnt = 0, blen = 0;
    if (!readAt(off, &cnt, 4) || !readAt(off + 4, &blen, 4)) return false;
    _fontCount[fi] = cnt;
    _fontMetaOff[fi] = off + 8;
    _fontBlobOff[fi] = off + 8 + (uint64_t)cnt * sizeof(GlyphMeta);
    off = _fontBlobOff[fi] + blen;
  }

  // v4: claim the page buffer for THIS profile and load its dictionary.
  // cycleSize calls back in here to change size, so the old buffer goes
  // first — the two profiles have different dictionaries.
  free(_page);
  _page = nullptr;
  _pageCap = 0;
  _dictLen = 0;
  if (_hdr.fmt_ver >= 4) {
    if (d.max_raw_page == 0 || d.max_raw_page > kMaxRawPage || d.dict_size > kDictCap) {
      Serial.printf("[xphone-os] fbp: profile %d has a bad page buffer (raw=%lu dict=%lu)\n", idx,
                    (unsigned long)d.max_raw_page, (unsigned long)d.dict_size);
      return false;
    }
    const uint32_t cap = d.dict_size + d.max_raw_page;
    _page = (uint8_t*)malloc(cap);
    if (!_page) {
      Serial.printf("[xphone-os] fbp: no room for a %lu byte page buffer (heap=%u)\n",
                    (unsigned long)cap, ESP.getFreeHeap());
      return false;
    }
    if (d.dict_size && !readAt(d.dict_off, _page, d.dict_size)) {
      free(_page);
      _page = nullptr;
      return false;
    }
    _pageCap = cap;
    _dictLen = d.dict_size;
  }
  _profIdx = idx;
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
  if (!_page || raw_len == 0 || _dictLen + raw_len > _pageCap) return false;
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
  u.dest_limit = u.dest + raw_len;
  const int r = uzlib_uncompress(&u);
  const bool full = (u.dest == u.dest_limit);
  free(comp);
  if ((r != TINF_OK && r != TINF_DONE) || !full) {
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
  const int savedGeo = _nGeo, savedIdx = _profIdx;
  ProfileDir saved[kMaxSizes];
  memcpy(saved, _geo, sizeof(saved));
  if (!hasGeometry(w, h)) return false;
  if (selectProfile(w, h, want)) return true;
  memcpy(_geo, saved, sizeof(saved));  // restore on failure
  _nGeo = savedGeo;
  _profIdx = savedIdx;
  return false;
}

bool FbpBook::pageFirstCidPublic(const uint16_t page, uint32_t* cid) {
  if (!_profSelected) return false;
  return pageFirstCid(_geo[_profIdx], page, cid);
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

// Last page whose first paragraph ID <= cid (IDs are monotonic across
// pages by construction). Same search cycleSize does, on the CURRENT
// profile — TOC jumps land on the chapter's opening page.
uint16_t FbpBook::pageForContentId(uint32_t cid) {
  const ProfileDir& d = _geo[_profIdx];
  uint32_t lo = 0, hi = d.page_count ? d.page_count - 1 : 0, best = 0;
  while (lo <= hi) {
    uint32_t mid = (lo + hi) / 2;
    uint32_t mc = 0;
    if (!pageFirstCid(d, (uint16_t)mid, &mc)) break;
    if (mc <= cid) {
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

// v3: the fine anchor is the page's first SENTENCE ID (second u32 of the
// record header). SIZE search on it lands near line-exact.
bool FbpBook::pageFirstCid(const ProfileDir& d, uint16_t page, uint32_t* cid) {
  uint64_t rec = 0;
  if (!readAt(d.page_index_off + (uint64_t)page * 8, &rec, 8)) return false;
  return readAt(rec + 4, cid, 4);
}

// Pages inside a long paragraph all share its content ID, so the run of
// same-ID pages measures how deep into the paragraph a page is; expanding
// a run costs a handful of 12-byte reads (runs are short).
bool FbpBook::cycleSize(uint16_t cur_page, uint16_t* new_page) {
  if (_nGeo <= 1) return false;
  uint32_t cid = 0;
  if (!pageFirstCid(_geo[_profIdx], cur_page, &cid)) return false;

  // Depth within the current paragraph: page k of n sharing this ID.
  uint32_t old_start = cur_page, old_len = 1;
  {
    const ProfileDir& od = _geo[_profIdx];
    uint32_t c;
    while (old_start > 0 && pageFirstCid(od, (uint16_t)(old_start - 1), &c) && c == cid) old_start--;
    uint32_t e = cur_page;
    while (e + 1 < od.page_count && pageFirstCid(od, (uint16_t)(e + 1), &c) && c == cid) e++;
    old_len = e - old_start + 1;
  }
  const uint32_t k = cur_page - old_start;

  if (!applyProfile((_profIdx + 1) % _nGeo)) return false;

  // Last page whose first content ID is <= cid (monotonic by construction).
  const ProfileDir& d = _geo[_profIdx];
  uint32_t lo = 0, hi = d.page_count ? d.page_count - 1 : 0, best = 0;
  while (lo <= hi) {
    uint32_t mid = (lo + hi) / 2;
    uint32_t mc = 0;
    if (!pageFirstCid(d, (uint16_t)mid, &mc)) break;
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
  if (old_len > 1 && pageFirstCid(d, (uint16_t)best, &c) && c == cid) {
    uint32_t new_start = best, e2 = best;
    while (new_start > 0 && pageFirstCid(d, (uint16_t)(new_start - 1), &c) && c == cid) new_start--;
    while (e2 + 1 < d.page_count && pageFirstCid(d, (uint16_t)(e2 + 1), &c) && c == cid) e2++;
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
    if (!readAt(rec_off + 8, &raw_len, 4)) return false;
    if (!inflatePage(rec_off + 12, rec_size - 12, raw_len)) return false;
    body = _page + _dictLen;
    body_end = body + raw_len;
  } else {
    // ONE read for the whole page record.
    owned = (uint8_t*)malloc(rec_size);
    if (!owned) return false;
    if (!readAt(rec_off, owned, rec_size)) {
      free(owned);
      return false;
    }
    body = owned + 8;  // past first_cid + first_sid
    body_end = owned + rec_size;
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
  const uint32_t kHash = 2048;
  uint32_t* keyset = (uint32_t*)malloc(kHash * sizeof(uint32_t));
  if (!keyset) {
    free(owned);
    return false;
  }
  memset(keyset, 0xFF, kHash * sizeof(uint32_t));  // key high bits <= 5: 0xFFFFFFFF is free
  uint32_t nuniq = 0;
  const uint8_t* p = body;
  const uint8_t* rec_end = body_end;
  for (uint16_t l = 0; l < nlines && p + 4 <= rec_end; l++) {
    uint16_t count;
    memcpy(&count, p + 2, 2);
    p += 4;
    for (uint16_t g = 0; g < count && p + 3 <= rec_end; g++) {
      uint32_t key = ((uint32_t)(p[0] + 1) << 16);
      uint16_t idx;
      memcpy(&idx, p + 1, 2);
      key |= idx;
      p += 3;
      if (v4) {
        skipVarint(&p, rec_end);  // x delta: this scan only needs the glyph id
      } else {
        if (p + 2 > rec_end) break;
        p += 2;
      }
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
  uint16_t* hmap = (uint16_t*)malloc(kHash * sizeof(uint16_t));
  if (!uniq || !hmap) {
    free(uniq);
    free(hmap);
    free(keyset);
    free(owned);
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
  free(keyset);

  // Pass 2: metas in ascending index order per font (metas are contiguous —
  // ascending reads share sectors), recording each glyph's bits size.
  // uniq[] is appended in page order; sort a light index by key (font,idx).
  FbpBook_PageGlyphSort* order =
      (FbpBook_PageGlyphSort*)malloc(nuniq * sizeof(FbpBook_PageGlyphSort));
  if (!order) {
    free(hmap);
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
  const uint32_t scanPeak = rec_size + kHash * (uint32_t)sizeof(uint32_t);
  const uint32_t renderPeak = rec_size + kHash * (uint32_t)sizeof(uint16_t) +
                              nuniq * (uint32_t)sizeof(PageGlyph) +
                              nuniq * (uint32_t)sizeof(FbpBook_PageGlyphSort) + arena_need;
  const uint32_t peak = renderPeak > scanPeak ? renderPeak : scanPeak;
  _lastPeak = peak;
  _lastUniq = nuniq;

  // Pass 3: bitmap bits in ascending disk order (elevator), one arena.
  uint8_t* arena = (uint8_t*)malloc(arena_need ? arena_need : 1);
  if (!arena) {
    free(order);
    free(hmap);
    free(uniq);
    free(owned);
    return false;
  }
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

  // Pass 4: draw everything from RAM.
  p = body;
  for (uint16_t l = 0; l < nlines && p + 4 <= rec_end; l++) {
    int16_t baseline;
    uint16_t count;
    memcpy(&baseline, p, 2);
    memcpy(&count, p + 2, 2);
    p += 4;
    int32_t x = 0;  // v4: x accumulates along the line and resets on each one
    for (uint16_t g = 0; g < count && p + 3 <= rec_end; g++) {
      uint32_t key = ((uint32_t)(p[0] + 1) << 16);
      uint16_t idx;
      memcpy(&idx, p + 1, 2);
      p += 3;
      if (v4) {
        x += readVarint(&p, rec_end);
      } else {
        if (p + 2 > rec_end) break;
        int16_t ax;
        memcpy(&ax, p, 2);
        x = ax;
        p += 2;
      }
      key |= idx;
      uint32_t h = (key * 2654435761u) & (kHash - 1);
      while (hmap[h] != 0xFFFF && uniq[hmap[h]].key != key) h = (h + 1) & (kHash - 1);
      if (hmap[h] == 0xFFFF) continue;
      PageGlyph* pg = &uniq[hmap[h]];
      if (pg->meta.w && pg->meta.h)
        drawGlyph(gfx, pg->meta, arena + pg->arena_off, (int)x, baseline);
    }
  }
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

  free(arena);
  free(order);
  free(hmap);
  free(uniq);
  free(owned);
  return true;
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
  if (*has_cover || *has_strip) return true;  // extracted on a previous scan

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
  if (ts) {
    b._f.seekSet(bits);
    *has_cover = writeXtBin(cov, tw, th, b._f, ts);
  }
  if (ss) {
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
  free(_page);  // the dictionary + page buffer lives only while a book is open
  _page = nullptr;
  _pageCap = _dictLen = 0;
}

}  // namespace reader
