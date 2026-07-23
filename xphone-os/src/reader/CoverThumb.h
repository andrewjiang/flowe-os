// R2a cover thumbnail pipeline: extracts a book's cover image from the EPUB
// zip, decodes it (JPEGDEC / PNGdec, vendored in lib/), box-samples it down to
// a tile-sized grayscale, Bayer-8x8 dithers to 1bpp and caches the result as
// <cachePath>/cover_<w>x<h>.bin so scenes can blit it without ever paying the
// decode cost twice.
//
// Thumb .bin format (all little-endian):
//   offset 0  uint16 magic 0x5854 ('XT')
//   offset 2  uint8  version (1)
//   offset 3  uint8  reserved (0)
//   offset 4  uint16 width in pixels
//   offset 6  uint16 height in pixels
//   offset 8  height rows of ceil(width/8) bytes, MSB-first within each byte.
// A SET bit is ink (black) — draw() stamps only ink pixels via
// Gfx::drawPixel(x, y, true) so the paper keeps the framebuffer's white.
// (Note: this is the OPPOSITE of art/LauncherIcons.h, where a CLEARED bit is
// ink; the icon arrays predate this format and are not thumb files.)
#pragma once

#include <string>

class Gfx;

namespace reader {

class Epub;

class CoverThumb {
 public:
  // Ensure <epub.getCachePath()>/cover_<w>x<h>.bin exists, building it from
  // the EPUB's cover image if needed. Deferred-work path: never call from a
  // per-tick/render path. The JPEG (~21KB) and PNG (~58KB) decoders run out of
  // a single shared scratch block that is allocated once on first use and
  // RETAINED — a fresh 58KB PNG decoder can't be malloc'd reliably on the
  // fragmented heap that BLE + grid browsing leaves, which is why PNG-cover
  // books (common on Project Gutenberg) used to never render a cover while
  // JPEG ones did. The thumb is aspect-fit within w x h (never upscaled past
  // 1:1), so the file's dimensions can be smaller than requested. Returns false
  // for books without covers (caller renders a text-only tile) and on decode
  // failure (a cover.none sentinel then suppresses retries; a failed scratch
  // allocation is transient and leaves no sentinel).
  static bool ensure(Epub& epub, int w, int h, std::string* outPath);

  // Free the shared decoder scratch block (~58KB). Call when leaving the book
  // grid to open a book, so section indexing / reading get the full heap back —
  // the block reallocates lazily the next time a cover is decoded.
  static void releaseScratch();

  // Blit a thumb file at (x, y) logical coordinates. Streams one packed row
  // at a time through a stack buffer (no heap) — safe in render paths. Only
  // ink (set) bits are drawn. Optionally reports the thumb's dimensions.
  static bool draw(Gfx& gfx, const char* binPath, int x, int y, int* outW = nullptr, int* outH = nullptr);
};

}  // namespace reader
