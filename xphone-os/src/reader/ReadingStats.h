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

  // Whole store as JSON for the transfer server's /stats endpoint.
  static std::string toJson();

 private:
  static void load();
  static bool save();
};

}  // namespace reader
