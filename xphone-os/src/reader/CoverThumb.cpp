// R2a cover thumbnail pipeline — see CoverThumb.h for the .bin format.
//
// Decode strategy (both formats stream, nothing holds a full image in RAM):
//  - JPEG (JPEGDEC): pick the largest 1/2 / 1/4 / 1/8 decode scale whose grid
//    still covers the target, buffer one MCU row band (16 x decodedW bytes,
//    the x4-os JpegToBmpConverter approach), then box-sample completed source
//    rows into the target row accumulators.
//  - PNG (PNGdec): scanlines arrive top-to-bottom via PNG_DRAW; each line is
//    converted to 8-bit gray and box-sampled the same way.
// Target rows are Bayer-8x8 ordered-dithered to 1bpp as they complete and
// written straight to the output file, so the accumulator footprint is just
// 2 x 4 x outW bytes regardless of source size.
#include "CoverThumb.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Memory.h>
#include <PNGdec.h>

#include <cctype>
#include <cstring>
#include <new>

#include "../Gfx.h"
#include "Epub.h"
#include "ReaderLog.h"

namespace reader {
namespace {

// One persistent scratch block, sized to the larger of the two decoders, shared
// by the JPEG (~21KB) and PNG (~58KB) paths — they never run concurrently (one
// cover at a time). Allocated lazily on first cover decode, when the heap is
// least fragmented (reader engine not yet loaded), and RETAINED so every later
// cover reuses it instead of re-requesting a 58KB contiguous block that a
// BLE-active fragmented heap routinely can't satisfy. CoverThumb::releaseScratch
// frees it when the reader leaves the grid to open a book.
// Per-format scratch. sizeof(PNG) embeds PNG_MAX_BUFFERED_PIXELS row buffers
// (~50 KB) while JPEGDEC — the common cover format — needs roughly half that.
// Sizing to the union meant JPEG covers failed whenever no PNG-sized hole
// existed (live X4 repro: "alloc failed (80244 free)" — free, not contiguous).
// Allocate what the sniffed format needs; grow-only so a JPEG→PNG→JPEG grid
// pass doesn't thrash, and a transient miss stays retryable on a later tick.
uint8_t* g_decoderScratch = nullptr;
size_t g_decoderScratchSize = 0;

// Smallest ask that already failed this grid session. Fragmentation doesn't
// heal between quiet ticks, so retrying an equal-or-bigger ask every pass just
// burns ~600 ms per grid entry re-extracting the image before failing again.
// Reset by preacquireScratch()/releaseScratch() (i.e., next Reader entry).
size_t g_scratchFailedNeed = 0;

uint8_t* acquireDecoderScratch(size_t need) {
  if (g_decoderScratch && g_decoderScratchSize < need) {
    free(g_decoderScratch);
    g_decoderScratch = nullptr;
    g_decoderScratchSize = 0;
  }
  if (!g_decoderScratch) {
    if (g_scratchFailedNeed && need >= g_scratchFailedNeed) {
      return nullptr;  // already proven unfit this session; fail fast, no log spam
    }
    g_decoderScratch = static_cast<uint8_t*>(malloc(need));
    if (g_decoderScratch) {
      g_decoderScratchSize = need;
    } else {
      g_scratchFailedNeed = need;
      LOG_DBG("CVR", "Decoder scratch alloc failed (need %u, free %u, largest %u)",
              static_cast<unsigned>(need), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    }
  }
  return g_decoderScratch;
}

constexpr uint16_t kMagic = 0x5854;  // 'XT'
constexpr uint8_t kVersion = 1;
constexpr int kHeaderSize = 8;
constexpr int kMaxThumbW = 528;  // logical panel width — thumbs never exceed the screen
constexpr int kMaxThumbH = 792;
constexpr int kMaxRowBytes = (kMaxThumbW + 7) / 8;
constexpr int kMaxSrcDim = 4096;       // reject absurd source images outright
constexpr int kMaxDecodedWidth = 2048; // caps the JPEG MCU band at 16 * 2048 = 32KB
constexpr int kMaxMcuHeight = 16;      // tallest JPEG MCU (4:2:0 chroma)

// Failures that would recur on every attempt (broken/unsupported image) get a
// cover.none sentinel; transient ones (low heap, SD hiccup) stay retryable.
enum class DecodeResult { Ok, Transient, Permanent };

enum class ImgFormat { Jpeg, Png, Unknown };

// Standard Bayer 8x8 ordered-dither matrix (0..63).
const uint8_t kBayer8[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},   {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22}, {3, 35, 11, 43, 1, 33, 9, 41},   {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},  {63, 31, 55, 23, 61, 29, 53, 21},
};

// Aspect-preserving fit of srcW x srcH within maxW x maxH, never upscaling
// past 1:1. 16.16 fixed point; outputs are always >= 1.
void fitWithin(const int srcW, const int srcH, const int maxW, const int maxH, int* outW, int* outH) {
  uint32_t scaleFp = 1u << 16;  // output pixels per source pixel, capped at 1.0
  const uint32_t scaleW = (static_cast<uint32_t>(maxW) << 16) / static_cast<uint32_t>(srcW);
  const uint32_t scaleH = (static_cast<uint32_t>(maxH) << 16) / static_cast<uint32_t>(srcH);
  if (scaleW < scaleFp) scaleFp = scaleW;
  if (scaleH < scaleFp) scaleFp = scaleH;
  *outW = static_cast<int>((static_cast<uint32_t>(srcW) * scaleFp) >> 16);
  *outH = static_cast<int>((static_cast<uint32_t>(srcH) * scaleFp) >> 16);
  if (*outW < 1) *outW = 1;
  if (*outH < 1) *outH = 1;
}

// Streams monotonically-increasing source rows in, box-samples them into
// target rows and writes each finished row (Bayer-dithered, bit-packed
// MSB-first, set bit = ink) straight to the output file.
struct ThumbWriter {
  HalFile* out = nullptr;
  int srcW = 0, srcH = 0;
  int outW = 0, outH = 0;
  int rowBytes = 0;
  uint32_t scaleXFp = 0, scaleYFp = 0;  // source pixels per output pixel, 16.16
  int currentOutY = 0;
  uint32_t nextOutBoundaryFp = 0;  // source-Y (16.16) that completes currentOutY
  std::unique_ptr<uint32_t[]> accum;  // per-output-column gray sums for the current band
  std::unique_ptr<uint32_t[]> count;  // matching sample counts
  uint8_t packed[kMaxRowBytes];

  // Allocates the accumulators and writes the 8-byte header. False on OOM or
  // a short write.
  bool init(HalFile* o, const int sw, const int sh, const int ow, const int oh) {
    out = o;
    srcW = sw;
    srcH = sh;
    outW = ow;
    outH = oh;
    rowBytes = (ow + 7) / 8;
    scaleXFp = (static_cast<uint32_t>(sw) << 16) / static_cast<uint32_t>(ow);
    scaleYFp = (static_cast<uint32_t>(sh) << 16) / static_cast<uint32_t>(oh);
    nextOutBoundaryFp = scaleYFp;
    accum = makeUniqueNoThrow<uint32_t[]>(ow);  // value-initialized (zeroed)
    count = makeUniqueNoThrow<uint32_t[]>(ow);
    if (!accum || !count) {
      LOG_ERR("CVR", "OOM: thumb accumulators (%d bytes)", ow * 8);
      return false;
    }
    const uint8_t hdr[kHeaderSize] = {
        static_cast<uint8_t>(kMagic & 0xFF),  static_cast<uint8_t>(kMagic >> 8),
        kVersion,                             0,
        static_cast<uint8_t>(ow & 0xFF),      static_cast<uint8_t>(ow >> 8),
        static_cast<uint8_t>(oh & 0xFF),      static_cast<uint8_t>(oh >> 8),
    };
    return out->write(hdr, kHeaderSize) == kHeaderSize;
  }

  // Dither + pack + write the current target row, then reset the accumulators.
  bool flushRow() {
    memset(packed, 0, rowBytes);
    for (int x = 0; x < outW; x++) {
      // No samples (rounding edge) reads as paper, not ink.
      const uint8_t gray = count[x] ? static_cast<uint8_t>(accum[x] / count[x]) : 255;
      const uint8_t threshold = kBayer8[currentOutY & 7][x & 7] * 4 + 2;
      if (gray < threshold) packed[x >> 3] |= 0x80 >> (x & 7);
    }
    if (out->write(packed, rowBytes) != static_cast<size_t>(rowBytes)) {
      LOG_ERR("CVR", "Short write on thumb row %d", currentOutY);
      return false;
    }
    currentOutY++;
    memset(accum.get(), 0, outW * sizeof(uint32_t));
    memset(count.get(), 0, outW * sizeof(uint32_t));
    return true;
  }

  // Box-sample one grayscale source row (srcW bytes) into the accumulators;
  // flushes target rows whose source band is complete. Rows past srcH or
  // after the last target row are tolerated and ignored.
  bool feedRow(const uint8_t* row, const int y) {
    if (y < 0 || y >= srcH || currentOutY >= outH) return true;
    for (int outX = 0; outX < outW; outX++) {
      const int srcXStart = static_cast<int>((static_cast<uint32_t>(outX) * scaleXFp) >> 16);
      const int srcXEnd = static_cast<int>((static_cast<uint32_t>(outX + 1) * scaleXFp) >> 16);
      uint32_t sum = 0;
      uint32_t n = 0;
      for (int srcX = srcXStart; srcX < srcXEnd && srcX < srcW; srcX++) {
        sum += row[srcX];
        n++;
      }
      if (n == 0 && srcXStart < srcW) {  // 1:1 columns (scale == 1.0) land here
        sum = row[srcXStart];
        n = 1;
      }
      accum[outX] += sum;
      count[outX] += n;
    }
    const uint32_t srcYFp = static_cast<uint32_t>(y + 1) << 16;
    while (srcYFp >= nextOutBoundaryFp && currentOutY < outH) {
      if (!flushRow()) return false;
      nextOutBoundaryFp = static_cast<uint32_t>(currentOutY + 1) * scaleYFp;
    }
    return true;
  }

  // Flush a trailing partially-accumulated row (fixed-point truncation can
  // leave the final target row pending) and pad to exactly outH rows.
  bool finish() {
    while (currentOutY < outH) {
      if (!flushRow()) return false;
    }
    return true;
  }
};

// --- JPEG (JPEGDEC) ---------------------------------------------------------

// JPEGDEC open-callback file pointer. Single-task use only (like the rest of
// the reader's SD access) — never touched concurrently.
HalFile* s_jpegFile = nullptr;

void* jpegOpenCb(const char* /*filename*/, int32_t* size) {
  if (!s_jpegFile || !*s_jpegFile) return nullptr;
  s_jpegFile->seek(0);
  *size = static_cast<int32_t>(s_jpegFile->size());
  return s_jpegFile;
}

void jpegCloseCb(void* /*handle*/) {
  // ensure() owns the file — nothing to do
}

int32_t jpegReadCb(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
  auto* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return 0;
  int32_t n = f->read(pBuf, len);
  if (n < 0) n = 0;
  pFile->iPos += n;
  return n;
}

int32_t jpegSeekCb(JPEGFILE* pFile, int32_t pos) {
  auto* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f || !f->seek(pos)) return -1;
  pFile->iPos = pos;
  return pos;
}

struct JpegThumbCtx {
  ThumbWriter writer;
  std::unique_ptr<uint8_t[]> mcuBuf;  // one MCU row band: kMaxMcuHeight x decodedW gray bytes
  bool error = false;
};

// Receives one MCU block at a time (left-to-right, top-to-bottom, coordinates
// in the scaled decode grid). Buffers the band; when the rightmost column
// lands, feeds the completed source rows to the writer.
int jpegDrawCb(JPEGDRAW* pDraw) {
  auto* ctx = static_cast<JpegThumbCtx*>(pDraw->pUser);
  if (!ctx || ctx->error) return 0;

  const uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);  // EIGHT_BIT_GRAYSCALE
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;
  const int blockX = pDraw->x;
  const int blockY = pDraw->y;
  const int srcW = ctx->writer.srcW;

  if (blockX < 0 || blockY < 0 || blockX >= srcW) {
    LOG_ERR("CVR", "Unexpected JPEG block origin (%d,%d) for grid %dx%d", blockX, blockY, srcW, ctx->writer.srcH);
    ctx->error = true;
    return 0;
  }

  for (int r = 0; r < blockH && r < kMaxMcuHeight; r++) {
    const int copyW = (blockX + validW <= srcW) ? validW : (srcW - blockX);
    if (copyW <= 0) continue;
    memcpy(ctx->mcuBuf.get() + r * srcW + blockX, pixels + r * stride, copyW);
  }

  if (blockX + validW < srcW) return 1;  // wait for the last MCU column of this band

  const int endRow = blockY + blockH;
  for (int y = blockY; y < endRow && y < ctx->writer.srcH; y++) {
    if (!ctx->writer.feedRow(ctx->mcuBuf.get() + (y - blockY) * srcW, y)) {
      ctx->error = true;
      return 0;
    }
  }
  return 1;
}

DecodeResult decodeJpegThumb(HalFile& imgFile, HalFile& binOut, const int maxW, const int maxH) {
  // JPEG asks only for its own ~21KB struct — never the ~58KB PNG union.
  uint8_t* scratch = acquireDecoderScratch(sizeof(JPEGDEC));
  if (!scratch) return DecodeResult::Transient;

  JPEGDEC* jpeg = new (scratch) JPEGDEC();  // placement-new into the shared scratch

  s_jpegFile = &imgFile;
  const ScopedCleanup cleanup{[jpeg] {
    jpeg->close();
    jpeg->~JPEGDEC();
    s_jpegFile = nullptr;
  }};

  if (jpeg->open("", jpegOpenCb, jpegCloseCb, jpegReadCb, jpegSeekCb, jpegDrawCb) != 1) {
    LOG_ERR("CVR", "JPEG open failed (err=%d)", jpeg->getLastError());
    return DecodeResult::Permanent;
  }

  const int srcW = jpeg->getWidth();
  const int srcH = jpeg->getHeight();
  if (srcW <= 0 || srcH <= 0 || srcW > kMaxSrcDim || srcH > kMaxSrcDim) {
    LOG_ERR("CVR", "JPEG cover too large or invalid (%dx%d)", srcW, srcH);
    return DecodeResult::Permanent;
  }

  int thumbW, thumbH;
  fitWithin(srcW, srcH, maxW, maxH, &thumbW, &thumbH);

  // Largest decode scale whose grid still covers the target (nearest-above).
  // JPEGDEC forces progressive streams to 1/8 internally regardless of the
  // options word, so their grid is fixed.
  int divisor = 1;
  int options = 0;
  const bool progressive = jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE;
  if (progressive) {
    divisor = 8;
  } else {
    struct Scale {
      int div;
      int opt;
    };
    constexpr Scale kScales[] = {{8, JPEG_SCALE_EIGHTH}, {4, JPEG_SCALE_QUARTER}, {2, JPEG_SCALE_HALF}};
    for (const Scale& s : kScales) {
      if (srcW / s.div >= thumbW && srcH / s.div >= thumbH) {
        divisor = s.div;
        options = s.opt;
        break;
      }
    }
  }
  const int decodedW = (srcW + divisor - 1) / divisor;
  const int decodedH = (srcH + divisor - 1) / divisor;
  if (decodedW > kMaxDecodedWidth) {
    LOG_ERR("CVR", "JPEG decode grid too wide (%d)", decodedW);
    return DecodeResult::Permanent;
  }

  // A forced-progressive 1/8 grid can undershoot the target; re-fit so the
  // thumb never upscales past the decoded pixels.
  int outW, outH;
  fitWithin(decodedW, decodedH, maxW, maxH, &outW, &outH);

  JpegThumbCtx ctx;
  ctx.mcuBuf = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(kMaxMcuHeight) * decodedW);
  if (!ctx.mcuBuf) {
    LOG_ERR("CVR", "OOM: MCU band (%d bytes)", kMaxMcuHeight * decodedW);
    return DecodeResult::Transient;
  }
  if (!ctx.writer.init(&binOut, decodedW, decodedH, outW, outH)) {
    return DecodeResult::Transient;
  }

  LOG_DBG("CVR", "JPEG %dx%d -> grid %dx%d (1/%d%s) -> thumb %dx%d", srcW, srcH, decodedW, decodedH, divisor,
          progressive ? ", progressive" : "", outW, outH);

  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&ctx);
  if (jpeg->decode(0, 0, options) != 1 || ctx.error) {
    LOG_ERR("CVR", "JPEG decode failed (err=%d)", jpeg->getLastError());
    return DecodeResult::Permanent;
  }
  return ctx.writer.finish() ? DecodeResult::Ok : DecodeResult::Transient;
}

// --- PNG (PNGdec) -----------------------------------------------------------

HalFile* s_pngFile = nullptr;

void* pngOpenCb(const char* /*filename*/, int32_t* size) {
  if (!s_pngFile || !*s_pngFile) return nullptr;
  s_pngFile->seek(0);
  *size = static_cast<int32_t>(s_pngFile->size());
  return s_pngFile;
}

void pngCloseCb(void* /*handle*/) {
  // ensure() owns the file — nothing to do
}

int32_t pngReadCb(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  auto* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return 0;
  const int n = f->read(pBuf, len);
  return n < 0 ? 0 : n;
}

int32_t pngSeekCb(PNGFILE* pFile, int32_t pos) {
  auto* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f || !f->seek(pos)) return -1;
  return pos;
}

int pngBytesPerPixel(const int pixelType) {
  switch (pixelType) {
    case PNG_PIXEL_TRUECOLOR:
      return 3;
    case PNG_PIXEL_GRAY_ALPHA:
      return 2;
    case PNG_PIXEL_TRUECOLOR_ALPHA:
      return 4;
    default:  // grayscale / indexed
      return 1;
  }
}

// 8-bit-per-stimulus scanline -> gray, alpha blended to white paper. Indexed
// PNGs with a tRNS chunk keep per-entry alpha at palette[768..].
// (Same conversion as x4-os PngToFramebufferConverter.)
void pngLineToGray(const uint8_t* pixels, uint8_t* grayLine, const int width, const int pixelType,
                   const uint8_t* palette, const int hasAlpha) {
  switch (pixelType) {
    case PNG_PIXEL_GRAYSCALE:
      memcpy(grayLine, pixels, width);
      break;
    case PNG_PIXEL_TRUECOLOR:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pixels[x * 3];
        grayLine[x] = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
      }
      break;
    case PNG_PIXEL_INDEXED:
      if (palette) {
        for (int x = 0; x < width; x++) {
          const uint8_t idx = pixels[x];
          const uint8_t* p = &palette[idx * 3];
          uint8_t gray = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          if (hasAlpha) {
            const uint8_t alpha = palette[768 + idx];
            gray = static_cast<uint8_t>((gray * alpha + 255 * (255 - alpha)) / 255);
          }
          grayLine[x] = gray;
        }
      } else {
        memcpy(grayLine, pixels, width);
      }
      break;
    case PNG_PIXEL_GRAY_ALPHA:
      for (int x = 0; x < width; x++) {
        const uint8_t gray = pixels[x * 2];
        const uint8_t alpha = pixels[x * 2 + 1];
        grayLine[x] = static_cast<uint8_t>((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;
    case PNG_PIXEL_TRUECOLOR_ALPHA:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pixels[x * 4];
        const uint8_t gray = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        const uint8_t alpha = p[3];
        grayLine[x] = static_cast<uint8_t>((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;
    default:
      memset(grayLine, 128, width);
      break;
  }
}

struct PngThumbCtx {
  ThumbWriter writer;
  std::unique_ptr<uint8_t[]> grayLine;  // srcW bytes
  bool error = false;
};

int pngDrawCb(PNGDRAW* pDraw) {
  auto* ctx = static_cast<PngThumbCtx*>(pDraw->pUser);
  if (!ctx || ctx->error) return 0;
  pngLineToGray(pDraw->pPixels, ctx->grayLine.get(), ctx->writer.srcW, pDraw->iPixelType, pDraw->pPalette,
                pDraw->iHasAlpha);
  if (!ctx->writer.feedRow(ctx->grayLine.get(), pDraw->y)) {
    ctx->error = true;
    return 0;
  }
  return 1;
}

DecodeResult decodePngThumb(HalFile& imgFile, HalFile& binOut, const int maxW, const int maxH) {
  // sizeof(PNG) is ~58KB (32KB inflate window + a 16416-byte scanline buffer
  // live inside the struct). Requesting that as a fresh contiguous block on the
  // fragmented BLE-active heap routinely fails — that is precisely why PNG-cover
  // books never rendered a cover. Run out of the retained shared scratch instead.
  uint8_t* scratch = acquireDecoderScratch(sizeof(PNG));
  if (!scratch) return DecodeResult::Transient;

  PNG* png = new (scratch) PNG();

  s_pngFile = &imgFile;
  const ScopedCleanup cleanup{[png] {
    png->close();
    png->~PNG();
    s_pngFile = nullptr;
  }};

  if (png->open("", pngOpenCb, pngCloseCb, pngReadCb, pngSeekCb, pngDrawCb) != PNG_SUCCESS) {
    LOG_ERR("CVR", "PNG open failed");
    return DecodeResult::Permanent;
  }

  const int srcW = png->getWidth();
  const int srcH = png->getHeight();
  const int pixelType = png->getPixelType();
  if (srcW <= 0 || srcH <= 0 || srcW > kMaxSrcDim || srcH > kMaxSrcDim) {
    LOG_ERR("CVR", "PNG cover too large or invalid (%dx%d)", srcW, srcH);
    return DecodeResult::Permanent;
  }
  if (png->isInterlaced()) {
    LOG_ERR("CVR", "Interlaced PNG covers are unsupported");
    return DecodeResult::Permanent;
  }
  if (png->getBpp() != 8) {
    LOG_ERR("CVR", "Unsupported PNG bit depth: %d", png->getBpp());
    return DecodeResult::Permanent;
  }
  // PNGdec keeps two filtered scanlines inside its fixed internal buffer; a
  // row wider than that overruns before the draw callback ever fires.
  const int requiredInternal = (srcW * pngBytesPerPixel(pixelType) + 1) * 2 + 32;
  if (requiredInternal > PNG_MAX_BUFFERED_PIXELS) {
    LOG_ERR("CVR", "PNG row too wide: need %d bytes, have %d", requiredInternal, PNG_MAX_BUFFERED_PIXELS);
    return DecodeResult::Permanent;
  }

  int outW, outH;
  fitWithin(srcW, srcH, maxW, maxH, &outW, &outH);

  PngThumbCtx ctx;
  ctx.grayLine = makeUniqueNoThrow<uint8_t[]>(srcW);
  if (!ctx.grayLine) {
    LOG_ERR("CVR", "OOM: PNG gray line (%d bytes)", srcW);
    return DecodeResult::Transient;
  }
  if (!ctx.writer.init(&binOut, srcW, srcH, outW, outH)) {
    return DecodeResult::Transient;
  }

  LOG_DBG("CVR", "PNG %dx%d (type %d) -> thumb %dx%d", srcW, srcH, pixelType, outW, outH);

  if (png->decode(&ctx, 0) != PNG_SUCCESS || ctx.error) {
    LOG_ERR("CVR", "PNG decode failed");
    return DecodeResult::Permanent;
  }
  return ctx.writer.finish() ? DecodeResult::Ok : DecodeResult::Transient;
}

// --- shared helpers ---------------------------------------------------------

bool hasSuffixCi(const std::string& s, const char* suffix) {
  const size_t n = strlen(suffix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++) {
    if (tolower(s[s.size() - n + i]) != suffix[i]) return false;
  }
  return true;
}

// Magic bytes win; the href extension only backs up an unreadable header.
ImgFormat sniffFormat(const std::string& imgPath, const std::string& coverHref) {
  uint8_t magic[8] = {0};
  {
    HalFile f;
    if (Storage.openFileForRead("CVR", imgPath, f)) {
      f.read(magic, sizeof(magic));
    }
  }
  if (magic[0] == 0xFF && magic[1] == 0xD8) return ImgFormat::Jpeg;
  if (magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47) return ImgFormat::Png;
  if (hasSuffixCi(coverHref, ".jpg") || hasSuffixCi(coverHref, ".jpeg")) return ImgFormat::Jpeg;
  if (hasSuffixCi(coverHref, ".png")) return ImgFormat::Png;
  return ImgFormat::Unknown;
}

// One-byte sentinel: "this book's cover failed to decode, don't retry".
void writeNoneSentinel(const std::string& nonePath) {
  HalFile f;
  if (!Storage.openFileForWrite("CVR", nonePath, f)) return;
  f.write(static_cast<uint8_t>(0));
  f.flush();
}

}  // namespace

int CoverThumb::probe(Epub& epub, const int w, const int h, std::string* outPath) {
  const std::string& cache = epub.getCachePath();
  std::string binPath = cache + "/cover_" + std::to_string(w) + "x" + std::to_string(h) + ".bin";
  if (Storage.exists(binPath.c_str())) {
    if (outPath) *outPath = std::move(binPath);
    return 1;
  }
  if (Storage.exists((cache + "/cover.none").c_str())) return 0;
  return -1;
}

bool CoverThumb::ensure(Epub& epub, const int w, const int h, std::string* outPath) {
  if (w <= 0 || h <= 0 || w > kMaxThumbW || h > kMaxThumbH) {
    LOG_ERR("CVR", "Bad thumb size %dx%d", w, h);
    return false;
  }

  const std::string& cache = epub.getCachePath();
  std::string binPath = cache + "/cover_" + std::to_string(w) + "x" + std::to_string(h) + ".bin";
  if (Storage.exists(binPath.c_str())) {
    if (outPath) *outPath = std::move(binPath);
    return true;
  }

  const std::string nonePath = cache + "/cover.none";
  if (Storage.exists(nonePath.c_str())) return false;

  std::string coverHref;
  if (!epub.getCoverHref(&coverHref)) return false;  // no cover: caller renders a text-only tile

  epub.setupCacheDir();

  // Extract the compressed cover to a temp SD file first — the decoders need
  // random access and a full file, and SD keeps the compressed image out of
  // the heap entirely.
  const std::string tmpImgPath = cache + "/cover.tmp";
  bool extracted = false;
  {
    HalFile tmpOut;
    if (Storage.openFileForWrite("CVR", tmpImgPath, tmpOut)) {
      extracted = epub.readItemContentsToStream(coverHref, tmpOut, 1024);
      tmpOut.flush();
    }
  }
  if (!extracted) {
    LOG_ERR("CVR", "Cover extract failed: %s", coverHref.c_str());
    Storage.remove(tmpImgPath.c_str());
    writeNoneSentinel(nonePath);  // broken/missing zip entry won't heal on retry
    return false;
  }

  const ImgFormat format = sniffFormat(tmpImgPath, coverHref);

  // Decode into a temp .bin, then move it into place (ProgressFile pattern:
  // SdFat rename won't overwrite, and a torn write must never look valid).
  const std::string tmpBinPath = cache + "/cover.tmp2";
  DecodeResult result = DecodeResult::Transient;
  {
    HalFile imgFile;
    HalFile binOut;
    if (Storage.openFileForRead("CVR", tmpImgPath, imgFile) && Storage.openFileForWrite("CVR", tmpBinPath, binOut)) {
      switch (format) {
        case ImgFormat::Jpeg:
          result = decodeJpegThumb(imgFile, binOut, w, h);
          break;
        case ImgFormat::Png:
          result = decodePngThumb(imgFile, binOut, w, h);
          break;
        case ImgFormat::Unknown:
          LOG_ERR("CVR", "Unrecognized cover format: %s", coverHref.c_str());
          result = DecodeResult::Permanent;
          break;
      }
      binOut.flush();
    }
  }
  Storage.remove(tmpImgPath.c_str());

  if (result != DecodeResult::Ok) {
    Storage.remove(tmpBinPath.c_str());
    if (result == DecodeResult::Permanent) writeNoneSentinel(nonePath);
    return false;
  }

  Storage.remove(binPath.c_str());
  if (!Storage.rename(tmpBinPath.c_str(), binPath.c_str())) {
    LOG_ERR("CVR", "Failed to move thumb into place: %s", binPath.c_str());
    Storage.remove(tmpBinPath.c_str());
    return false;
  }

  if (outPath) *outPath = std::move(binPath);
  return true;
}

void CoverThumb::preacquireScratch() {
  // Union of both decoder structs — grow-only acquire keeps this block for
  // every later per-format request in the same grid session.
  g_scratchFailedNeed = 0;  // fresh scene, fresh heap — allow one new attempt
  acquireDecoderScratch(sizeof(PNG) > sizeof(JPEGDEC) ? sizeof(PNG) : sizeof(JPEGDEC));
}

void CoverThumb::releaseScratch() {
  g_scratchFailedNeed = 0;
  if (g_decoderScratch) {
    free(g_decoderScratch);
    g_decoderScratch = nullptr;
    g_decoderScratchSize = 0;
  }
}

bool CoverThumb::draw(Gfx& gfx, const char* binPath, const int x, const int y, int* outW, int* outH) {
  HalFile f;
  if (!Storage.openFileForRead("CVR", binPath, f)) return false;

  uint8_t hdr[kHeaderSize];
  if (f.read(hdr, kHeaderSize) != kHeaderSize) return false;
  const uint16_t magic = static_cast<uint16_t>(hdr[0] | (hdr[1] << 8));
  const int w = hdr[4] | (hdr[5] << 8);
  const int h = hdr[6] | (hdr[7] << 8);
  if (magic != kMagic || hdr[2] != kVersion || w <= 0 || w > kMaxThumbW || h <= 0 || h > kMaxThumbH) {
    LOG_ERR("CVR", "Bad thumb header: %s", binPath);
    return false;
  }

  const int rowBytes = (w + 7) / 8;
  uint8_t row[kMaxRowBytes];  // <= 66 bytes on the stack
  for (int r = 0; r < h; r++) {
    if (f.read(row, rowBytes) != rowBytes) {
      LOG_ERR("CVR", "Truncated thumb at row %d: %s", r, binPath);
      return false;
    }
    for (int b = 0; b < rowBytes; b++) {
      const uint8_t byte = row[b];
      if (!byte) continue;  // fast-skip all-paper bytes
      const int colBase = b << 3;
      for (int bit = 0; bit < 8; bit++) {
        if (byte & (0x80 >> bit)) gfx.drawPixel(x + colBase + bit, y + r, true);
      }
    }
  }

  if (outW) *outW = w;
  if (outH) *outH = h;
  return true;
}

}  // namespace reader
