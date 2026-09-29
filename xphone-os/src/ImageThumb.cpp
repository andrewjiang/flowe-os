// Shared JPEG/PNG decode/dither pipeline — see ImageThumb.h. Extracted from
// reader/CoverThumb.cpp (the R2a cover thumbnail path), which now calls into
// this file instead of keeping its own copy; BackgroundImage.cpp is the
// second caller, decoding a bare SD-root file instead of an EPUB-extracted
// cover. Nothing here is EPUB-specific — callers own file extraction, cache
// paths, and any not-found/sentinel bookkeeping.
//
// Decode strategy (both formats stream, nothing holds a full image in RAM):
//  - JPEG (JPEGDEC): pick the largest 1/2 / 1/4 / 1/8 decode scale whose grid
//    still covers the target, buffer one MCU row band (16 x decodedW bytes,
//    the x4-os JpegToBmpConverter approach), then box-sample completed source
//    rows into the target row accumulators.
//  - PNG (PNGdec): scanlines arrive top-to-bottom via PNG_DRAW; each line is
//    converted to 8-bit gray and box-sampled the same way.
// Gray target rows stage to a small SD temp as they complete; finish() then
// runs the tone pass (auto-contrast + unsharp, bookc parity) and Floyd-
// Steinberg into the output, so RAM stays a few row buffers regardless of
// source size.
#include "ImageThumb.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Memory.h>
#include <PNGdec.h>

#include <cctype>
#include <cstring>
#include <new>

#include "Gfx.h"

namespace imagethumb {
namespace {

// One persistent scratch block, sized to the larger of the two decoders,
// shared by the JPEG (~21KB) and PNG (~58KB) paths — they never run
// concurrently (one image at a time). Allocated lazily on first decode and
// RETAINED by default so a caller doing many decodes back-to-back (the
// Reader's cover grid) doesn't re-request a 58KB contiguous block that a
// BLE-active fragmented heap routinely can't satisfy. releaseScratch() frees
// it for callers (BackgroundImage) that only ever decode once per session.
uint8_t* g_decoderScratch = nullptr;
size_t g_decoderScratchSize = 0;

// Smallest ask that already failed this session. Fragmentation doesn't heal
// between quiet ticks, so retrying an equal-or-bigger ask every pass just
// burns time re-extracting the image before failing again. Reset by
// preacquireScratch()/releaseScratch().
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
      Serial.printf("[xphone-os] img: decoder scratch alloc failed (need %u, free %u, largest %u)\n",
                    static_cast<unsigned>(need), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    }
  }
  return g_decoderScratch;
}

constexpr uint16_t kMagic = 0x5854;  // 'XT'
constexpr uint8_t kVersion = 1;
constexpr int kHeaderSize = 8;
constexpr int kMaxRowBytes = (kMaxThumbW + 7) / 8;
constexpr int kMaxSrcDim = 4096;        // reject absurd source images outright
constexpr int kMaxDecodedWidth = 2048;  // caps the JPEG MCU band at 16 * 2048 = 32KB
constexpr int kMaxMcuHeight = 16;       // tallest JPEG MCU (4:2:0 chroma)

}  // namespace

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

namespace {

// Streams monotonically-increasing source rows in, box-samples them into
// GRAY target rows staged in a temp file, then finish() runs the tone pass
// (auto-contrast + gentle unsharp, mirroring bookc's punch_gray) and
// Floyd-Steinberg dither into the output file (bit-packed MSB-first, set
// bit = ink). Staging costs a <=52 KB temp on SD and ~2.5 KB of RAM for the
// second pass; the decode itself still streams in bands.
struct ThumbWriter {
  // Single-task use, like every SD path in the reader; the file is deleted
  // in finish() on every outcome. Shared by both callers (cover + background)
  // since they never decode concurrently either.
  static constexpr const char* kGrayTmpPath = "/.xphone/img.gray.tmp";

  HalFile* out = nullptr;
  HalFile tmp;
  int srcW = 0, srcH = 0;
  int outW = 0, outH = 0;
  int rowBytes = 0;
  uint32_t scaleXFp = 0, scaleYFp = 0;  // source pixels per output pixel, 16.16
  int currentOutY = 0;
  uint32_t nextOutBoundaryFp = 0;  // source-Y (16.16) that completes currentOutY
  std::unique_ptr<uint32_t[]> accum;  // per-output-column gray sums for the current band
  std::unique_ptr<uint32_t[]> count;  // matching sample counts
  uint8_t packed[kMaxRowBytes];
  uint8_t grayRow[kMaxRowBytes * 8];

  // Allocates the accumulators, opens the gray staging file, and writes the
  // 8-byte header to the output. False on OOM or a short write.
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
      Serial.printf("[xphone-os] img: OOM thumb accumulators (%d bytes)\n", ow * 8);
      return false;
    }
    if (!Storage.openFileForWrite("IMG", kGrayTmpPath, tmp)) {
      Serial.println("[xphone-os] img: gray staging open failed");
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

  // Average + write the current GRAY target row to the staging file, then
  // reset the accumulators. Dithering waits for finish(), which knows the
  // whole image's tone.
  bool flushRow() {
    for (int x = 0; x < outW; x++) {
      // No samples (rounding edge) reads as paper, not ink.
      grayRow[x] = count[x] ? static_cast<uint8_t>(accum[x] / count[x]) : 255;
    }
    if (tmp.write(grayRow, outW) != static_cast<size_t>(outW)) {
      Serial.printf("[xphone-os] img: short write on gray row %d\n", currentOutY);
      return false;
    }
    currentOutY++;
    memset(accum.get(), 0, outW * sizeof(uint32_t));
    memset(count.get(), 0, outW * sizeof(uint32_t));
    return true;
  }

  // The tone pass: histogram over the staged gray, stretch the 2nd..98th
  // percentile to full range (skipped when already narrow — line art and
  // solid images stay untouched), a 0.5-amount 3x3 unsharp, then Floyd-
  // Steinberg into the output.
  bool tonePass() {
    if (!Storage.openFileForRead("IMG", kGrayTmpPath, tmp)) return false;
    uint32_t hist[256] = {0};
    for (int y = 0; y < outH; y++) {
      if (tmp.read(grayRow, outW) != static_cast<size_t>(outW)) return false;
      for (int x = 0; x < outW; x++) hist[grayRow[x]]++;
    }
    const uint32_t n = static_cast<uint32_t>(outW) * static_cast<uint32_t>(outH);
    const uint32_t need = n / 50; /* 2% */
    uint32_t acc = 0;
    int lo = 0, hi = 255;
    for (int v = 0; v < 256; v++) {
      acc += hist[v];
      if (acc > need) {
        lo = v;
        break;
      }
    }
    acc = 0;
    for (int v = 255; v >= 0; v--) {
      acc += hist[v];
      if (acc > need) {
        hi = v;
        break;
      }
    }
    uint8_t lut[256];
    for (int v = 0; v < 256; v++) {
      if (hi - lo < 32) {
        lut[v] = static_cast<uint8_t>(v);
      } else {
        int s = (v - lo) * 255 / (hi - lo);
        lut[v] = static_cast<uint8_t>(s < 0 ? 0 : s > 255 ? 255 : s);
      }
    }

    // Rolling three LUT'd rows for the unsharp, two error rows for FS.
    auto rows = makeUniqueNoThrow<uint8_t[]>(3 * outW);
    auto err = makeUniqueNoThrow<int16_t[]>(2 * (outW + 2));
    if (!rows || !err) return false;
    memset(err.get(), 0, 2 * (outW + 2) * sizeof(int16_t));
    uint8_t* r[3] = {rows.get(), rows.get() + outW, rows.get() + 2 * outW};
    int16_t* cur = err.get() + 1;
    int16_t* next = err.get() + (outW + 2) + 1;

    if (!tmp.seek(0)) return false;  // the staging file is raw rows, no header
    auto loadRow = [&](uint8_t* dst) -> bool {
      if (tmp.read(grayRow, outW) != static_cast<size_t>(outW)) return false;
      for (int x = 0; x < outW; x++) dst[x] = lut[grayRow[x]];
      return true;
    };
    if (!loadRow(r[0])) return false;
    memcpy(r[1], r[0], outW);  // virtual row above the top edge

    for (int y = 0; y < outH; y++) {
      // r[1] = row y (LUT'd), r[0] = y-1, r[2] = y+1 (edge rows repeat).
      uint8_t* above = r[0];
      uint8_t* mid = r[1];
      uint8_t* below = r[2];
      if (y + 1 < outH) {
        if (!loadRow(below)) return false;
      } else {
        memcpy(below, mid, outW);
      }

      memset(next - 1, 0, (outW + 2) * sizeof(int16_t));
      memset(packed, 0, rowBytes);
      for (int x = 0; x < outW; x++) {
        const int xl = x > 0 ? x - 1 : 0;
        const int xr = x + 1 < outW ? x + 1 : outW - 1;
        const int blur = (above[xl] + above[x] + above[xr] + mid[xl] + mid[x] + mid[xr] +
                          below[xl] + below[x] + below[xr]) /
                         9;
        int v = mid[x] + (mid[x] - blur) / 2;
        v = v < 0 ? 0 : v > 255 ? 255 : v;
        v += cur[x];
        const int on = v < 128;
        const int e = v - (on ? 0 : 255);
        if (on) packed[x >> 3] |= static_cast<uint8_t>(0x80 >> (x & 7));
        cur[x + 1] += static_cast<int16_t>(e * 7 / 16);
        next[x - 1] += static_cast<int16_t>(e * 3 / 16);
        next[x] += static_cast<int16_t>(e * 5 / 16);
        next[x + 1] += static_cast<int16_t>(e * 1 / 16);
      }
      if (out->write(packed, rowBytes) != static_cast<size_t>(rowBytes)) {
        Serial.printf("[xphone-os] img: short write on thumb row %d\n", y);
        return false;
      }
      int16_t* t = cur;
      cur = next;
      next = t;
      uint8_t* rt = above;
      r[0] = mid;
      r[1] = below;
      r[2] = rt;
    }
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
  // leave the final target row pending), pad to exactly outH gray rows,
  // then run the tone pass into the output. The staging file dies here on
  // every outcome.
  bool finish() {
    while (currentOutY < outH) {
      if (!flushRow()) return false;
    }
    tmp.flush();
    tmp.close();
    const bool ok = tonePass();
    tmp.close();
    Storage.remove(kGrayTmpPath);
    return ok;
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
  // caller owns the file — nothing to do
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
    Serial.printf("[xphone-os] img: unexpected JPEG block origin (%d,%d) for grid %dx%d\n", blockX, blockY, srcW,
                  ctx->writer.srcH);
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

// --- PNG (PNGdec) -----------------------------------------------------------

HalFile* s_pngFile = nullptr;

void* pngOpenCb(const char* /*filename*/, int32_t* size) {
  if (!s_pngFile || !*s_pngFile) return nullptr;
  s_pngFile->seek(0);
  *size = static_cast<int32_t>(s_pngFile->size());
  return s_pngFile;
}

void pngCloseCb(void* /*handle*/) {
  // caller owns the file — nothing to do
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

// --- shared helpers ---------------------------------------------------------

bool hasSuffixCi(const std::string& s, const char* suffix) {
  const size_t n = strlen(suffix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++) {
    if (tolower(s[s.size() - n + i]) != suffix[i]) return false;
  }
  return true;
}

}  // namespace

// Magic bytes win; `hintPath` only backs up an unreadable header.
ImgFormat sniffFormat(const std::string& imgPath, const std::string& hintPath) {
  uint8_t magic[8] = {0};
  {
    HalFile f;
    if (Storage.openFileForRead("IMG", imgPath, f)) {
      f.read(magic, sizeof(magic));
    }
  }
  if (magic[0] == 0xFF && magic[1] == 0xD8) return ImgFormat::Jpeg;
  if (magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47) return ImgFormat::Png;
  if (magic[0] == 'B' && magic[1] == 'M') return ImgFormat::Bmp;
  if (hasSuffixCi(hintPath, ".jpg") || hasSuffixCi(hintPath, ".jpeg")) return ImgFormat::Jpeg;
  if (hasSuffixCi(hintPath, ".png")) return ImgFormat::Png;
  if (hasSuffixCi(hintPath, ".bmp")) return ImgFormat::Bmp;
  return ImgFormat::Unknown;
}

namespace {
uint16_t readU16LE(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t readU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
}  // namespace

// Uncompressed BMP (BI_RGB, 8/24/32 bpp). Rows are read straight off SD one
// at a time — no full-image buffer, no shared decoder scratch (unlike
// JPEG/PNG, a BMP row IS raw pixels; there's no entropy coding to unpack).
// Bottom-up files (the common case, height > 0 in the header) are fed to the
// writer top-to-bottom by seeking each row in reverse file order — feedRow's
// box-sampling accumulator requires monotonically increasing image-space y,
// which is the opposite of a bottom-up file's physical row order.
DecodeResult decodeBmpThumb(HalFile& imgFile, HalFile& binOut, const int maxW, const int maxH) {
  uint8_t fileHdr[14];
  if (!imgFile.seek(0) || imgFile.read(fileHdr, sizeof(fileHdr)) != sizeof(fileHdr) || fileHdr[0] != 'B' ||
      fileHdr[1] != 'M') {
    Serial.println("[xphone-os] img: bad BMP signature");
    return DecodeResult::Permanent;
  }
  const uint32_t pixelDataOffset = readU32LE(fileHdr + 10);

  uint8_t dibHdr[40];
  if (imgFile.read(dibHdr, sizeof(dibHdr)) != sizeof(dibHdr)) return DecodeResult::Permanent;
  const uint32_t dibHdrSize = readU32LE(dibHdr + 0);
  if (dibHdrSize < 40) {
    Serial.printf("[xphone-os] img: unsupported BMP DIB header size %u\n", static_cast<unsigned>(dibHdrSize));
    return DecodeResult::Permanent;
  }
  const int32_t rawW = static_cast<int32_t>(readU32LE(dibHdr + 4));
  const int32_t rawH = static_cast<int32_t>(readU32LE(dibHdr + 8));
  const uint16_t planes = readU16LE(dibHdr + 12);
  const uint16_t bpp = readU16LE(dibHdr + 14);
  const uint32_t compression = readU32LE(dibHdr + 16);
  uint32_t colorsUsed = readU32LE(dibHdr + 32);

  if (planes != 1 || compression != 0 /* BI_RGB: no RLE, no bitfields */) {
    Serial.printf("[xphone-os] img: unsupported BMP (planes=%u compression=%u)\n", planes,
                  static_cast<unsigned>(compression));
    return DecodeResult::Permanent;
  }
  // 1 and 4 bpp matter here: a wallpaper prepared by hand for a 1-bit e-ink
  // panel is usually saved exactly that way.
  if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) {
    Serial.printf("[xphone-os] img: unsupported BMP bit depth %u\n", bpp);
    return DecodeResult::Permanent;
  }

  const bool topDown = rawH < 0;
  const int srcW = rawW;
  const int srcH = topDown ? -rawH : rawH;
  if (srcW <= 0 || srcH <= 0 || srcW > kMaxSrcDim || srcH > kMaxSrcDim) {
    Serial.printf("[xphone-os] img: BMP too large or invalid (%dx%d)\n", srcW, srcH);
    return DecodeResult::Permanent;
  }

  // Indexed depths carry a palette right after the DIB header, 4 bytes per
  // entry (B,G,R,reserved). Only the gray value of each entry is ever needed,
  // so collapse to one byte per entry as it is read — a full BGRA copy would
  // put 1KB on the loop task's stack, which runs close to its limit.
  uint8_t palGray[256] = {0};
  if (bpp <= 8) {
    const uint32_t maxEntries = 1u << bpp;
    if (colorsUsed == 0 || colorsUsed > maxEntries) colorsUsed = maxEntries;
    if (!imgFile.seek(14 + dibHdrSize)) {
      Serial.println("[xphone-os] img: BMP palette seek failed");
      return DecodeResult::Permanent;
    }
    uint8_t entries[16 * 4];
    for (uint32_t done = 0; done < colorsUsed;) {
      const uint32_t batch = (colorsUsed - done > 16) ? 16 : (colorsUsed - done);
      if (imgFile.read(entries, batch * 4) != static_cast<int>(batch * 4)) {
        Serial.println("[xphone-os] img: BMP palette read failed");
        return DecodeResult::Permanent;
      }
      for (uint32_t i = 0; i < batch; i++) {
        const uint8_t* p = &entries[i * 4];  // B,G,R,reserved
        palGray[done + i] = static_cast<uint8_t>((p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8);
      }
      done += batch;
    }
  }

  int outW, outH;
  fitWithin(srcW, srcH, maxW, maxH, &outW, &outH);

  ThumbWriter writer;
  if (!writer.init(&binOut, srcW, srcH, outW, outH)) return DecodeResult::Transient;

  const int bytesPerPixel = bpp / 8;
  const uint32_t rowStride = ((static_cast<uint32_t>(srcW) * bpp + 31) / 32) * 4;
  auto rowBuf = makeUniqueNoThrow<uint8_t[]>(rowStride);
  auto grayLine = makeUniqueNoThrow<uint8_t[]>(srcW);
  if (!rowBuf || !grayLine) {
    Serial.println("[xphone-os] img: OOM BMP row buffer");
    return DecodeResult::Transient;
  }

  Serial.printf("[xphone-os] img: BMP %dx%d (%u bpp%s) -> thumb %dx%d\n", srcW, srcH, bpp,
                topDown ? ", top-down" : ", bottom-up", outW, outH);

  for (int y = 0; y < srcH; y++) {
    const int fileRow = topDown ? y : (srcH - 1 - y);
    if (!imgFile.seek(pixelDataOffset + static_cast<uint32_t>(fileRow) * rowStride) ||
        imgFile.read(rowBuf.get(), rowStride) != static_cast<int>(rowStride)) {
      Serial.printf("[xphone-os] img: BMP short read at row %d\n", y);
      Storage.remove(ThumbWriter::kGrayTmpPath);  // init() opened it; finish() won't run
      return DecodeResult::Permanent;
    }
    for (int x = 0; x < srcW; x++) {
      switch (bpp) {
        case 1:
          grayLine[x] = palGray[(rowBuf[x >> 3] >> (7 - (x & 7))) & 1];
          break;
        case 4:
          grayLine[x] = palGray[(x & 1) ? (rowBuf[x >> 1] & 0x0F) : (rowBuf[x >> 1] >> 4)];
          break;
        case 8:
          grayLine[x] = palGray[rowBuf[x]];
          break;
        default: {  // 24 / 32 bpp: B,G,R[,A]
          const uint8_t* p = rowBuf.get() + x * bytesPerPixel;
          grayLine[x] = static_cast<uint8_t>((p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8);
          break;
        }
      }
    }
    if (!writer.feedRow(grayLine.get(), y)) {
      Storage.remove(ThumbWriter::kGrayTmpPath);
      return DecodeResult::Transient;
    }
  }
  return writer.finish() ? DecodeResult::Ok : DecodeResult::Transient;
}

DecodeResult decodeJpegThumb(HalFile& imgFile, HalFile& binOut, const int maxW, const int maxH) {
  // JPEG asks only for its own ~21KB struct — never the ~58KB PNG union.
  uint8_t* scratch = acquireDecoderScratch(sizeof(JPEGDEC));
  if (!scratch) return DecodeResult::Transient;

  JPEGDEC* jpeg = new (scratch) JPEGDEC();  // placement-new into the shared scratch

  s_jpegFile = &imgFile;
  struct Cleanup {
    JPEGDEC* j;
    ~Cleanup() {
      j->close();
      j->~JPEGDEC();
      s_jpegFile = nullptr;
    }
  } cleanup{jpeg};

  if (jpeg->open("", jpegOpenCb, jpegCloseCb, jpegReadCb, jpegSeekCb, jpegDrawCb) != 1) {
    Serial.printf("[xphone-os] img: JPEG open failed (err=%d)\n", jpeg->getLastError());
    return DecodeResult::Permanent;
  }

  const int srcW = jpeg->getWidth();
  const int srcH = jpeg->getHeight();
  if (srcW <= 0 || srcH <= 0 || srcW > kMaxSrcDim || srcH > kMaxSrcDim) {
    Serial.printf("[xphone-os] img: JPEG too large or invalid (%dx%d)\n", srcW, srcH);
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
    Serial.printf("[xphone-os] img: JPEG decode grid too wide (%d)\n", decodedW);
    return DecodeResult::Permanent;
  }

  // A forced-progressive 1/8 grid can undershoot the target; re-fit so the
  // thumb never upscales past the decoded pixels.
  int outW, outH;
  fitWithin(decodedW, decodedH, maxW, maxH, &outW, &outH);

  JpegThumbCtx ctx;
  ctx.mcuBuf = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(kMaxMcuHeight) * decodedW);
  if (!ctx.mcuBuf) {
    Serial.printf("[xphone-os] img: OOM MCU band (%d bytes)\n", kMaxMcuHeight * decodedW);
    return DecodeResult::Transient;
  }
  if (!ctx.writer.init(&binOut, decodedW, decodedH, outW, outH)) {
    return DecodeResult::Transient;
  }

  Serial.printf("[xphone-os] img: JPEG %dx%d -> grid %dx%d (1/%d%s) -> thumb %dx%d\n", srcW, srcH, decodedW, decodedH,
                divisor, progressive ? ", progressive" : "", outW, outH);

  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&ctx);
  if (jpeg->decode(0, 0, options) != 1 || ctx.error) {
    Serial.printf("[xphone-os] img: JPEG decode failed (err=%d)\n", jpeg->getLastError());
    return DecodeResult::Permanent;
  }
  return ctx.writer.finish() ? DecodeResult::Ok : DecodeResult::Transient;
}

DecodeResult decodePngThumb(HalFile& imgFile, HalFile& binOut, const int maxW, const int maxH) {
  // sizeof(PNG) is ~58KB (32KB inflate window + a 16416-byte scanline buffer
  // live inside the struct). Requesting that as a fresh contiguous block on
  // the fragmented BLE-active heap routinely fails. Run out of the retained
  // shared scratch instead.
  uint8_t* scratch = acquireDecoderScratch(sizeof(PNG));
  if (!scratch) return DecodeResult::Transient;

  PNG* png = new (scratch) PNG();

  s_pngFile = &imgFile;
  struct Cleanup {
    PNG* p;
    ~Cleanup() {
      p->close();
      p->~PNG();
      s_pngFile = nullptr;
    }
  } cleanup{png};

  if (png->open("", pngOpenCb, pngCloseCb, pngReadCb, pngSeekCb, pngDrawCb) != PNG_SUCCESS) {
    Serial.println("[xphone-os] img: PNG open failed");
    return DecodeResult::Permanent;
  }

  const int srcW = png->getWidth();
  const int srcH = png->getHeight();
  const int pixelType = png->getPixelType();
  if (srcW <= 0 || srcH <= 0 || srcW > kMaxSrcDim || srcH > kMaxSrcDim) {
    Serial.printf("[xphone-os] img: PNG too large or invalid (%dx%d)\n", srcW, srcH);
    return DecodeResult::Permanent;
  }
  if (png->isInterlaced()) {
    Serial.println("[xphone-os] img: interlaced PNGs are unsupported");
    return DecodeResult::Permanent;
  }
  if (png->getBpp() != 8) {
    Serial.printf("[xphone-os] img: unsupported PNG bit depth: %d\n", png->getBpp());
    return DecodeResult::Permanent;
  }
  // PNGdec keeps two filtered scanlines inside its fixed internal buffer; a
  // row wider than that overruns before the draw callback ever fires.
  const int requiredInternal = (srcW * pngBytesPerPixel(pixelType) + 1) * 2 + 32;
  if (requiredInternal > PNG_MAX_BUFFERED_PIXELS) {
    Serial.printf("[xphone-os] img: PNG row too wide: need %d bytes, have %d\n", requiredInternal,
                  PNG_MAX_BUFFERED_PIXELS);
    return DecodeResult::Permanent;
  }

  int outW, outH;
  fitWithin(srcW, srcH, maxW, maxH, &outW, &outH);

  PngThumbCtx ctx;
  ctx.grayLine = makeUniqueNoThrow<uint8_t[]>(srcW);
  if (!ctx.grayLine) {
    Serial.printf("[xphone-os] img: OOM PNG gray line (%d bytes)\n", srcW);
    return DecodeResult::Transient;
  }
  if (!ctx.writer.init(&binOut, srcW, srcH, outW, outH)) {
    return DecodeResult::Transient;
  }

  Serial.printf("[xphone-os] img: PNG %dx%d (type %d) -> thumb %dx%d\n", srcW, srcH, pixelType, outW, outH);

  if (png->decode(&ctx, 0) != PNG_SUCCESS || ctx.error) {
    Serial.println("[xphone-os] img: PNG decode failed");
    return DecodeResult::Permanent;
  }
  return ctx.writer.finish() ? DecodeResult::Ok : DecodeResult::Transient;
}

void preacquireScratch() {
  g_scratchFailedNeed = 0;  // fresh scene, fresh heap — allow one new attempt
  acquireDecoderScratch(sizeof(JPEGDEC));
}

void releaseScratch() {
  g_scratchFailedNeed = 0;
  if (g_decoderScratch) {
    free(g_decoderScratch);
    g_decoderScratch = nullptr;
    g_decoderScratchSize = 0;
  }
}

namespace {
// Shared by blit() and peekSize(): parse + validate the 8-byte header of an
// already-open thumb/.bin file.
bool readValidHeader(HalFile& f, int* w, int* h) {
  uint8_t hdr[kHeaderSize];
  if (f.read(hdr, kHeaderSize) != kHeaderSize) return false;
  const uint16_t magic = static_cast<uint16_t>(hdr[0] | (hdr[1] << 8));
  *w = hdr[4] | (hdr[5] << 8);
  *h = hdr[6] | (hdr[7] << 8);
  return magic == kMagic && hdr[2] == kVersion && *w > 0 && *w <= kMaxThumbW && *h > 0 && *h <= kMaxThumbH;
}
}  // namespace

bool peekSize(const char* binPath, int* outW, int* outH) {
  HalFile f;
  if (!Storage.openFileForRead("IMG", binPath, f)) return false;
  int w = 0, h = 0;
  if (!readValidHeader(f, &w, &h)) return false;
  if (outW) *outW = w;
  if (outH) *outH = h;
  return true;
}

bool blit(Gfx& gfx, const char* binPath, const int x, const int y, int* outW, int* outH, const int clipX0,
          const int clipX1) {
  HalFile f;
  if (!Storage.openFileForRead("IMG", binPath, f)) return false;

  int w = 0, h = 0;
  if (!readValidHeader(f, &w, &h)) {
    Serial.printf("[xphone-os] img: bad thumb header: %s\n", binPath);
    return false;
  }

  const int rowBytes = (w + 7) / 8;
  uint8_t row[kMaxRowBytes];  // <= 66 bytes on the stack
  for (int r = 0; r < h; r++) {
    if (f.read(row, rowBytes) != rowBytes) {
      Serial.printf("[xphone-os] img: truncated thumb at row %d: %s\n", r, binPath);
      return false;
    }
    for (int b = 0; b < rowBytes; b++) {
      const uint8_t byte = row[b];
      if (!byte) continue;  // fast-skip all-paper bytes
      const int colBase = b << 3;
      for (int bit = 0; bit < 8; bit++) {
        if (!(byte & (0x80 >> bit))) continue;
        const int px = x + colBase + bit;
        if (px < clipX0 || px >= clipX1) continue;
        gfx.drawPixel(px, y + r, true);
      }
    }
  }

  if (outW) *outW = w;
  if (outH) *outH = h;
  return true;
}

}  // namespace imagethumb
