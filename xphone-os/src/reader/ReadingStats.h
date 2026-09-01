#pragma once

// Reading stats, pages-first (docs/plans/2026-07-29-reading-stats-design.md).
//
// One ~1.5 KB stats.bin at /.xphone/stats.bin holds a 64-day ring of
// {day, pages, minutes} plus a 64-book table of lifetime totals — rewritten
// atomically (ProgressFile pattern) at each session flush. No append log, no
// scans: the grid band reads a preloaded struct, growth is bounded forever.
//
// Time costs nothing to record (two millis() reads per session, no wakeups),
// but PAGES are the honest e-ink metric — a turn is an observed event, time
// includes the fell-asleep tail (bounded by auto-sleep). The band shows pages;
// minutes ride along in the same records for the app's charts.
//
// The device has no RTC: "today" derives from the phone-synced ClockStore
// (yyyymmdd + minutes-into-day + millis() elapsed since sync). With no sync
// since boot (day == 0) sessions still update book totals but skip day/streak
// attribution rather than inventing dates.

#include <cstdint>
#include <string>

namespace reader {

class ReadingStats {
 public:
  // Session lifecycle, driven by ReaderScene. start/end pairs are idempotent:
  // a second start() for the same book is a no-op, end() without start is too.
  static void sessionStart(const std::string& bookPath);
  static void pageTurn();
  static void sessionEnd();

  struct Band {
    uint16_t streakDays;    // consecutive days with >=1 page, ending today/yesterday
    uint16_t todayPages;    // 0 when clock unknown
    uint16_t weekPages[7];  // [0]=6 days ago .. [6]=today
    uint8_t todayWeekday;   // 0=Mon .. 6=Sun (for M T W T F S S labels)
    bool clockValid;        // false -> render band in "no clock yet" form
  };
  static Band band();

  // Per-book lifetime totals for the reader menu's stats page. Includes
  // the OPEN session's live pages/time when it is this book. Returns
  // false when the book has no recorded reading yet (and no session).
  static bool bookStats(const std::string& bookPath, uint32_t* pages, uint32_t* minutes,
                        uint32_t* lastDay, uint32_t* firstDay = nullptr,
                        uint16_t* daysRead = nullptr);

  // Today's recorded reading minutes plus the open session's live time
  // (0 when the clock is unknown).
  static uint16_t todayMinutes();

  // Everything the full stats page shows beyond the band: records and
  // lifetime totals derived from the same 64-day ring and book table.
  struct Summary {
    uint16_t longestStreak;   // best consecutive-day run inside the ring
    uint16_t bestDayMinutes;  // single best day inside the ring
    uint32_t lifetimeMinutes;
    uint32_t lifetimePages;
    uint16_t booksThisYear;   // books with reading recorded this calendar year
    uint16_t booksAllTime;    // books with ANY recorded reading — needs no clock,
                              // so the stats page still has something to say
                              // before the phone has ever set the date
    uint32_t monthMask;       // bit (d-1) set = read on day d of this month
    uint8_t monthDays;        // days in the current month
    uint8_t monthRead;        // how many of them have reading
    bool clockValid;
  };
  static Summary summary();

  // Today as yyyymmdd from the phone-synced clock (0 = never synced).
  // Exposed so the stats page can title itself with the real month.
  static uint32_t todayYmdPublic();

  // Whole store as JSON for the transfer server's /stats endpoint.
  static std::string toJson();

  // Erase everything recorded — the day ring, the book table, and any open
  // session's accumulated pages/time (flowe-os#44). An open session keeps
  // running from zero, so reading after the reset counts normally.
  static bool resetAll();

 private:
  static void load();
  static bool save();
};

}  // namespace reader
