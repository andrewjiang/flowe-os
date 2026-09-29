#include "BackgroundImage.h"

#include <Arduino.h>
#include <HalStorage.h>

#include "ImageThumb.h"

namespace BackgroundImage {
namespace {

constexpr const char* kCacheDir = "/.xphone";
constexpr const char* kSource = "/background.bmp";

// Sidecar cache-validity header (distinct from ImageThumb's own 'XT' thumb
// format, which the cache .bin still uses): magic, version, then the source
// file's byte size at the time the cache was built, so a replaced
// background.bmp invalidates the cache without an mtime API.
constexpr uint16_t kMetaMagic = 0x4247;  // 'BG'
constexpr uint8_t kMetaVersion = 1;
constexpr int kMetaSize = 7;

std::string cachePath(const int w, const int h) {
  return std::string(kCacheDir) + "/background_" + std::to_string(w) + "x" + std::to_string(h) + ".bin";
}

std::string metaPath(const int w, const int h) {
  return std::string(kCacheDir) + "/background_" + std::to_string(w) + "x" + std::to_string(h) + ".meta";
}

// True when the cache's recorded source size still matches the source file.
bool cacheFresh(const std::string& meta, const size_t srcSize) {
  HalFile f;
  if (!Storage.openFileForRead("BG", meta, f)) return false;
  uint8_t hdr[kMetaSize];
  if (f.read(hdr, sizeof(hdr)) != sizeof(hdr)) return false;
  const uint16_t magic = static_cast<uint16_t>(hdr[0] | (hdr[1] << 8));
  if (magic != kMetaMagic || hdr[2] != kMetaVersion) return false;
  const uint32_t storedSize = static_cast<uint32_t>(hdr[3]) | (static_cast<uint32_t>(hdr[4]) << 8) |
                              (static_cast<uint32_t>(hdr[5]) << 16) | (static_cast<uint32_t>(hdr[6]) << 24);
  return storedSize == static_cast<uint32_t>(srcSize);
}

void writeMeta(const std::string& meta, const size_t srcSize) {
  HalFile f;
  if (!Storage.openFileForWrite("BG", meta, f)) return;
  const uint32_t sz = static_cast<uint32_t>(srcSize);
  const uint8_t hdr[kMetaSize] = {
      static_cast<uint8_t>(kMetaMagic & 0xFF), static_cast<uint8_t>(kMetaMagic >> 8),
      kMetaVersion,
      static_cast<uint8_t>(sz & 0xFF),         static_cast<uint8_t>((sz >> 8) & 0xFF),
      static_cast<uint8_t>((sz >> 16) & 0xFF), static_cast<uint8_t>((sz >> 24) & 0xFF),
  };
  f.write(hdr, sizeof(hdr));
  f.flush();
}

}  // namespace

bool available() { return Storage.exists(kSource); }

bool cached(const int w, const int h) {
  if (w <= 0 || h <= 0 || w > imagethumb::kMaxThumbW || h > imagethumb::kMaxThumbH) return false;
  if (!Storage.exists(kSource)) return false;
  if (!Storage.exists(cachePath(w, h).c_str())) return false;
  HalFile srcFile;
  if (!Storage.openFileForRead("BG", kSource, srcFile)) return false;
  return cacheFresh(metaPath(w, h), srcFile.size());
}

bool ensure(const int w, const int h, std::string* outPath, int* outW, int* outH) {
  if (w <= 0 || h <= 0 || w > imagethumb::kMaxThumbW || h > imagethumb::kMaxThumbH) {
    // Landscape (the Reader swaps Gfx's logical w/h) lands here, as would any
    // future panel bigger than the cap. Logged because the caller can only
    // fall back to the normal face without a word otherwise.
    Serial.printf("[xphone-os] bg: target %dx%d outside the %dx%d cap\n", w, h, imagethumb::kMaxThumbW,
                  imagethumb::kMaxThumbH);
    return false;
  }

  if (!Storage.exists(kSource)) {
    Serial.printf("[xphone-os] bg: no %s on the card\n", kSource);
    return false;
  }

  const std::string bin = cachePath(w, h);
  const std::string meta = metaPath(w, h);

  HalFile srcFile;
  if (!Storage.openFileForRead("BG", kSource, srcFile)) {
    Serial.printf("[xphone-os] bg: cannot open %s\n", kSource);
    return false;
  }
  const size_t srcSize = srcFile.size();

  if (Storage.exists(bin.c_str()) && cacheFresh(meta, srcSize)) {
    if (imagethumb::peekSize(bin.c_str(), outW, outH)) {
      if (outPath) *outPath = bin;
      return true;
    }
    // The .meta says this cache is current but the .bin won't parse (torn
    // write, power loss mid-rename, or a header from an older build). Drop it
    // and fall through to a fresh decode instead of failing every call from
    // here on.
    Serial.println("[xphone-os] bg: cached background unreadable; rebuilding");
    Storage.remove(bin.c_str());
    Storage.remove(meta.c_str());
  }

  Storage.mkdir(kCacheDir);

  // Magic-byte check, so a JPEG/PNG renamed to .bmp gets a clear message
  // rather than a confusing header-parse failure. Only BMP is supported here
  // on purpose — see BackgroundImage.h.
  if (imagethumb::sniffFormat(kSource, kSource) != imagethumb::ImgFormat::Bmp) {
    Serial.printf("[xphone-os] bg: %s is not a BMP (only uncompressed BMP is supported)\n", kSource);
    return false;
  }

  const std::string tmpBin = std::string(kCacheDir) + "/background.tmp";
  imagethumb::DecodeResult result;
  {
    HalFile binOut;
    if (!Storage.openFileForWrite("BG", tmpBin, binOut)) {
      Serial.println("[xphone-os] bg: cannot open the cache for writing");
      return false;
    }
    result = imagethumb::decodeBmpThumb(srcFile, binOut, w, h);
    binOut.flush();
  }

  if (result != imagethumb::DecodeResult::Ok) {
    Storage.remove(tmpBin.c_str());
    Serial.println("[xphone-os] bg: decode failed");
    return false;
  }

  Storage.remove(bin.c_str());
  if (!Storage.rename(tmpBin.c_str(), bin.c_str())) {
    Serial.println("[xphone-os] bg: failed to move the cache into place");
    Storage.remove(tmpBin.c_str());
    return false;
  }
  writeMeta(meta, srcSize);

  if (!imagethumb::peekSize(bin.c_str(), outW, outH)) {
    Serial.println("[xphone-os] bg: freshly built cache won't parse");
    return false;
  }
  if (outPath) *outPath = bin;
  return true;
}

}  // namespace BackgroundImage
