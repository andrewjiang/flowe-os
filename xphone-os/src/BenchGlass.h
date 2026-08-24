#pragma once

// Bench-only "glass twin" support — the exact pixels, not a photograph.
//
// The gooseneck camera sees the panel at roughly 500 px across, which is under
// one camera pixel per panel pixel, so a photo can never be pixel-exact. This
// file gives the bench two things that are:
//
//   fb   dumps the live framebuffer over USB CDC. The host decodes it back to
//        a 1-bit PNG at the panel's true logical size (X3 528x792, X4 480x800).
//        That is ground truth: what the firmware believes is on glass.
//   cal  draws a calibration pattern. The host photographs it, finds the four
//        panel corners, and solves a homography. After that any camera frame
//        warps onto the exact panel pixel grid, so a photo can be diffed
//        against the framebuffer. That measures what the GLASS really shows —
//        ghosting, contrast, a stuck row — which the framebuffer cannot know.
//
// Host side: tools/glass-twin/twin.py.
//
// Wire format of `fb` (one header line, N base64 lines, one footer line):
//
//   [xphone-os] fb begin dev=xteink_x3 nw=792 nh=528 stride=99 bytes=52272
//               lw=528 lh=792 orient=portrait enc=rle-b64 crc32=deadbeef
//   orient: portrait = Gfx's 90 CW rotation, landscape = identity (the
//   panel's native orientation). The twin picks its mapping from this.
//   fb:<base64, 76 chars>
//   ...
//   [xphone-os] fb end lines=137
//
// nw/nh/stride describe the NATIVE landscape framebuffer that is dumped
// verbatim; lw/lh are the logical portrait size the UI draws in. The host
// applies the same 90-degree rotation Gfx::drawPixel does (phyX = y,
// phyY = lw - 1 - x), so the decode cannot drift from the firmware.
//
// The payload is PackBits-style byte RLE, then base64. E-ink UI frames are
// mostly 0xFF runs, so a launcher frame lands near 4 KB instead of 52 KB.
// crc32 is over the RAW framebuffer, so a bad decode is caught end to end.
//
// Cost: no heap, no static buffers beyond one 80-byte line. Streaming
// RLE -> base64 -> serial, scanning the framebuffer in place.

#include <Arduino.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Fonts.h"
#include "Gfx.h"

namespace bench {

// --- CRC32 (reflected, poly 0xEDB88320) -------------------------------------
// Nibble table: 16 entries instead of 256, two lookups per byte. 52 KB costs
// well under 10 ms at 80 MHz, which is noise next to the serial transfer.
inline uint32_t crc32(const uint8_t* data, uint32_t len) {
  static constexpr uint32_t kNibble[16] = {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
                                           0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
                                           0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
                                           0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C};
  uint32_t crc = 0xFFFFFFFF;
  for (uint32_t i = 0; i < len; i++) {
    crc ^= data[i];
    crc = (crc >> 4) ^ kNibble[crc & 0x0F];
    crc = (crc >> 4) ^ kNibble[crc & 0x0F];
  }
  return ~crc;
}

// --- streaming base64 -> "fb:" lines -----------------------------------------
class B64LineWriter {
 public:
  void putByte(const uint8_t b) {
    _acc[_accN++] = b;
    if (_accN == 3) {
      emitGroup(3);
      _accN = 0;
    }
  }

  // Flush the base64 tail (with padding) and any partial line.
  void finish() {
    if (_accN > 0) {
      _acc[1] = _accN > 1 ? _acc[1] : 0;
      _acc[2] = 0;
      emitGroup(_accN);
      _accN = 0;
    }
    if (_len > 0) emitLine();
  }

  uint32_t lines() const { return _lines; }

 private:
  static constexpr uint8_t kLineChars = 76;  // multiple of 4: no group is split

  static char enc(const uint8_t v) {
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    return kAlphabet[v & 0x3F];
  }

  void emitGroup(const uint8_t n) {
    const uint32_t v = (static_cast<uint32_t>(_acc[0]) << 16) | (static_cast<uint32_t>(_acc[1]) << 8) | _acc[2];
    pushChar(enc(v >> 18));
    pushChar(enc(v >> 12));
    pushChar(n > 1 ? enc(v >> 6) : '=');
    pushChar(n > 2 ? enc(v) : '=');
  }

  void pushChar(const char c) {
    _line[_len++] = c;
    if (_len == kLineChars) emitLine();
  }

  void emitLine() {
    _line[_len] = 0;
    Serial.print("fb:");
    Serial.println(_line);
    _len = 0;
    // Let the USB-CDC FIFO drain and feed the watchdog. The logger reads
    // continuously, so this stays cheap; without it a long dump can stall.
    if ((++_lines & 0x1F) == 0) delay(1);
  }

  char _line[kLineChars + 1] = {0};
  uint8_t _len = 0;
  uint8_t _acc[3] = {0, 0, 0};
  uint8_t _accN = 0;
  uint32_t _lines = 0;
};

// --- fb ---------------------------------------------------------------------
// PackBits-style RLE: a token with bit 7 set is a run of (token & 0x7F) copies
// of the next byte; otherwise it is a literal count and that many bytes follow.
// Token 0 is never emitted, so the host can treat it as a stream error.
inline void dumpFrameBuffer(Gfx& gfx) {
  EInkDisplay& d = gfx.display();
  const uint8_t* fb = d.getFrameBuffer();
  if (fb == nullptr) {
    Serial.println("[xphone-os] fb error: no framebuffer");
    return;
  }
  const uint16_t nw = d.getDisplayWidth();        // native landscape width  (X3 792)
  const uint16_t nh = d.getDisplayHeight();       // native landscape height (X3 528)
  const uint16_t stride = d.getDisplayWidthBytes();
  const uint32_t bytes = static_cast<uint32_t>(stride) * nh;

  Serial.printf(
      "[xphone-os] fb begin dev=%s nw=%u nh=%u stride=%u bytes=%lu lw=%d lh=%d orient=%s "
      "enc=rle-b64 crc32=%08lx\n",
      BoardConfig::ACTIVE.name, static_cast<unsigned>(nw), static_cast<unsigned>(nh),
      static_cast<unsigned>(stride), static_cast<unsigned long>(bytes), gfx.width(), gfx.height(),
      gfx.orientation() == Gfx::Orient::Landscape ? "landscape" : "portrait",
      static_cast<unsigned long>(crc32(fb, bytes)));

  B64LineWriter out;
  uint32_t i = 0;
  while (i < bytes) {
    uint32_t runEnd = i + 1;
    while (runEnd < bytes && fb[runEnd] == fb[i] && (runEnd - i) < 127) runEnd++;
    if ((runEnd - i) >= 3) {
      out.putByte(static_cast<uint8_t>(0x80 | (runEnd - i)));
      out.putByte(fb[i]);
      i = runEnd;
      continue;
    }
    // Literals up to the next run of 3+ (or 127 bytes, whichever comes first).
    uint32_t j = i;
    while (j < bytes && (j - i) < 127) {
      if (j + 2 < bytes && fb[j] == fb[j + 1] && fb[j] == fb[j + 2]) break;
      j++;
    }
    if (j == i) j = i + 1;  // never emit a zero token
    out.putByte(static_cast<uint8_t>(j - i));
    for (uint32_t k = i; k < j; k++) out.putByte(fb[k]);
    i = j;
  }
  out.finish();
  Serial.printf("[xphone-os] fb end lines=%lu\n", static_cast<unsigned long>(out.lines()));
}

// --- cal --------------------------------------------------------------------
// Every pattern is drawn in LOGICAL portrait coordinates, so the host reasons
// in exactly one coordinate system for both the framebuffer and the camera.

// A 1px checkerboard of `cell`-sized squares. cell=1 is the hardest MTF target
// a 1-bit panel can show: if the camera resolves it, the optics are good
// enough to trust a per-pixel diff.
inline void fillChecker(Gfx& g, const int x, const int y, const int w, const int h, const int cell) {
  for (int row = 0; row < h; row++) {
    for (int col = 0; col < w; col++) {
      const bool black = (((col / cell) + (row / cell)) & 1) == 0;
      if (black) g.drawPixel(x + col, y + row, true);
    }
  }
}

// Ordered 25% dither (1 ink pixel per 2x2 cell).
inline void fillQuarter(Gfx& g, const int x, const int y, const int w, const int h) {
  for (int row = 0; row < h; row++) {
    for (int col = 0; col < w; col++) {
      if ((col & 1) == 0 && (row & 1) == 0) g.drawPixel(x + col, y + row, true);
    }
  }
}

// Pattern "frame" — the one the homography is solved from.
//
//   * A 4px black border on the exact panel edge. Four straight lines fitted to
//     its outer edge intersect at the four true panel corners with subpixel
//     accuracy. That beats blob centroids, and it anchors logical (0,0)
//     directly.
//   * Three filled finder squares (top-left, top-right, bottom-left; the
//     bottom-right is deliberately EMPTY). The missing corner resolves
//     orientation, so a rotated or mirrored camera mount cannot fool the solve.
//   * A centre cross for the residual check.
//   * Ruler ticks every 25 px along the top and left, long every 100 px.
//   * A tone strip: solid black, 1px checker (50%), 2x2 dither (25%), white.
//     Those four patches calibrate the camera's grey thresholds, and after a
//     scene change they also reveal ghosting.
inline void drawCalFrame(Gfx& g) {
  const int w = g.width();
  const int h = g.height();
  g.clear();

  g.drawRect(0, 0, w, h, 4, true);  // panel-edge border: the corner solve

  constexpr int kFinder = 48;
  constexpr int kInset = 20;
  g.fillRect(kInset, kInset, kFinder, kFinder, true);                        // TL
  g.fillRect(w - kInset - kFinder, kInset, kFinder, kFinder, true);          // TR
  g.fillRect(kInset, h - kInset - kFinder, kFinder, kFinder, true);          // BL
  // bottom-right stays white on purpose — that asymmetry IS the orientation key

  const int cx = w / 2;
  const int cy = h / 2;
  g.fillRect(cx - 80, cy - 2, 160, 4, true);
  g.fillRect(cx - 2, cy - 80, 4, 160, true);

  // Ruler ticks, inside the border, starting at the border's inner edge.
  for (int p = 0; p <= w; p += 25) {
    const int len = (p % 100 == 0) ? 18 : 9;
    g.fillRect(p, 4, 2, len, true);
  }
  for (int p = 0; p <= h; p += 25) {
    const int len = (p % 100 == 0) ? 18 : 9;
    g.fillRect(4, p, len, 2, true);
  }

  // Tone strip: black / 50% checker / 25% dither / outlined white.
  const int patch = 72;
  const int stripW = patch * 4;
  const int sx = cx - stripW / 2;
  const int sy = cy + 140;
  g.fillRect(sx, sy, patch, patch, true);
  fillChecker(g, sx + patch, sy, patch, patch, 1);
  fillQuarter(g, sx + patch * 2, sy, patch, patch);
  g.drawRect(sx + patch * 3, sy, patch, patch, 1, true);

  // A 4px-cell checkerboard block: coarse enough to survive soft optics, so
  // scale and shear show up even before the fine patches resolve.
  fillChecker(g, cx - 96, cy - 240, 192, 96, 4);

  char label[48];
  snprintf(label, sizeof(label), "%s  %dx%d", BoardConfig::ACTIVE.name, w, h);
  g.drawTextCentered(kFontBold, cx, cy - 300, label);
  g.drawTextCentered(kFontRegular, cx, cy + 60, "glass-twin calibration");
}

// Pattern "grid" — 1px rules every 8 px. Residual warp after the homography
// shows as a bent or doubled rule.
inline void drawCalGrid(Gfx& g) {
  const int w = g.width();
  const int h = g.height();
  g.clear();
  for (int x = 0; x < w; x += 8) g.fillRect(x, 0, 1, h, true);
  for (int y = 0; y < h; y += 8) g.fillRect(0, y, w, 1, true);
  g.drawRect(0, 0, w, h, 4, true);
}

// Pattern "checker" — full-panel 1px checkerboard. Every pixel is exercised, so
// a stuck row or column is unmissable, and the camera's limit is measured.
inline void drawCalChecker(Gfx& g) {
  g.clear();
  fillChecker(g, 0, 0, g.width(), g.height(), 1);
  g.drawRect(0, 0, g.width(), g.height(), 4, true);
}

// Pattern "selftest" — proves the host's decode is bit-exact.
//
// A plain checkerboard is a weak test: it is symmetric, so a host that had the
// rotation backwards, or x and y transposed, or an off-by-one in the stride,
// would still reproduce it perfectly and look right. This pattern instead makes
// every pixel a hash of its OWN coordinates, with different multipliers for x
// and y. Nothing but the exact mapping reproduces it.
//
// The host recomputes the same hash and compares all 418,176 pixels. Two
// unrelated 25%-density patterns agree on about 62.5% of pixels by chance, so
// a broken mapping cannot hide near 100%.
//
// It also stresses the transport: pseudorandom bits barely compress, so this is
// the only frame that exercises the RLE encoder's literal path end to end. The
// UI, being mostly white runs, never touches it.
inline bool selfTestInk(const int x, const int y) {
  uint32_t h = static_cast<uint32_t>(x) * 2654435761u ^ static_cast<uint32_t>(y) * 2246822519u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  return (h & 3u) == 0u;
}

inline void drawCalSelfTest(Gfx& g) {
  g.clear();
  for (int y = 0; y < g.height(); y++) {
    for (int x = 0; x < g.width(); x++) {
      if (selfTestInk(x, y)) g.drawPixel(x, y, true);
    }
  }
}

// Pattern "bars" — what can the CAMERA actually see?
//
// The framebuffer path is exact, but the camera under-samples the glass, and
// "the tool says it is there" is worth little if nobody knows the smallest
// feature a photo can confirm. This draws paired horizontal and vertical rules
// at 1, 2, 3, 4, 6 and 8 px, each labelled. Photograph it and read off the
// finest bar that still separates: that is the honest resolution limit of the
// camera at its current position.
inline void drawCalBars(Gfx& g) {
  const int w = g.width();
  const int h = g.height();
  g.clear();
  g.drawRect(0, 0, w, h, 4, true);
  g.drawTextCentered(kFontBold, w / 2, 24, "resolution wedge");

  static const int kWidths[] = {1, 2, 3, 4, 6, 8};
  const int blockH = 84;
  int y = 64;
  for (unsigned i = 0; i < sizeof(kWidths) / sizeof(kWidths[0]); i++) {
    const int t = kWidths[i];
    char label[8];
    snprintf(label, sizeof(label), "%dpx", t);
    g.drawText(kFontRegular, 24, y + blockH / 2 - 14, label);

    // Vertical rules, one gap of the same width between them: the classic
    // line-pair target. If they merge into grey, the camera cannot resolve t.
    int x = 110;
    while (x + t <= w / 2 - 12) {
      g.fillRect(x, y, t, blockH - 12, true);
      x += t * 2;
    }
    // Horizontal rules, same widths, to catch an axis-dependent blur.
    int yy = y;
    while (yy + t <= y + blockH - 12) {
      g.fillRect(w / 2 + 12, yy, w - (w / 2 + 12) - 24, t, true);
      yy += t * 2;
    }
    y += blockH;
    if (y + blockH > h - 60) break;
  }
}

// Pattern "proof" — the renderer-equivalence battery.
//
// Draws every Gfx primitive, including its edge cases, in a fixed order. The
// host repeats the identical battery through the xpgfx.py port of Gfx, and
// `twin prove` demands pixel equality between this frame (via `fb`) and the
// host render. Passing proves the design chain end to end: a screen composed
// with xpgfx IS the frame this firmware will draw.
//
// MUST stay in lockstep with tools/glass-twin/xpgfx.py draw_proof — any edit
// here without the mirror edit there makes prove fail loudly, by design.
inline void drawCalProof(Gfx& g) {
  const int w = g.width();
  const int h = g.height();
  g.clear();
  g.drawRect(0, 0, w, h, 3, true);
  // clipped fills: off every edge
  g.fillRect(-20, 40, 60, 24, true);
  g.fillRect(w - 40, 80, 60, 24, true);
  g.fillRect(w / 3, -10, 40, 30, true);
  g.fillRect(w / 2, h - 15, 40, 30, true);
  // rounded rects, including the radius clamp and the interior erase
  g.fillRoundedRect(20, 120, 120, 60, 14, true);
  g.drawRoundedRect(160, 120, 140, 60, 10, 3, true);
  g.drawRoundedRect(20, 200, w - 40, 70, 18, 2, true);
  g.fillRoundedRect(w - 100, 130, 70, 40, 40, true);
  // lines at assorted slopes and thicknesses
  g.drawLine(20, 300, w - 20, 340, 1, true);
  g.drawLine(20, 310, w - 20, 390, 3, true);
  g.drawLine(w - 30, 300, 30, 420, 2, true);
  g.drawLine(w / 2, 300, w / 2, 430, 5, true);
  // text: three fonts, centring, both clip sides, subset fallback to '?'
  g.drawText(kFontBold, 20, 440, "Pack my box: 5 dozen quartz Jugs!");
  g.drawText(kFontRegular, 20, 470, "waltz, bad nymph & quick jigs (0123456789)");
  g.drawText(kFontSmall, 20, 500, "SOFT KEYS 10pt: BACK OPEN PREV NEXT");
  g.drawTextCentered(kFontBold, w / 2, 525, "centered ĂĉÉñ→ text");
  g.drawText(kFontRegular, w - 60, 550, "edge clip test at the right margin");
  g.drawText(kFontRegular, -7, 575, "left-clipped glyphs");
  g.drawTextWrapped(kFontRegular, 20, 605,
                    "The quick brown fox jumps over the lazy dog again and again until the "
                    "line must wrap and finally be truncated with an ellipsis because there "
                    "is far more text than fits in three lines at this width.",
                    w - 120, 3);
  // inverted text on a black band
  g.fillRect(20, h - 100, w - 40, 40, true);
  g.drawText(kFontBold, 30, h - 94, "INVERTED", false);
  // parity checker via the pixel path
  for (int y = h - 50; y < h - 20; y++) {
    for (int x = w - 120; x < w - 20; x++) {
      if (((x ^ y) & 1) == 0) g.drawPixel(x, y, true);
    }
  }
}

// Returns false for an unknown name.
inline bool drawCalPattern(Gfx& g, const char* name) {
  if (!strcmp(name, "frame")) {
    drawCalFrame(g);
  } else if (!strcmp(name, "grid")) {
    drawCalGrid(g);
  } else if (!strcmp(name, "checker")) {
    drawCalChecker(g);
  } else if (!strcmp(name, "selftest")) {
    drawCalSelfTest(g);
  } else if (!strcmp(name, "proof")) {
    drawCalProof(g);
  } else if (!strcmp(name, "bars")) {
    drawCalBars(g);
  } else if (!strcmp(name, "white")) {
    g.clear();
  } else if (!strcmp(name, "black")) {
    g.clear();
    g.fillRect(0, 0, g.width(), g.height(), true);
  } else {
    return false;
  }
  return true;
}

}  // namespace bench
