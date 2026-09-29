// R2a cover thumbnail pipeline — see CoverThumb.h for the .bin format. The
// actual JPEG/PNG decode/dither/blit pipeline lives in ../ImageThumb.{h,cpp}
// (shared with BackgroundImage, the nap-screen wallpaper feature); this file
// is now just the EPUB-specific plumbing: extracting the cover image from
// the zip, the cover-cache path, and the "this book has no cover" sentinel.
#include "CoverThumb.h"

#include <Arduino.h>
#include <HalStorage.h>

#include "../ImageThumb.h"
#include "Epub.h"
#include "ReaderLog.h"

namespace reader {
namespace {

// One-byte sentinel: "this book's cover failed to decode, don't retry".
// The byte is a VERSION ('2'). Sentinels written before the 0.3.x decoder
// fixes (empty files or a 0x00 byte) were often set by failures that are now
// fixed — the X3 'Elon book' cover survived its bugfix because a stale
// sentinel kept suppressing the retry. A non-current sentinel is deleted so
// the decode runs once more under the fixed pipeline; genuinely broken images
// re-sentinel with the current version.
constexpr uint8_t kNoneSentinelVersion = '2';

void writeNoneSentinel(const std::string& nonePath) {
  HalFile f;
  if (!Storage.openFileForWrite("CVR", nonePath, f)) return;
  f.write(kNoneSentinelVersion);
  f.flush();
}

// True only for a current-version sentinel; legacy ones are removed en route.
bool noneSentinelCurrent(const std::string& nonePath) {
  if (!Storage.exists(nonePath.c_str())) return false;
  uint8_t v = 0;
  {
    HalFile f;
    if (Storage.openFileForRead("CVR", nonePath, f) && f.read(&v, 1) == 1 &&
        v == kNoneSentinelVersion) {
      return true;
    }
  }
  Storage.remove(nonePath.c_str());
  return false;
}

}  // namespace

int CoverThumb::probe(Epub& epub, const int w, const int h, std::string* outPath) {
  const std::string& cache = epub.getCachePath();
  std::string binPath = cache + "/cover_" + std::to_string(w) + "x" + std::to_string(h) + ".bin";
  if (Storage.exists(binPath.c_str())) {
    if (outPath) *outPath = std::move(binPath);
    return 1;
  }
  if (noneSentinelCurrent(cache + "/cover.none")) return 0;
  return -1;
}

bool CoverThumb::ensure(Epub& epub, const int w, const int h, std::string* outPath) {
  if (w <= 0 || h <= 0 || w > imagethumb::kMaxThumbW || h > imagethumb::kMaxThumbH) {
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
  if (noneSentinelCurrent(nonePath)) return false;

  std::string coverHref;
  if (!epub.getCoverHref(&coverHref)) {
    // "The OPF names no cover" is as definitive as a broken image — persist
    // it, or every future shelf pass re-answers -1 (transient) and the tile
    // never settles past "..." (audit I3/I8 tail, 2026-08-18).
    epub.setupCacheDir();
    writeNoneSentinel(nonePath);
    return false;  // caller renders a text-only tile
  }

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

  const imagethumb::ImgFormat format = imagethumb::sniffFormat(tmpImgPath, coverHref);

  // Decode into a temp .bin, then move it into place (ProgressFile pattern:
  // SdFat rename won't overwrite, and a torn write must never look valid).
  const std::string tmpBinPath = cache + "/cover.tmp2";
  imagethumb::DecodeResult result = imagethumb::DecodeResult::Transient;
  {
    HalFile imgFile;
    HalFile binOut;
    if (Storage.openFileForRead("CVR", tmpImgPath, imgFile) && Storage.openFileForWrite("CVR", tmpBinPath, binOut)) {
      switch (format) {
        case imagethumb::ImgFormat::Jpeg:
          result = imagethumb::decodeJpegThumb(imgFile, binOut, w, h);
          break;
        case imagethumb::ImgFormat::Png:
          result = imagethumb::decodePngThumb(imgFile, binOut, w, h);
          break;
        case imagethumb::ImgFormat::Bmp:
          result = imagethumb::decodeBmpThumb(imgFile, binOut, w, h);
          break;
        case imagethumb::ImgFormat::Unknown:
          LOG_ERR("CVR", "Unrecognized cover format: %s", coverHref.c_str());
          result = imagethumb::DecodeResult::Permanent;
          break;
      }
      binOut.flush();
    }
  }
  Storage.remove(tmpImgPath.c_str());

  if (result != imagethumb::DecodeResult::Ok) {
    Storage.remove(tmpBinPath.c_str());
    if (result == imagethumb::DecodeResult::Permanent) writeNoneSentinel(nonePath);
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

void CoverThumb::preacquireScratch() { imagethumb::preacquireScratch(); }

void CoverThumb::releaseScratch() { imagethumb::releaseScratch(); }

bool CoverThumb::draw(Gfx& gfx, const char* binPath, const int x, const int y, int* outW,
                      int* outH, const int clipX0, const int clipX1) {
  return imagethumb::blit(gfx, binPath, x, y, outW, outH, clipX0, clipX1);
}

}  // namespace reader
