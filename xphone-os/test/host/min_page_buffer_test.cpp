// Host test for reader::FbpBook::minPageBufferBytes: the smallest contiguous
// block a package needs to open at all, computed from a synthetic package so
// the test needs no bookc-built fixture. Mirrors the formula the reader uses
// to decide whether the radio must yield (X3 trace: 12,079 B needed for a
// 528x792 fmt_ver 9 compact package vs 7,156 B largest block with BLE up).
#include "../../src/reader/FbpBook.h"
#include "../../src/Gfx.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

// minPageBufferBytes allocates nothing, so FbpBook.cpp is linked with its
// normal malloc/free (no ReaderAllocationFaults redirection). uzlib's crc
// helpers and Gfx::drawPixel are only needed to satisfy the linker for the
// parts of FbpBook.cpp this test does not exercise (renderPage is not called).
extern "C" uint32_t uzlib_crc32(const void*, unsigned, uint32_t) { std::abort(); }
extern "C" uint32_t uzlib_adler32(const void*, unsigned, uint32_t) { std::abort(); }
void Gfx::drawPixel(int, int, bool) {}

namespace {

void put_u16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
  b[off] = (uint8_t)(v & 0xFF);
  b[off + 1] = (uint8_t)(v >> 8);
}
void put_u32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
  b[off] = (uint8_t)(v & 0xFF);
  b[off + 1] = (uint8_t)((v >> 8) & 0xFF);
  b[off + 2] = (uint8_t)((v >> 16) & 0xFF);
  b[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}
void put_u64(std::vector<uint8_t>& b, size_t off, uint64_t v) {
  for (int i = 0; i < 8; i++) b[off + i] = (uint8_t)((v >> (8 * i)) & 0xFF);
}

// 80-byte FbpBook::Header, then 3 compact 48-byte ProfileDir entries, then
// three page-index slots and three "PRED" predictor tables. fmt_ver 9 compact
// profiles: page = dict_size + max_raw_page + 1; predictors read from the PRED
// block at dict_off + dict_size; page_index_off points at a slot whose value
// must equal dict_off + dict_size + 8 + predictor_count * 8.
std::vector<uint8_t> compactFixture() {
  std::vector<uint8_t> b(65536, 0);
  // Header.
  memcpy(b.data(), "FBPK", 4);
  put_u16(b, 4, 9);    // fmt_ver
  put_u16(b, 6, 6);    // min_reader
  put_u32(b, 24, 3);   // profile_count
  put_u64(b, 64, 248); // meta_off -> zero region (empty title/author)
  // Three 528x792 profiles: dict_off + dict_size lands each PRED block.
  struct Spec {
    size_t dir;      // ProfileDir offset
    uint64_t dict_off;
    uint32_t dict_size, max_raw;
    uint64_t page_index_off;  // slot offset
    uint64_t prd_off;         // dict_off + dict_size (PRED block)
    uint32_t predictors;
  };
  const Spec specs[3] = {
      {80, 0, 8192, 3086, 224, 8192, 100},
      {128, 8192, 8192, 4000, 232, 16384, 200},
      {176, 16384, 16384, 3086, 240, 32768, 100},
  };
  for (const Spec& s : specs) {
    put_u16(b, s.dir + 0, 528);   // width
    put_u16(b, s.dir + 2, 792);   // height
    put_u16(b, s.dir + 4, 22);    // px_size
    put_u16(b, s.dir + 6, 10);    // page_count
    put_u64(b, s.dir + 8, s.page_index_off);
    put_u64(b, s.dir + 24, s.dict_off);
    put_u32(b, s.dir + 32, s.dict_size);
    put_u32(b, s.dir + 36, s.max_raw);
    b[s.dir + 40] = 1;            // font_count
    b[s.dir + 47] = 2;            // reserved[6] == FBP_COMPACT_CODEC
    // First page record offset: immediately after the predictor table.
    put_u64(b, s.page_index_off, s.prd_off + 8 + (uint64_t)s.predictors * 8);
    // PRED block: magic + count + count * {key,delta}.
    memcpy(b.data() + s.prd_off, "PRED", 4);
    put_u32(b, s.prd_off + 4, s.predictors);
  }
  return b;
}

void writeFile(const char* path, const std::vector<uint8_t>& bytes) {
  FILE* f = std::fopen(path, "wb");
  assert(f);
  assert(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
  std::fclose(f);
}

void testCompact() {
  const char* path = "/tmp/fbp_minbuf_compact.fbp";
  writeFile(path, compactFixture());
  const uint32_t need = reader::FbpBook::minPageBufferBytes(path);
  // Lightest profile: (8192 + 3086 + 1) + 100 * 8 = 12,079.
  assert(need == 12079u);
  std::remove(path);
}

// A v3 package (no dictionary) cannot be decided: the helper must return 0,
// which means "keep today's behaviour".
void testV3() {
  const char* path = "/tmp/fbp_minbuf_v3.fbp";
  std::vector<uint8_t> b(4096, 0);
  memcpy(b.data(), "FBPK", 4);
  put_u16(b, 4, 3);    // fmt_ver
  put_u16(b, 6, 6);    // min_reader
  put_u32(b, 24, 1);   // profile_count
  put_u64(b, 64, 248); // meta_off -> zero region
  writeFile(path, b);
  assert(reader::FbpBook::minPageBufferBytes(path) == 0u);
  std::remove(path);
}

void testUnreadable() {
  // Nonexistent path and bad magic both mean "cannot decide".
  assert(reader::FbpBook::minPageBufferBytes("/tmp/fbp_minbuf_missing.fbp") == 0u);
  const char* path = "/tmp/fbp_minbuf_bad.fbp";
  std::vector<uint8_t> b(128, 0);
  memcpy(b.data(), "NOPE", 4);
  writeFile(path, b);
  assert(reader::FbpBook::minPageBufferBytes(path) == 0u);
  std::remove(path);
}

}  // namespace

int main() {
  testCompact();
  testV3();
  testUnreadable();
  std::puts("PASS: minPageBufferBytes (compact min, v3 undecidable, unreadable)");
  return 0;
}
