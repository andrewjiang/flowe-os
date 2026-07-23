#pragma once

// xphone-os M3 — dedicated Today store.
//
// PrioritiesStore's mold applied to the "today.snapshot" card: fixed char
// fields, no std::string, no heap growth, ~1.9KB static RAM total. The
// companion service copies every today snapshot into this store at the parse
// point (CompanionBleService.cpp applyCardPayload, predicate identical to
// x4-os TodayActivity.cpp:13-15 isTodayCard), because the service holds only
// ONE CompanionCardState — any later card (a Block status push, a priorities
// snapshot) would otherwise clobber the today data while the Today scene is
// off glass.
//
// applyCardPayload runs ONLY on the Arduino main loop (handleCardWrite on the
// NimBLE host task just stashes raw bytes; processPending() parses them from
// loop()), and scenes render on the main loop too, so every accessor here is
// main-loop-only by construction: no mutex needed.

#include <cstddef>
#include <cstdint>

struct CompanionCardState;

class TodayStore {
 public:
  // Capacity/field sizes mirror CompanionProtocol (MAX_TODAY_ITEMS,
  // MAX_ID_CHARS + NUL, MAX_TODAY_FIELD_CHARS + NUL, MAX_TITLE_CHARS + NUL);
  // static_asserts in the .cpp keep them in lockstep.
  static constexpr std::size_t CAPACITY = 6;

  struct Item {
    char kind[65] = {0};      // "reminder" selects the Reminders section; anything else is agenda
    char time[49] = {0};      // e.g. "9:00 AM" ("" for undated reminders)
    char title[97] = {0};
    char subtitle[49] = {0};  // "" -> the scene falls back to `kind`, like x4-os TodayActivity
  };  // 260 bytes

  // Copy the card's today items + weather/high-low/sync lines into the fixed
  // buffers and bump the revision. Call ONLY when the card actually is a
  // today snapshot — the service's predicate decides.
  void updateFromCard(const CompanionCardState& card);

  bool hasSnapshot() const { return _hasSnapshot; }
  std::size_t count() const { return _count; }
  // Bounds-checked copy-out; returns false when index >= count().
  bool get(std::size_t index, Item& out) const;
  // Agenda (non-reminder) / reminder tallies for the section headers.
  void tally(int& agenda, int& reminders) const;
  const char* weather() const { return _weather; }    // "" when the card had none
  const char* highLow() const { return _highLow; }
  const char* syncLine() const { return _syncLine; }  // e.g. "Synced 9:41 AM"

  // Bumped on every updateFromCard(); the main loop polls this to mark the
  // Today scene dirty (e-ink discipline: no redraws from BLE paths).
  uint32_t revision() const { return _revision; }

 private:
  Item _items[CAPACITY];
  std::size_t _count = 0;
  bool _hasSnapshot = false;
  char _weather[97] = {0};
  char _highLow[97] = {0};
  char _syncLine[97] = {0};
  uint32_t _revision = 0;
};

extern TodayStore TODAY_STORE;
