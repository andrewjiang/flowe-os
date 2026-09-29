// Shared JPEG/PNG -> box-sample -> tone-pass -> Floyd-Steinberg -> packed 1bpp
// image pipeline. Originally the cover-thumbnail path (reader/CoverThumb.cpp),
// extracted so any caller with a plain HalFile (not just an EPUB-extracted
// cover) can reuse the same decoders — e.g. BackgroundImage.cpp, which reads a
// bare /background.bmp off the SD root.
//
// .bin format (all little-endian), unchanged from the original cover thumb:
//   offset 0  uint16 magic 0x5854 ('XT')
//   offset 2  uint8  version (1)
//   offset 3  uint8  reserved (0)
//   offset 4  uint16 width in pixels
//   offset 6  uint16 height in pixels
//   offset 8  height rows of ceil(width/8) bytes, MSB-first within each byte.
// A SET bit is ink (black) — blit() stamps only ink pixels via
// Gfx::drawPixel(x, y, true) so the paper keeps the framebuffer's white.
#pragma once

#include <string>

class HalFile;
class Gfx;

namespace imagethumb {

// Logical panel size, sized to the larger of the two device profiles (X3
// 528x792, X4 480x800 — Gfx.h) so a full-screen BackgroundImage fits, not
// just a cover-grid-sized thumbnail.
constexpr int kMaxThumbW = 528;
constexpr int kMaxThumbH = 800;

enum class DecodeResult { Ok, Transient, Permanent };
enum class ImgFormat { Jpeg, Png, Bmp, Unknown };

// Aspect-preserving fit of srcW x srcH within maxW x maxH, never upscaling
// past 1:1. 16.16 fixed point; outputs are always >= 1.
void fitWithin(int srcW, int srcH, int maxW, int maxH, int* outW, int* outH);

// Magic bytes win; `hintPath` (source filename/href) only backs up an
// unreadable header via its extension.
ImgFormat sniffFormat(const std::string& imgPath, const std::string& hintPath);

// Decode `imgFile` (already open for read) into `binOut` (already open for
// write), fit within maxW x maxH, writing the header above. Deferred-work
// path: never call from a per-tick/render path.
DecodeResult decodeJpegThumb(HalFile& imgFile, HalFile& binOut, int maxW, int maxH);
DecodeResult decodePngThumb(HalFile& imgFile, HalFile& binOut, int maxW, int maxH);
// Uncompressed BMP only (BI_RGB, 1/4/8/24/32 bpp, top-down or bottom-up) —
// no RLE. Needs no decoder library or scratch buffer, unlike JPEG/PNG: a BMP
// row is already raw pixels, just read (and un-flip if bottom-up) and
// converted to gray. This is what BackgroundImage uses, precisely because it
// cannot fail on a fragmented heap.
DecodeResult decodeBmpThumb(HalFile& imgFile, HalFile& binOut, int maxW, int maxH);

// Best-effort: claim the JPEG decoder's scratch (~21KB) while the heap is at
// its cleanest. Grow-only: the first PNG cover resizes it to the ~58KB union.
void preacquireScratch();

// Free the shared JPEG/PNG decoder scratch block. CoverThumb (the Reader
// cover grid) deliberately keeps it retained across a whole grid session and
// frees it on releaseScratch() when leaving the grid. A one-shot caller like
// BackgroundImage should call this right after its own decode completes —
// holding a 21-58KB block on the heap indefinitely, outside a hot decode
// loop, only fragments the heap for the rest of the session.
void releaseScratch();

// Blit a thumb/.bin file at (x, y) logical coordinates. Streams one packed
// row at a time through a stack buffer (no heap) — safe in render paths.
// clipX0/clipX1 bound the columns actually stamped (half-open, logical
// coords); default is unbounded.
bool blit(Gfx& gfx, const char* binPath, int x, int y, int* outW = nullptr, int* outH = nullptr,
          int clipX0 = -32768, int clipX1 = 32767);

// Read just the 8-byte header of a thumb/.bin file (no pixel data) to learn
// its encoded size — e.g. so a caller can center a smaller-than-canvas image
// before calling blit(). False on a missing/invalid file.
bool peekSize(const char* binPath, int* outW, int* outH);

}  // namespace imagethumb
