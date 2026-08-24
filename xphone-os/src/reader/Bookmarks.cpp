#include "Bookmarks.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include <cstdio>
#include <cstring>

namespace reader {
namespace {

constexpr uint16_t kMagic = 0x4B42;  // 'BK'
constexpr uint8_t kVersion = 1;

struct Header {
  uint16_t magic;
  uint8_t version;
  uint8_t count;
};

void sidecarPath(const char* bookPath, char* out, size_t cap) {
  snprintf(out, cap, "%s.bmk", bookPath);
}

}  // namespace

int Bookmarks::load(const char* bookPath, Mark* out, int cap) {
  if (!bookPath || !out || cap <= 0) return 0;
  char side[192];
  sidecarPath(bookPath, side, sizeof(side));
  FsFile f = SdMan.open(side, O_RDONLY);
  if (!f) return 0;
  Header h{};
  int n = 0;
  if (f.read(&h, sizeof(h)) == (int)sizeof(h) && h.magic == kMagic && h.version == kVersion) {
    n = h.count > kMax ? kMax : h.count;
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) {
      if (f.read(&out[i], sizeof(Mark)) != (int)sizeof(Mark)) {
        n = i;  // truncated file: keep what parsed
        break;
      }
    }
  }
  f.close();
  return n;
}

bool Bookmarks::save(const char* bookPath, const Mark* marks, int count) {
  if (!bookPath || (count > 0 && !marks)) return false;
  if (count > kMax) count = kMax;
  char side[192];
  sidecarPath(bookPath, side, sizeof(side));
  FsFile f = SdMan.open(side, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return false;
  Header h{kMagic, kVersion, (uint8_t)(count < 0 ? 0 : count)};
  bool ok = f.write(&h, sizeof(h)) == (int)sizeof(h);
  for (int i = 0; ok && i < count; i++)
    ok = f.write(&marks[i], sizeof(Mark)) == (int)sizeof(Mark);
  f.close();
  return ok;
}

int Bookmarks::add(const char* bookPath, Mark m) {
  Mark marks[kMax];
  int n = load(bookPath, marks, kMax);
  for (int i = 0; i < n; i++)
    if (marks[i].spine == m.spine && marks[i].page == m.page) return n;  // already marked
  if (n == kMax) {
    memmove(&marks[0], &marks[1], sizeof(Mark) * (kMax - 1));  // drop the oldest
    n = kMax - 1;
  }
  marks[n++] = m;
  return save(bookPath, marks, n) ? n : -1;
}

int Bookmarks::remove(const char* bookPath, int index) {
  Mark marks[kMax];
  int n = load(bookPath, marks, kMax);
  if (index < 0 || index >= n) return n;
  memmove(&marks[index], &marks[index + 1], sizeof(Mark) * (size_t)(n - index - 1));
  n--;
  return save(bookPath, marks, n) ? n : -1;
}

}  // namespace reader
