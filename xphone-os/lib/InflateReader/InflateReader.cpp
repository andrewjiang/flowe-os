#include "InflateReader.h"

#include <Arduino.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cstring>
#include <type_traits>

namespace {
constexpr size_t INFLATE_DICT_SIZE = InflateReader::kDictSize;

uint8_t* gSharedDict = nullptr;
bool gSharedDictInUse = false;
// True while gSharedDict points at a LENT buffer (the framebuffer) rather
// than a heap allocation this module owns.
bool gSharedDictBorrowed = false;
}

void InflateReader::setSharedDict(uint8_t* dict) {
  gSharedDict = dict;
  gSharedDictInUse = false;
  gSharedDictBorrowed = false;
}

void InflateReader::lendDict(uint8_t* buf) {
  if (gSharedDict && !gSharedDictBorrowed) return;  // real heap dict parked: keep it
  if (gSharedDictInUse) return;                     // mid-inflate: don't swap the ring
  gSharedDict = buf;
  gSharedDictInUse = false;
  gSharedDictBorrowed = true;
}

void InflateReader::returnDict() {
  if (!gSharedDictBorrowed) return;
  if (gSharedDictInUse) {
    Serial.println("[xphone-os] InflateReader: lent dict still in use at return — lifetime bug");
  }
  gSharedDict = nullptr;
  gSharedDictInUse = false;
  gSharedDictBorrowed = false;
}

void InflateReader::releaseSharedDict() {
  if (gSharedDictBorrowed) return;  // loan: returnDict() owns the lifecycle
  if (gSharedDict && !gSharedDictInUse) {
    free(gSharedDict);
    gSharedDict = nullptr;
  }
}

bool InflateReader::hasSharedDict() { return gSharedDict != nullptr; }

// Guarantee the cast pattern in the header comment is valid.
static_assert(std::is_standard_layout<InflateReader>::value,
              "InflateReader must be standard-layout for the uzlib callback cast to work");

InflateReader::~InflateReader() { deinit(); }

bool InflateReader::init(const bool streaming) {
  deinit();  // free any previously allocated ring buffer and reset state

  if (streaming) {
    if (gSharedDict && !gSharedDictInUse) {
      // Borrow the reserved dictionary FIRST — it exists precisely for this
      // (static BSS since boot). Malloc is the fallback for nested inflates
      // only: on X3-class heaps a fresh 32 KB block never exists, and on the
      // X4 the old malloc-first order churned a transient 32 KB block per
      // streaming open while the reserved dict sat idle.
      gSharedDictInUse = true;
      usingSharedDict = true;
      ringBuffer = gSharedDict;
    } else {
      ringBuffer = static_cast<uint8_t*>(malloc(INFLATE_DICT_SIZE));
    }
    if (!ringBuffer) {
      LOG_ERR("ZIP", "No 32 KB inflate window: largest block %u, free %u, reserved dict %s",
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
              static_cast<unsigned>(ESP.getFreeHeap()),
              gSharedDict ? (gSharedDictInUse ? "in use" : "free") : "none");
      return false;
    }
    memset(ringBuffer, 0, INFLATE_DICT_SIZE);
  }

  uzlib_uncompress_init(&decomp, ringBuffer, ringBuffer ? INFLATE_DICT_SIZE : 0);
  return true;
}

void InflateReader::deinit() {
  if (ringBuffer) {
    if (usingSharedDict) {
      gSharedDictInUse = false;
      usingSharedDict = false;
    } else {
      free(ringBuffer);
    }
    ringBuffer = nullptr;
  }
  memset(&decomp, 0, sizeof(decomp));
}

void InflateReader::setSource(const uint8_t* src, size_t len) {
  decomp.source = src;
  decomp.source_limit = src + len;
}

void InflateReader::setReadCallback(int (*cb)(struct uzlib_uncomp*)) { decomp.source_read_cb = cb; }

void InflateReader::skipZlibHeader() {
  uzlib_get_byte(&decomp);
  uzlib_get_byte(&decomp);
}

bool InflateReader::read(uint8_t* dest, size_t len) {
  if (!ringBuffer) {
    // One-shot mode: back-references use absolute offset from dest_start.
    // Valid only when read() is called once with the full output buffer.
    decomp.dest_start = dest;
  }
  decomp.dest = dest;
  decomp.dest_limit = dest + len;

  const int res = uzlib_uncompress(&decomp);
  if (res < 0) return false;
  return decomp.dest == decomp.dest_limit;
}

InflateStatus InflateReader::readAtMost(uint8_t* dest, size_t maxLen, size_t* produced) {
  if (!ringBuffer) {
    // One-shot mode: back-references use absolute offset from dest_start.
    // Valid only when readAtMost() is called once with the full output buffer.
    decomp.dest_start = dest;
  }
  decomp.dest = dest;
  decomp.dest_limit = dest + maxLen;

  const int res = uzlib_uncompress(&decomp);
  *produced = static_cast<size_t>(decomp.dest - dest);

  if (res == TINF_DONE) return InflateStatus::Done;
  if (res < 0) return InflateStatus::Error;
  return InflateStatus::Ok;
}
