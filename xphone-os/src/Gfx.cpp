#include "Gfx.h"

#include <cstdio>
#include <cstring>

bool Gfx::begin() {
  _fb = _d.getFrameBuffer();
  // Logical PORTRAIT frame (phone-style): logical width = native panel
  // height, logical height = native panel width (X3: 528x792, X4: 480x800).
  _w = _d.getDisplayHeight();
  _h = _d.getDisplayWidth();
  _wBytes = _d.getDisplayWidthBytes();  // native framebuffer stride
  return _fb != nullptr && _w > 0 && _h > 0;
}

void Gfx::drawPixel(const int x, const int y, const bool black) {
  if (x < 0 || y < 0 || x >= _w || y >= _h) return;
  // Logical portrait -> native panel: 90 degrees CLOCKWISE, chirality matching
  // CrossPoint's default GfxRenderer::Portrait (x4-os GfxRenderer.cpp
  // rotateCoordinates(): phyX = y; phyY = panelHeight - 1 - x, where native
  // panelHeight == our logical _w). Single transform point for ALL drawing —
  // rects and glyph blits funnel through here.
  const int phyX = y;
  const int phyY = _w - 1 - x;
  const uint32_t idx = static_cast<uint32_t>(phyY) * _wBytes + (phyX >> 3);
  const uint8_t mask = 0x80 >> (phyX & 7);
  if (black) {
    _fb[idx] &= ~mask;  // cleared bit = black
  } else {
    _fb[idx] |= mask;
  }
}

// M3.2 — integer Bresenham segment for glyph-scale marks (the Priorities
// checkmark). Each plotted point is stamped as a thickness x thickness filled
// square centered on it (offset -thickness/2). Every stamp pixel goes through
// drawPixel, which already clips to the panel and applies the portrait ->
// native rotation (verified above: the bounds check precedes the transform),
// so no bounds work is needed here. Per-pixel cost — fine for short strokes,
// too slow for long rules (use fillRect's byte runs instead).
void Gfx::drawLine(int x0, int y0, const int x1, const int y1, const int thickness, const bool black) {
  const int t = thickness < 1 ? 1 : thickness;
  const int dx = x1 > x0 ? x1 - x0 : x0 - x1;
  const int dy = y1 > y0 ? y1 - y0 : y0 - y1;
  const int sx = x0 < x1 ? 1 : -1;
  const int sy = y0 < y1 ? 1 : -1;
  int err = dx - dy;
  while (true) {
    for (int oy = 0; oy < t; oy++) {
      for (int ox = 0; ox < t; ox++) {
        drawPixel(x0 - t / 2 + ox, y0 - t / 2 + oy, black);
      }
    }
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 > -dy) {
      err -= dy;
      x0 += sx;
    }
    if (e2 < dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void Gfx::fillRect(const int x, const int y, const int w, const int h, const bool black) {
  // M2.1a byte-run fill. Transform is IDENTICAL to drawPixel (phyX = y,
  // phyY = _w - 1 - x): one logical COLUMN is one native framebuffer row, and
  // the logical-Y run inside it is the native bit/byte axis — so each column
  // becomes head-mask + memset middle + tail-mask instead of h drawPixel
  // calls (~5-15x less CPU on large fills; verified bit-exact against the
  // pixel-wise reference in a host-side self-check).
  if (w <= 0 || h <= 0) return;
  const int x0 = x < 0 ? 0 : x;
  const int y0 = y < 0 ? 0 : y;
  const int x1 = (x + w > _w) ? _w : x + w;  // exclusive
  const int y1 = (y + h > _h) ? _h : y + h;  // exclusive
  if (x0 >= x1 || y0 >= y1) return;

  const int px0 = y0;      // native X of the run start (phyX = y)
  const int px1 = y1 - 1;  // native X of the run end, inclusive
  const int firstByte = px0 >> 3;
  const int lastByte = px1 >> 3;
  const uint8_t headMask = static_cast<uint8_t>(0xFF >> (px0 & 7));         // bits px0..7 of firstByte
  const uint8_t tailMask = static_cast<uint8_t>(0xFF << (7 - (px1 & 7)));   // bits 0..px1 of lastByte
  for (int col = x0; col < x1; col++) {
    uint8_t* row = _fb + static_cast<uint32_t>(_w - 1 - col) * _wBytes;  // phyY = _w - 1 - x
    if (firstByte == lastByte) {
      const uint8_t m = headMask & tailMask;
      if (black) row[firstByte] &= static_cast<uint8_t>(~m);
      else row[firstByte] |= m;
      continue;
    }
    if (black) {
      row[firstByte] &= static_cast<uint8_t>(~headMask);
      if (lastByte - firstByte > 1) memset(row + firstByte + 1, 0x00, lastByte - firstByte - 1);
      row[lastByte] &= static_cast<uint8_t>(~tailMask);
    } else {
      row[firstByte] |= headMask;
      if (lastByte - firstByte > 1) memset(row + firstByte + 1, 0xFF, lastByte - firstByte - 1);
      row[lastByte] |= tailMask;
    }
  }
}

void Gfx::drawRect(const int x, const int y, const int w, const int h, const int t, const bool black) {
  fillRect(x, y, w, t, black);              // top
  fillRect(x, y + h - t, w, t, black);      // bottom
  fillRect(x, y + t, t, h - 2 * t, black);  // left
  fillRect(x + w - t, y + t, t, h - 2 * t, black);
}

namespace {
// Integer floor(sqrt(v)) for the small corner radii used here.
int isqrt(const int v) {
  int r = 0;
  while ((r + 1) * (r + 1) <= v) r++;
  return r;
}
}  // namespace

void Gfx::fillRoundedRect(const int x, const int y, const int w, const int h, int r, const bool black) {
  if (w <= 0 || h <= 0) return;
  if (r < 0) r = 0;
  if (2 * r > w) r = w / 2;
  if (2 * r > h) r = h / 2;
  // Straight middle band as ONE fillRect (byte-run fast path); only the
  // r corner rows top and bottom need per-row insets. Output identical to the
  // previous every-row loop (center rows had inset == 0).
  if (h > 2 * r) fillRect(x, y + r, w, h - 2 * r, black);
  for (int row = 0; row < r; row++) {
    const int cy = r - row;  // 1..r from the curved edge
    const int inset = r - isqrt(r * r - cy * cy);
    fillRect(x + inset, y + row, w - 2 * inset, 1, black);
  }
  for (int row = (h - r > r) ? h - r : r; row < h; row++) {
    const int cy = row - (h - r) + 1;
    const int inset = r - isqrt(r * r - cy * cy);
    fillRect(x + inset, y + row, w - 2 * inset, 1, black);
  }
}

void Gfx::drawRoundedRect(const int x, const int y, const int w, const int h, const int r, const int t,
                          const bool black) {
  fillRoundedRect(x, y, w, h, r, black);
  fillRoundedRect(x + t, y + t, w - 2 * t, h - 2 * t, r > t ? r - t : 0, !black);
}

// --- Text ------------------------------------------------------------------

// Minimal UTF-8 decoder (labels are ASCII; multibyte handled for safety).
// Returns 0 at end of string; advances *s past the consumed sequence.
uint32_t Gfx::nextCodepoint(const char** s) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(*s);
  const uint8_t b0 = p[0];
  if (b0 == 0) return 0;
  uint32_t cp;
  int len;
  if (b0 < 0x80) {
    cp = b0;
    len = 1;
  } else if ((b0 & 0xE0) == 0xC0) {
    cp = b0 & 0x1F;
    len = 2;
  } else if ((b0 & 0xF0) == 0xE0) {
    cp = b0 & 0x0F;
    len = 3;
  } else if ((b0 & 0xF8) == 0xF0) {
    cp = b0 & 0x07;
    len = 4;
  } else {  // stray continuation byte — skip it
    *s += 1;
    return 0xFFFD;
  }
  for (int i = 1; i < len; i++) {
    if ((p[i] & 0xC0) != 0x80) {  // truncated sequence
      *s += i;
      return 0xFFFD;
    }
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  *s += len;
  return cp;
}

const EpdGlyph* Gfx::findGlyph(const XpFont& f, const uint32_t cp) const {
  // Linear scan over the interval table (~51 entries for the Ubuntu fonts) —
  // sorted ascending by `first`, same data GfxRenderer binary-searches.
  for (uint32_t i = 0; i < f.intervalCount; i++) {
    const EpdUnicodeInterval& iv = f.intervals[i];
    if (cp < iv.first) break;  // sorted: no later interval can match
    if (cp <= iv.last) return &f.glyphs[iv.offset + (cp - iv.first)];
  }
  return nullptr;
}

void Gfx::blitGlyph(const XpFont& f, const EpdGlyph* g, const int penX, const int lineTopY, const bool black) {
  // Same packing as GfxRenderer renderCharImpl (1bpp branch): MSB-first bits,
  // pixelPosition = glyphY * width + glyphX, no per-row padding.
  //
  // M2.1a column-major blit: one logical glyph COLUMN is one native
  // framebuffer row (drawPixel transform: phyX = y, phyY = _w - 1 - x), so
  // walking gy inside gx lets up to 8 glyph pixels coalesce into a single
  // framebuffer read-modify-write with the transform hoisted out of the inner
  // loop. Set pixels only (transparent background), identical output to the
  // row-major pixel-wise loop (host-side self-check).
  const uint8_t* bmp = f.bitmap + g->dataOffset;
  const int x0 = penX + g->left;
  const int y0 = lineTopY + f.ascender - g->top;
  const int gw = g->width;
  const int gyStart = (y0 < 0) ? -y0 : 0;                          // clip top
  const int gyEnd = (y0 + g->height > _h) ? _h - y0 : g->height;   // clip bottom
  if (gyStart >= gyEnd) return;
  for (int gx = 0; gx < gw; gx++) {
    const int lx = x0 + gx;
    if (lx < 0 || lx >= _w) continue;  // clip left/right
    uint8_t* row = _fb + static_cast<uint32_t>(_w - 1 - lx) * _wBytes;
    int gy = gyStart;
    while (gy < gyEnd) {
      const int py = y0 + gy;  // native X, guaranteed in [0, _h)
      const int bit = py & 7;
      int chunk = 8 - bit;  // stay inside one framebuffer byte
      if (chunk > gyEnd - gy) chunk = gyEnd - gy;
      uint8_t m = 0;
      int pp = gy * gw + gx;
      for (int k = 0; k < chunk; k++, pp += gw) {
        if ((bmp[pp >> 3] >> (7 - (pp & 7))) & 1) m |= static_cast<uint8_t>(0x80 >> (bit + k));
      }
      if (m) {
        if (black) row[py >> 3] &= static_cast<uint8_t>(~m);
        else row[py >> 3] |= m;
      }
      gy += chunk;
    }
  }
}

int Gfx::textWidth(const XpFont& f, const char* text) const {
  if (!text) return 0;
  int32_t advFp = 0;  // 12.4 fixed-point accumulator
  uint32_t cp;
  while ((cp = nextCodepoint(&text)) != 0) {
    const EpdGlyph* g = findGlyph(f, cp);
    if (!g) g = findGlyph(f, '?');
    if (g) advFp += g->advanceX;
  }
  return fp4::toPixel(advFp);
}

void Gfx::drawText(const XpFont& f, const int x, const int y, const char* text, const bool black) {
  if (!text) return;
  int32_t advFp = 0;  // 12.4 fixed-point pen position relative to x
  uint32_t cp;
  while ((cp = nextCodepoint(&text)) != 0) {
    const EpdGlyph* g = findGlyph(f, cp);
    if (!g) g = findGlyph(f, '?');
    if (!g) continue;
    blitGlyph(f, g, x + fp4::toPixel(advFp), y, black);
    advFp += g->advanceX;
  }
}

void Gfx::drawTextCentered(const XpFont& f, const int cx, const int y, const char* text, const bool black) {
  drawText(f, cx - textWidth(f, text) / 2, y, text, black);
}

// M3 — greedy word wrap (Gfx.h contract). Line slices always end on UTF-8
// codepoint boundaries because the scanner only ever stops at positions
// returned by nextCodepoint(); multibyte glyphs the fonts lack fall back to
// '?' in drawText, same as every other text path.
int Gfx::drawTextWrapped(const XpFont& f, const int x, int y, const char* text, const int maxWidth,
                         const int maxLines, const bool black) {
  if (!text || !text[0] || maxWidth <= 0 || maxLines <= 0) return 0;

  int lines = 0;
  const char* pos = text;
  while (*pos && lines < maxLines) {
    while (*pos == ' ') pos++;  // never start a line on the break space
    if (!*pos) break;

    // Scan forward until the line overflows maxWidth, a '\n', or the end.
    const char* scan = pos;
    const char* fitEnd = pos;           // end (exclusive) of what fits
    const char* spaceBreak = nullptr;   // start of the last space that fits
    const char* resume = nullptr;       // where the next line starts
    int32_t advFp = 0;                  // 12.4 fixed-point width accumulator
    bool more = false;                  // text remains after this line
    while (true) {
      const char* cpStart = scan;
      const uint32_t cp = nextCodepoint(&scan);
      if (cp == 0) break;  // natural end: fitEnd already covers the rest
      if (cp == '\n') {    // forced break; resume after the newline
        fitEnd = cpStart;
        resume = scan;
        more = *scan != '\0';
        break;
      }
      const EpdGlyph* g = findGlyph(f, cp);
      if (!g) g = findGlyph(f, '?');
      if (g) advFp += g->advanceX;
      if (fp4::toPixel(advFp) > maxWidth) {
        more = true;
        if (spaceBreak && spaceBreak > pos) {  // break at the last space
          fitEnd = spaceBreak;
          resume = spaceBreak;
        } else if (fitEnd == pos) {  // single codepoint wider than the line
          fitEnd = scan;
          resume = scan;
        } else {  // overlong word: hard-break at the last codepoint that fit
          resume = fitEnd;
        }
        break;
      }
      fitEnd = scan;
      if (cp == ' ') spaceBreak = cpStart;
    }
    if (!resume) resume = fitEnd;

    // Copy the slice (fields are protocol-capped well under this buffer).
    char line[192];
    std::size_t len = static_cast<std::size_t>(fitEnd - pos);
    if (len > sizeof(line) - 1) len = sizeof(line) - 1;
    memcpy(line, pos, len);
    line[len] = '\0';

    // Last permitted line with text left over -> re-clip to end in "...".
    const char* rest = resume;
    while (*rest == ' ') rest++;
    if (lines == maxLines - 1 && (more || *rest)) {
      char probe[196];
      while (true) {
        snprintf(probe, sizeof(probe), "%s...", line);
        if (textWidth(f, probe) <= maxWidth || line[0] == '\0') break;
        std::size_t l = strlen(line);
        do {  // strip one whole UTF-8 sequence (skip continuation bytes)
          l--;
        } while (l > 0 && (static_cast<uint8_t>(line[l]) & 0xC0) == 0x80);
        line[l] = '\0';
      }
      snprintf(line, sizeof(line), "%s", probe);
    }

    drawText(f, x, y, line, black);
    y += f.lineAdvance;
    lines++;
    pos = resume;
  }
  return lines;
}

// --- Partial-window refresh (M2.1a) -----------------------------------------

Gfx::FlushTier Gfx::flushWindow(const int x, const int y, const int w, const int h) {
  // Clamp the LOGICAL rect; anything degenerate falls back to full-panel FAST.
  const int x0 = x < 0 ? 0 : x;
  const int y0 = y < 0 ? 0 : y;
  const int x1 = (x + w > _w) ? _w : x + w;  // exclusive
  const int y1 = (y + h > _h) ? _h : y + h;  // exclusive
  if (x0 >= x1 || y0 >= y1) {
    _d.displayBuffer(EInkDisplay::FAST_REFRESH);
    return FlushTier::Fast;
  }

  // Logical -> native, from the drawPixel transform (phyX = y, phyY = _w-1-x):
  // the native X span is the logical Y span, the native Y span is the flipped
  // logical X span. Native X is then expanded outward to byte boundaries —
  // the UC8253 PTL window (0x90) has 8px horizontal resolution and the
  // SSD1677 displayWindow path requires x%8 == 0 && w%8 == 0 (silent no-op
  // otherwise, Ssd1677Driver.cpp:340-341).
  const int nativeW = _d.getDisplayWidth();   // == logical _h
  const int nativeH = _d.getDisplayHeight();  // == logical _w
  int nx0 = y0 & ~7;
  int nx1 = (y1 - 1) | 7;
  if (nx1 >= nativeW) nx1 = nativeW - 1;  // panel widths are /8 => stays aligned
  const int ny0 = _w - x1;
  const int nh = x1 - x0;
  const int nw = nx1 - nx0 + 1;
  if (nx0 < 0 || nw <= 0 || ny0 < 0 || ny0 + nh > nativeH) {
    _d.displayBuffer(EInkDisplay::FAST_REFRESH);
    return FlushTier::Fast;
  }

  // Both panels now take the driver-native displayWindow:
  //  - SSD1677/X4: true partial (Ssd1677Driver.cpp:338-383) — differential
  //    FAST refresh of the window with BW/RED planes re-synced afterwards.
  //  - UC8253/X3 (M5 Phase 3, E1): PTL windowed FAST with the full-voltage
  //    `_fast` bank and the standard DTM contract (DTM2 window write -> fire ->
  //    DTM1 window resync). This is NOT the retired M2.1a composite — that one
  //    misused the half-conditioning preBwMid settle bank as its visible
  //    updater and left particles weakly driven (ghosting/gray blacks). The
  //    native path uses the exact bank+CDI a full-frame FAST uses, scoped by
  //    the controller's own partial window, and falls back to full-frame FAST
  //    internally whenever the RAM state can't support a clean differential.
  // Alignment/bounds were validated above, so silent-reject guards can't fire.
  _d.displayWindow(static_cast<uint16_t>(nx0), static_cast<uint16_t>(ny0), static_cast<uint16_t>(nw),
                   static_cast<uint16_t>(nh));
  return FlushTier::Partial;
}

void Gfx::flushWindowFlash(const int x, const int y, const int w, const int h) {
  // Same logical->native mapping as flushWindow, but best-effort: an invalid
  // window is silently dropped (press feedback must never cost a full flush).
  const int x0 = x < 0 ? 0 : x;
  const int y0 = y < 0 ? 0 : y;
  const int x1 = (x + w > _w) ? _w : x + w;  // exclusive
  const int y1 = (y + h > _h) ? _h : y + h;  // exclusive
  if (x0 >= x1 || y0 >= y1) return;
  const int nativeW = _d.getDisplayWidth();
  const int nativeH = _d.getDisplayHeight();
  int nx0 = y0 & ~7;
  int nx1 = (y1 - 1) | 7;
  if (nx1 >= nativeW) nx1 = nativeW - 1;
  const int ny0 = _w - x1;
  const int nh = x1 - x0;
  const int nw = nx1 - nx0 + 1;
  if (nx0 < 0 || nw <= 0 || ny0 < 0 || ny0 + nh > nativeH) return;
  _d.displayWindowFlash(static_cast<uint16_t>(nx0), static_cast<uint16_t>(ny0), static_cast<uint16_t>(nw),
                        static_cast<uint16_t>(nh));
}
