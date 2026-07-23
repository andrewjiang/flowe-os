// SD-card firmware self-update. See SdUpdate.h for the contract.
//
// This is a minimal, single-task port of CrossPoint's proven SD flash path:
//   * validation  — x4-os/src/network/FirmwareFlasher.cpp validateImageFile()
//   * flashing    — x4-os/src/network/FirmwareFlasher.cpp flashFromSdPath()
//     (raw esp_partition erase/write, 4096-byte chunks, interleaved erase)
//   * boot switch — x4-os/src/network/OtaBootSwitch.cpp switchTo()
//     (raw otadata write; bypasses esp_ota_set_boot_partition, whose
//      esp_image_verify rejects factory-bootloader-patched images)
// Differences: fixed static chunk buffer instead of heap, Serial logging
// instead of Logging.h, SDCardManager used directly (single task — no
// HalStorage mutex needed), and the consumed file is renamed to
// /update.bin.flashed so boot never loops reflashing the same image.

#include "SdUpdate.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <SDCardManager.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include <spi_flash_mmap.h>

#include <cstring>
#include <new>

namespace {

constexpr char UPDATE_PATH[] = "/update.bin";
constexpr char APPLIED_PATH[] = "/update.bin.flashed";

// Image constants — mirrors x4-os/src/network/FirmwareFlasher.cpp:20-28.
constexpr uint8_t IMAGE_MAGIC = 0xE9;
constexpr size_t MIN_FIRMWARE_SIZE = 64 * 1024;
constexpr size_t SEC = SPI_FLASH_SEC_SIZE;  // 4 KiB
constexpr size_t BLK = 64 * 1024;           // 64 KiB erase granularity
constexpr size_t CHUNK = 4096;
constexpr size_t SHA_TRAILER = 32;
constexpr uint8_t CHECKSUM_SEED = 0xEF;
constexpr size_t HEADER_SIZE = 24;
constexpr size_t SEG_HEADER_SIZE = 8;

// M2.1d: chunk buffer is heap-allocated in checkAndApply() only when an
// /update.bin actually exists, instead of a permanent 4 KiB BSS block that
// 99.9% of boots never touch. This runs pre-BLE at the emptiest point of the
// heap's lifetime — a single short-lived 4 KiB block freed before any
// long-lived allocation happens, so zero fragmentation risk. A stack local
// was rejected: the 4 KiB would sit on the 8 KB Arduino loopTask stack under
// SdFat + mbedtls sha256 + EInkDisplay::displayBuffer refreshes — too tight
// to certify without hardware high-water-mark data.
uint8_t* chunkBuf = nullptr;

#define SDUP_LOG(fmt, ...) Serial.printf("[xphone-os] sdup: " fmt "\n", ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Framebuffer primitives (same 1bpp MSB-first convention as main.cpp:37-63;
// duplicated here so main.cpp stays a plain staged boot sequence).
// ---------------------------------------------------------------------------

void fbFillRect(EInkDisplay& d, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool black) {
  uint8_t* fb = d.getFrameBuffer();
  const uint16_t dw = d.getDisplayWidth();
  const uint16_t dh = d.getDisplayHeight();
  const uint16_t wb = d.getDisplayWidthBytes();
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t py = y + row;
    if (py >= dh) break;
    for (uint16_t col = 0; col < w; col++) {
      const uint16_t px = x + col;
      if (px >= dw) break;
      const uint32_t idx = static_cast<uint32_t>(py) * wb + (px >> 3);
      const uint8_t mask = 0x80 >> (px & 7);
      if (black) {
        fb[idx] &= ~mask;
      } else {
        fb[idx] |= mask;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Progress bar — font-free, coarse steps, few refreshes. One FULL refresh for
// the empty frame, then a FAST refresh per 25% fill increment (5 total).
// ---------------------------------------------------------------------------

struct ProgressBar {
  EInkDisplay& d;
  uint16_t x, y, w, h;
  unsigned lastStep;  // 0..STEPS

  static constexpr unsigned STEPS = 4;  // fill in 25% increments
  static constexpr uint16_t BORDER = 4;

  explicit ProgressBar(EInkDisplay& display) : d(display), lastStep(0) {
    const uint16_t dw = d.getDisplayWidth();
    const uint16_t dh = d.getDisplayHeight();
    w = dw - 160;
    h = 48;
    x = (dw - w) / 2;
    y = (dh - h) / 2;
  }

  void drawFrame() {
    d.clearScreen(0xFF);
    // Outline.
    fbFillRect(d, x, y, w, BORDER, true);
    fbFillRect(d, x, y + h - BORDER, w, BORDER, true);
    fbFillRect(d, x, y, BORDER, h, true);
    fbFillRect(d, x + w - BORDER, y, BORDER, h, true);
    // Small solid marker above the bar so the screen visibly means "updating"
    // even before the first fill step.
    fbFillRect(d, x, y - 24, 48, 12, true);
    d.displayBuffer(EInkDisplay::FULL_REFRESH);
  }

  // Called per chunk; only refreshes when a 25% boundary is crossed.
  void update(size_t written, size_t total) {
    if (total == 0) return;
    const unsigned step = static_cast<unsigned>((written * STEPS) / total);
    if (step <= lastStep) return;
    lastStep = step;
    const uint16_t innerW = w - 2 * BORDER;
    const uint16_t fillW = static_cast<uint16_t>((static_cast<uint32_t>(innerW) * step) / STEPS);
    fbFillRect(d, x + BORDER, y + BORDER, fillW, h - 2 * BORDER, true);
    d.displayBuffer(EInkDisplay::FAST_REFRESH);
  }
};

// Distinct error pattern: two thick diagonal bars forming an X, centered.
// One FULL refresh, then a pause so it is visible before normal boot redraws.
void drawErrorX(EInkDisplay& d) {
  const uint16_t dw = d.getDisplayWidth();
  const uint16_t dh = d.getDisplayHeight();
  const uint16_t s = (dw < dh ? dw : dh) / 2;  // X square side
  const uint16_t x0 = (dw - s) / 2;
  const uint16_t y0 = (dh - s) / 2;
  constexpr uint16_t T = 20;  // stroke thickness

  d.clearScreen(0xFF);
  fbFillRect(d, x0, y0, s, 8, true);  // top rule to frame the glyph
  fbFillRect(d, x0, y0 + s - 8, s, 8, true);
  for (uint16_t i = 0; i + T <= s; i += T / 2) {
    fbFillRect(d, x0 + i, y0 + i, T, T, true);              // "\" stroke
    fbFillRect(d, x0 + s - i - T, y0 + i, T, T, true);      // "/" stroke
  }
  d.displayBuffer(EInkDisplay::FULL_REFRESH);
  delay(2500);
}

// ---------------------------------------------------------------------------
// Image validation — straight port of firmware_flash::validateImageFile
// (x4-os/src/network/FirmwareFlasher.cpp:89-227) onto a raw FsFile and the
// static chunk buffer. Verifies header magic, segment table bounds, the
// ESP image XOR checksum, and the SHA256 trailer end-to-end so a truncated
// or corrupted update.bin never becomes the boot target.
// ---------------------------------------------------------------------------

bool feedHashAndChecksum(FsFile& file, size_t length, uint8_t* xorAccum, mbedtls_sha256_context* sha) {
  size_t remaining = length;
  while (remaining > 0) {
    const size_t want = remaining < CHUNK ? remaining : CHUNK;
    const int got = file.read(chunkBuf, want);
    if (got <= 0 || static_cast<size_t>(got) != want) return false;
    mbedtls_sha256_update(sha, chunkBuf, want);
    uint8_t acc = *xorAccum;
    for (size_t i = 0; i < want; i++) acc ^= chunkBuf[i];
    *xorAccum = acc;
    remaining -= want;
  }
  return true;
}

bool validateImage(FsFile& file, size_t partitionSize) {
  const size_t fileSize = static_cast<size_t>(file.fileSize());
  if (fileSize < MIN_FIRMWARE_SIZE) {
    SDUP_LOG("validate: too small: %u", static_cast<unsigned>(fileSize));
    return false;
  }
  if (fileSize > partitionSize) {
    SDUP_LOG("validate: too large: %u > %u", static_cast<unsigned>(fileSize), static_cast<unsigned>(partitionSize));
    return false;
  }

  file.rewind();  // returns void in SdFat; subsequent reads catch any failure

  uint8_t header[HEADER_SIZE];
  if (file.read(header, HEADER_SIZE) != static_cast<int>(HEADER_SIZE)) {
    SDUP_LOG("validate: header read failed");
    return false;
  }
  if (header[0] != IMAGE_MAGIC) {
    SDUP_LOG("validate: bad magic 0x%02X", header[0]);
    return false;
  }
  const uint8_t segCount = header[1];
  const bool hashAppended = header[23] != 0;

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, /*is224=*/0);
  mbedtls_sha256_update(&sha, header, HEADER_SIZE);

  bool ok = false;
  uint8_t xorAccum = CHECKSUM_SEED;
  size_t pos = HEADER_SIZE;

  do {
    bool segsOk = true;
    for (uint8_t i = 0; i < segCount; i++) {
      if (pos + SEG_HEADER_SIZE > fileSize) {
        SDUP_LOG("validate: seg %u header overruns EOF at %u", i, static_cast<unsigned>(pos));
        segsOk = false;
        break;
      }
      uint8_t segHdr[SEG_HEADER_SIZE];
      if (file.read(segHdr, SEG_HEADER_SIZE) != static_cast<int>(SEG_HEADER_SIZE)) {
        SDUP_LOG("validate: seg %u header read failed", i);
        segsOk = false;
        break;
      }
      mbedtls_sha256_update(&sha, segHdr, SEG_HEADER_SIZE);
      pos += SEG_HEADER_SIZE;

      uint32_t dataLen;
      std::memcpy(&dataLen, segHdr + 4, sizeof(dataLen));
      if (pos + dataLen > fileSize) {
        SDUP_LOG("validate: seg %u data overruns EOF (%u + %u > %u)", i, static_cast<unsigned>(pos),
                 static_cast<unsigned>(dataLen), static_cast<unsigned>(fileSize));
        segsOk = false;
        break;
      }
      if (!feedHashAndChecksum(file, dataLen, &xorAccum, &sha)) {
        SDUP_LOG("validate: seg %u data read failed", i);
        segsOk = false;
        break;
      }
      pos += dataLen;
    }
    if (!segsOk) break;

    // Checksum byte sits at the last byte of the 16-byte-aligned padding.
    const size_t padEnd = (pos + 16) & ~static_cast<size_t>(15);
    const size_t expectedTotal = padEnd + (hashAppended ? SHA_TRAILER : 0);
    if (expectedTotal != fileSize) {
      SDUP_LOG("validate: size mismatch expected=%u actual=%u", static_cast<unsigned>(expectedTotal),
               static_cast<unsigned>(fileSize));
      break;
    }

    const size_t padLen = padEnd - pos;
    uint8_t padBuf[16];
    if (padLen == 0 || padLen > sizeof(padBuf)) {
      SDUP_LOG("validate: bad pad length %u", static_cast<unsigned>(padLen));
      break;
    }
    if (file.read(padBuf, padLen) != static_cast<int>(padLen)) {
      SDUP_LOG("validate: pad read failed");
      break;
    }
    mbedtls_sha256_update(&sha, padBuf, padLen);

    const uint8_t storedChecksum = padBuf[padLen - 1];
    if ((xorAccum & 0xFF) != storedChecksum) {
      SDUP_LOG("validate: checksum mismatch computed=0x%02X stored=0x%02X", xorAccum, storedChecksum);
      break;
    }

    if (hashAppended) {
      uint8_t computed[SHA_TRAILER];
      mbedtls_sha256_finish(&sha, computed);
      uint8_t stored[SHA_TRAILER];
      if (file.read(stored, SHA_TRAILER) != static_cast<int>(SHA_TRAILER)) {
        SDUP_LOG("validate: SHA trailer read failed");
        break;
      }
      if (std::memcmp(computed, stored, SHA_TRAILER) != 0) {
        SDUP_LOG("validate: SHA256 mismatch");
        break;
      }
    }
    ok = true;
  } while (false);

  mbedtls_sha256_free(&sha);
  return ok;
}

// ---------------------------------------------------------------------------
// otadata switch — port of ota_boot::switchTo (x4-os/src/network/
// OtaBootSwitch.cpp:14-84). Writes a fresh select entry with a higher
// ota_seq into the inactive otadata slot, bypassing
// esp_ota_set_boot_partition's esp_image_verify (which rejects
// factory-bootloader-patched images with bogus efuse-blk-rev errors).
// ---------------------------------------------------------------------------

struct __attribute__((packed)) SelectEntry {
  uint32_t ota_seq;
  uint8_t seq_label[20];
  uint32_t ota_state;
  uint32_t crc;
};
static_assert(sizeof(SelectEntry) == 32, "SelectEntry must be 32 bytes");

constexpr uint32_t OTA_IMG_NEW = 0;      // ESP_OTA_IMG_NEW
constexpr uint32_t OTA_IMG_INVALID = 3;  // ESP_OTA_IMG_INVALID
constexpr uint32_t OTA_IMG_ABORTED = 4;  // ESP_OTA_IMG_ABORTED

uint32_t seqCrc(uint32_t seq) {
  // CRC32-LE over the 4-byte ota_seq, init UINT32_MAX. Matches IDF.
  return esp_rom_crc32_le(UINT32_MAX, reinterpret_cast<const uint8_t*>(&seq), sizeof(seq));
}

bool otadataSwitchTo(const esp_partition_t* dest) {
  const esp_partition_t* otadata =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (!otadata) {
    SDUP_LOG("otadata partition not found");
    return false;
  }
  if (otadata->size < 2 * SPI_FLASH_SEC_SIZE) {
    SDUP_LOG("otadata too small: %u", static_cast<unsigned>(otadata->size));
    return false;
  }

  SelectEntry slots[2] = {};
  if (esp_partition_read(otadata, 0, &slots[0], sizeof(SelectEntry)) != ESP_OK ||
      esp_partition_read(otadata, SPI_FLASH_SEC_SIZE, &slots[1], sizeof(SelectEntry)) != ESP_OK) {
    SDUP_LOG("otadata read failed");
    return false;
  }

  // Pick the slot with valid CRC and highest seq, ignoring INVALID/ABORTED.
  int activeIdx = -1;
  uint32_t activeSeq = 0;
  for (int i = 0; i < 2; ++i) {
    if (slots[i].ota_seq == 0xFFFFFFFFu) continue;
    if (slots[i].crc != seqCrc(slots[i].ota_seq)) continue;
    if (slots[i].ota_state == OTA_IMG_INVALID || slots[i].ota_state == OTA_IMG_ABORTED) continue;
    if (activeIdx < 0 || slots[i].ota_seq > activeSeq) {
      activeIdx = i;
      activeSeq = slots[i].ota_seq;
    }
  }

  const uint32_t destOtaIdx =
      static_cast<uint32_t>(dest->subtype) - static_cast<uint32_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0);
  if (destOtaIdx > 15) {
    SDUP_LOG("dest is not an OTA app partition (subtype=0x%02X)", dest->subtype);
    return false;
  }

  // Smallest seq > activeSeq with (seq-1) % 2 == destOtaIdx (2 OTA slots in
  // partitions.csv: app0/ota_0 + app1/ota_1, shared with CrossPoint).
  uint32_t newSeq = activeSeq + 1;
  while (((newSeq - 1u) % 2u) != (destOtaIdx % 2u)) ++newSeq;

  SelectEntry next = {};
  next.ota_seq = newSeq;
  std::memset(next.seq_label, 0xFF, sizeof(next.seq_label));
  next.ota_state = OTA_IMG_NEW;
  next.crc = seqCrc(next.ota_seq);

  const int targetSlot = (activeIdx == 0) ? 1 : 0;
  const size_t targetOff = static_cast<size_t>(targetSlot) * SPI_FLASH_SEC_SIZE;

  if (esp_partition_erase_range(otadata, targetOff, SPI_FLASH_SEC_SIZE) != ESP_OK) {
    SDUP_LOG("otadata erase failed (slot=%d)", targetSlot);
    return false;
  }
  if (esp_partition_write(otadata, targetOff, &next, sizeof(next)) != ESP_OK) {
    SDUP_LOG("otadata write failed (slot=%d)", targetSlot);
    return false;
  }

  SDUP_LOG("otadata: wrote slot=%d seq=%u -> %s", targetSlot, static_cast<unsigned>(newSeq), dest->label);
  return true;
}

// ---------------------------------------------------------------------------
// Flash loop — port of firmware_flash::flashFromSdPath
// (x4-os/src/network/FirmwareFlasher.cpp:229-308): interleaved 64 KiB erase +
// 4096-byte chunked writes so progress advances smoothly.
// ---------------------------------------------------------------------------

bool flashImage(FsFile& file, const esp_partition_t* dest, ProgressBar& bar) {
  file.rewind();  // returns void in SdFat; subsequent reads catch any failure
  const size_t firmwareSize = static_cast<size_t>(file.fileSize());

  size_t streamPos = 0;
  size_t erasedUpto = 0;
  unsigned lastLoggedPct = 0;
  while (streamPos < firmwareSize) {
    if (streamPos >= erasedUpto) {
      size_t eraseLen = BLK < dest->size - streamPos ? BLK : dest->size - streamPos;
      eraseLen = (eraseLen + SEC - 1) & ~(SEC - 1);
      if (eraseLen > dest->size - streamPos) eraseLen = dest->size - streamPos;
      if (esp_partition_erase_range(dest, streamPos, eraseLen) != ESP_OK) {
        SDUP_LOG("erase @%u (len=%u) failed", static_cast<unsigned>(streamPos), static_cast<unsigned>(eraseLen));
        return false;
      }
      erasedUpto = streamPos + eraseLen;
    }

    const size_t want = (firmwareSize - streamPos) < CHUNK ? (firmwareSize - streamPos) : CHUNK;
    const int got = file.read(chunkBuf, want);
    if (got <= 0 || static_cast<size_t>(got) != want) {
      SDUP_LOG("read @%u: got=%d want=%u", static_cast<unsigned>(streamPos), got, static_cast<unsigned>(want));
      return false;
    }
    if (esp_partition_write(dest, streamPos, chunkBuf, want) != ESP_OK) {
      SDUP_LOG("write @%u failed", static_cast<unsigned>(streamPos));
      return false;
    }
    streamPos += want;

    bar.update(streamPos, firmwareSize);
    const unsigned pct = static_cast<unsigned>((static_cast<uint64_t>(streamPos) * 100) / firmwareSize);
    if (pct >= lastLoggedPct + 10) {
      lastLoggedPct = pct - (pct % 10);
      SDUP_LOG("flashed %u%% (%u / %u bytes)", pct, static_cast<unsigned>(streamPos),
               static_cast<unsigned>(firmwareSize));
    }
    delay(1);  // feed the watchdog / let USB CDC drain
  }
  return true;
}

// Move update.bin out of the way so the freshly flashed firmware (which runs
// this same check) does not reflash it forever. Rename to /update.bin.flashed
// (keeps the image on card for re-use: rename it back to update.bin to flash
// again); if rename fails, fall back to deleting. Returns false only if the
// file could not be moved OR removed — in that case the caller must NOT
// switch otadata, or boot would loop reflashing.
bool retireUpdateFile() {
  SdMan.remove(APPLIED_PATH);  // drop stale copy from a previous update, if any
  if (SdMan.rename(UPDATE_PATH, APPLIED_PATH)) {
    SDUP_LOG("renamed %s -> %s", UPDATE_PATH, APPLIED_PATH);
    return true;
  }
  SDUP_LOG("rename failed, deleting %s instead", UPDATE_PATH);
  if (SdMan.remove(UPDATE_PATH)) return true;
  SDUP_LOG("could not rename or delete %s — refusing to switch boot partition", UPDATE_PATH);
  return false;
}

// ---------------------------------------------------------------------------
// Shared apply path — everything after "an image file was chosen": chunk
// buffer alloc, open, validate, progress-bar flash, retire (update.bin only),
// otadata switch, restart. Used by both the boot path (checkAndApply) and the
// Settings picker (flashFromPath). Returns false on any failure (error X
// drawn, boot selection untouched); on success restarts and never returns.
// ---------------------------------------------------------------------------
bool applyImageAtPath(EInkDisplay& display, const char* path) {
  // Allocate the 4 KiB chunk buffer only now that an image was actually
  // chosen (see the chunkBuf comment above for why heap and not BSS/stack).
  // RAII guard frees it on every return path below; the esp_restart() success
  // path never returns, which is fine — the heap dies with the reboot.
  chunkBuf = new (std::nothrow) uint8_t[CHUNK];
  if (!chunkBuf) {
    SDUP_LOG("chunk buffer alloc (%u B) failed — skipping update", static_cast<unsigned>(CHUNK));
    drawErrorX(display);
    return false;  // firmware stays in charge; retry later
  }
  struct ChunkBufGuard {
    ~ChunkBufGuard() {
      delete[] chunkBuf;
      chunkBuf = nullptr;
    }
  } chunkBufGuard;

  FsFile file = SdMan.open(path, O_RDONLY);
  if (!file) {
    SDUP_LOG("open %s failed", path);
    drawErrorX(display);
    return false;
  }

  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    SDUP_LOG("no next-update partition");
    file.close();
    drawErrorX(display);
    return false;
  }
  SDUP_LOG("flashing %s (%u bytes); running=%s dest=%s @0x%x", path,
           static_cast<unsigned>(file.fileSize()), running ? running->label : "?", dest->label,
           static_cast<unsigned>(dest->address));

  if (!validateImage(file, dest->size)) {
    SDUP_LOG("image validation failed — leaving %s untouched", path);
    file.close();
    drawErrorX(display);
    return false;
  }
  SDUP_LOG("image validated OK");

  ProgressBar bar(display);
  bar.drawFrame();

  const unsigned long tFlash = millis();
  const bool flashed = flashImage(file, dest, bar);
  file.close();
  if (!flashed) {
    SDUP_LOG("flash failed — boot partition unchanged");
    drawErrorX(display);
    return false;
  }
  SDUP_LOG("flash complete in %lu ms", millis() - tFlash);

  // Retire /update.bin BEFORE switching otadata: if the file can't be moved
  // out of the way, switching would make every subsequent boot reflash it.
  // Other picker-chosen images (update.bin.flashed, arbitrary *.bin) are not
  // boot-consumed, so they stay on the card untouched.
  if (std::strcmp(path, UPDATE_PATH) == 0 && !retireUpdateFile()) {
    drawErrorX(display);
    return false;
  }

  if (!otadataSwitchTo(dest)) {
    // Partition contents are written but boot selection is unchanged; the
    // current firmware stays in charge. (update.bin was already retired —
    // re-copy it to retry.)
    SDUP_LOG("boot switch failed");
    drawErrorX(display);
    return false;
  }

  SDUP_LOG("update applied, restarting into %s", dest->label);
  Serial.flush();
  delay(250);
  esp_restart();
  return true;  // unreachable
}

}  // namespace

namespace sd_update {

void checkAndApply(EInkDisplay& display) {
  // Mount. The shared SPI bus (SCLK=8 / MOSI=10 + SD's MISO=7) must already be
  // up — main.cpp calls SPI.begin() with the SD MISO before display.begin(),
  // mirroring x4-os/lib/hal/HalGPIO.cpp:195. On X3/X4 sd.sclk is unassigned so
  // SDCardManager itself never touches SPI pin mapping.
  if (!SdMan.begin()) {
    SDUP_LOG("no SD card (or mount failed) — continuing normal boot");
    return;
  }
  if (!SdMan.exists(UPDATE_PATH)) {
    SDUP_LOG("no %s on SD root — continuing normal boot", UPDATE_PATH);
    return;
  }
  applyImageAtPath(display, UPDATE_PATH);  // restarts on success
}

bool flashFromPath(EInkDisplay& display, const char* path) {
  if (!SdMan.exists(path)) {
    SDUP_LOG("flashFromPath: %s not found", path);
    drawErrorX(display);
    return false;
  }
  return applyImageAtPath(display, path);  // restarts on success
}

}  // namespace sd_update
