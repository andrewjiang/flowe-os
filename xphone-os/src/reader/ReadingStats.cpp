#include "ReadingStats.h"

#include <Arduino.h>
#include <HalStorage.h>

#include <cstdio>
#include <cstring>

#include "../ClockStore.h"
#include "ReaderLog.h"

namespace reader {
namespace {

constexpr const char* kPath = "/.xphone/stats.bin";
constexpr const char* kTmpPath = "/.xphone/stats.bin.tmp";
constexpr int kDays = 64;
constexpr int kBooks = 64;

struct DayRec {  // 8 B
  uint32_t day;  // yyyymmdd, 0 = empty slot
  uint16_t pages;
  uint16_t minutes;
};
struct BookRec {  // 16 B
  uint32_t hash;  // FNV-1a of book path, 0 = empty slot
  uint32_t pages;
  uint32_t minutes;
  uint32_t lastDay;  // yyyymmdd of last session (0 if clock unknown)
};
struct Store {  // 1540 B, plain POD read/written whole
  uint16_t magic;  // 'ST'
  uint8_t version;
  uint8_t reserved;
  DayRec days[kDays];
  BookRec books[kBooks];
};
constexpr uint16_t kMagic = 0x5453;
constexpr uint8_t kVersion = 1;

// Static store: 1.5 KB of BSS, deliberately NOT heap — stats must never
// contribute to the fragmentation the reader fights everywhere else.
Store s_store;
bool s_loaded = false;

// Open session accumulator.
bool s_active = false;
uint32_t s_hash = 0;
uint32_t s_startMs = 0;
uint16_t s_pages = 0;

uint32_t fnv1a(const char* s) {
  uint32_t h = 2166136261u;
  while (*s) {
    h ^= static_cast<uint8_t>(*s++);
    h *= 16777619u;
  }
  return h ? h : 1;  // 0 means "empty slot"
}

// Civil-calendar conversions (Howard Hinnant's algorithms) so streaks and the
// 7-day window cross month/year boundaries correctly without <ctime>.
int32_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}
void civilFromDays(int32_t z, int* y, int* m, int* d) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int yr = static_cast<int>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  *m = static_cast<int>(mp + (mp < 10 ? 3 : -9));
  *y = yr + (*m <= 2);
}
int32_t serialFromYmd(uint32_t ymd) {
  return daysFromCivil(static_cast<int>(ymd / 10000), static_cast<int>((ymd / 100) % 100),
                       static_cast<int>(ymd % 100));
}
uint32_t ymdFromSerial(int32_t serial) {
  int y, m, d;
  civilFromDays(serial, &y, &m, &d);
  return static_cast<uint32_t>(y) * 10000u + static_cast<uint32_t>(m) * 100u +
         static_cast<uint32_t>(d);
}

// Today's yyyymmdd from the phone-synced clock, rolled forward by uptime.
// 0 = no time.sync since boot; callers skip day attribution.
uint32_t todayYmd() {
  if (CLOCK_STORE.day == 0 || CLOCK_STORE.firstSyncMs == 0) return 0;
  const uint32_t elapsedMin = (millis() - CLOCK_STORE.firstSyncMs) / 60000u;
  const uint32_t totalMin = CLOCK_STORE.minutesIntoDay + elapsedMin;
  return ymdFromSerial(serialFromYmd(CLOCK_STORE.day) + static_cast<int32_t>(totalMin / 1440u));
}

DayRec* dayRec(uint32_t ymd, bool createIfMissing) {
  for (int i = 0; i < kDays; i++) {
    if (s_store.days[i].day == ymd) return &s_store.days[i];
  }
  if (!createIfMissing) return nullptr;
  // Evict the oldest slot (or an empty one — day 0 sorts oldest).
  DayRec* oldest = &s_store.days[0];
  for (int i = 1; i < kDays; i++) {
    if (s_store.days[i].day < oldest->day) oldest = &s_store.days[i];
  }
  *oldest = DayRec{ymd, 0, 0};
  return oldest;
}

BookRec* bookRec(uint32_t hash) {
  BookRec* lru = &s_store.books[0];
  for (int i = 0; i < kBooks; i++) {
    if (s_store.books[i].hash == hash) return &s_store.books[i];
    if (s_store.books[i].lastDay < lru->lastDay || s_store.books[i].hash == 0) {
      if (lru->hash != 0) lru = &s_store.books[i];
    }
  }
  *lru = BookRec{hash, 0, 0, 0};
  return lru;
}

}  // namespace

void ReadingStats::load() {
  if (s_loaded) return;
  s_loaded = true;
  memset(&s_store, 0, sizeof(s_store));
  s_store.magic = kMagic;
  s_store.version = kVersion;
  HalFile f;
  if (!Storage.openFileForRead("STA", kPath, f)) return;  // first run
  Store fromDisk;
  const size_t got = f.read(reinterpret_cast<uint8_t*>(&fromDisk), sizeof(fromDisk));
  if (got == sizeof(fromDisk) && fromDisk.magic == kMagic && fromDisk.version == kVersion) {
    s_store = fromDisk;
  } else {
    LOG_ERR("STA", "stats.bin invalid (%u bytes) — starting fresh", (unsigned)got);
  }
}

bool ReadingStats::save() {
  // ProgressFile's crash-safety pattern: full write to a temp, then
  // remove + rename. A torn write costs the temp file, never stats.bin.
  {
    HalFile f;
    if (!Storage.openFileForWrite("STA", kTmpPath, f)) {
      LOG_ERR("STA", "stats tmp open failed");
      return false;
    }
    if (f.write(reinterpret_cast<const uint8_t*>(&s_store), sizeof(s_store)) != sizeof(s_store)) {
      LOG_ERR("STA", "stats short write");
      return false;
    }
    f.flush();
  }
  Storage.remove(kPath);
  if (!Storage.rename(kTmpPath, kPath)) {
    LOG_ERR("STA", "stats rename failed");
    return false;
  }
  return true;
}

void ReadingStats::sessionStart(const std::string& bookPath) {
  const uint32_t h = fnv1a(bookPath.c_str());
  if (s_active && h == s_hash) return;
  if (s_active) sessionEnd();  // book switch closes the previous session
  load();
  s_active = true;
  s_hash = h;
  s_startMs = millis();
  s_pages = 0;
}

void ReadingStats::pageTurn() {
  if (s_active && s_pages < 0xFFFF) s_pages++;
}

void ReadingStats::sessionEnd() {
  if (!s_active) return;
  s_active = false;
  const uint32_t ms = millis() - s_startMs;
  if (s_pages == 0 && ms < 30000u) return;  // opened and bounced — not a session
  uint32_t roundedMin = (ms + 30000u) / 60000u;
  if (roundedMin == 0) roundedMin = 1;
  const uint16_t minutes = static_cast<uint16_t>(roundedMin > 0xFFFF ? 0xFFFF : roundedMin);

  const uint32_t today = todayYmd();
  if (today != 0) {
    DayRec* d = dayRec(today, true);
    d->pages = static_cast<uint16_t>(d->pages + s_pages);
    d->minutes = static_cast<uint16_t>(d->minutes + minutes);
  }
  BookRec* b = bookRec(s_hash);
  b->pages += s_pages;
  b->minutes += minutes;
  if (today != 0) b->lastDay = today;

  const bool ok = save();
  LOG_DBG("STA", "session flush: %u pages, %u min, day %u -> %s", s_pages, minutes, today,
          ok ? "saved" : "SAVE FAILED");
  s_pages = 0;
}

ReadingStats::Band ReadingStats::band() {
  load();
  Band out;
  memset(&out, 0, sizeof(out));
  const uint32_t today = todayYmd();
  out.clockValid = today != 0;
  if (!out.clockValid) return out;

  const int32_t todaySerial = serialFromYmd(today);
  // days-from-civil for 1970-01-01 is 0 and it was a Thursday; 0=Mon indexing.
  out.todayWeekday = static_cast<uint8_t>(((todaySerial % 7) + 7 + 3) % 7);

  for (int back = 0; back < 7; back++) {
    const DayRec* d = dayRec(ymdFromSerial(todaySerial - back), false);
    out.weekPages[6 - back] = d ? d->pages : 0;
  }
  out.todayPages = out.weekPages[6];

  // Streak: consecutive read-days ending today — or yesterday, so the flame
  // doesn't reset to 0 at midnight before today's first page.
  int32_t cursor = todaySerial;
  const DayRec* t = dayRec(today, false);
  if (!t || t->pages == 0) cursor--;
  uint16_t streak = 0;
  while (streak < 9999) {
    const DayRec* d = dayRec(ymdFromSerial(cursor), false);
    if (!d || d->pages == 0) break;
    streak++;
    cursor--;
  }
  out.streakDays = streak;
  return out;
}

std::string ReadingStats::toJson() {
  load();
  const Band b = band();
  std::string out;
  out.reserve(1024);
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"clockValid\":%s,\"streak\":%u,\"todayPages\":%u,\"days\":[",
           b.clockValid ? "true" : "false", b.streakDays, b.todayPages);
  out += buf;
  bool first = true;
  for (int i = 0; i < kDays; i++) {
    const DayRec& d = s_store.days[i];
    if (d.day == 0) continue;
    snprintf(buf, sizeof(buf), "%s{\"day\":%u,\"pages\":%u,\"minutes\":%u}", first ? "" : ",",
             d.day, d.pages, d.minutes);
    out += buf;
    first = false;
  }
  out += "],\"books\":[";
  first = true;
  for (int i = 0; i < kBooks; i++) {
    const BookRec& r = s_store.books[i];
    if (r.hash == 0) continue;
    snprintf(buf, sizeof(buf), "%s{\"hash\":%u,\"pages\":%u,\"minutes\":%u,\"lastDay\":%u}",
             first ? "" : ",", r.hash, r.pages, r.minutes, r.lastDay);
    out += buf;
    first = false;
  }
  out += "]}";
  return out;
}

}  // namespace reader
