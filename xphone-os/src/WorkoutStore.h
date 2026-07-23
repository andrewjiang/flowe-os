#pragma once

// xphone-os Workout — dedicated workout store (PrioritiesStore's mold).
//
// Holds today's workout: up to 8 exercises, each "N sets of <free text>"
// with a completed-set count. The companion service copies every
// workout.snapshot card into this store at the parse point
// (CompanionBleService.cpp applyCardPayload), so the snapshot is captured
// even if the Workout scene was never opened — the dormant sleep frame reads
// the store directly when the device sleeps from the Workout scene.
//
// The device is increment-only: +/- adjust `done` locally (bumpDone) for an
// instant optimistic repaint, and the absolute new count is sent to the
// phone as a workout.set action (idempotent — a retried packet can't
// double-count). The phone remains the source of truth for structure; a
// fresh card replaces everything here.
//
// Main-loop-only by construction (same reasoning as PrioritiesStore): no
// mutex needed.

#include <cstddef>
#include <cstdint>

struct CompanionCardState;

class WorkoutStore {
 public:
  static constexpr std::size_t CAPACITY = 8;   // == MAX_WORKOUT_ITEMS

  struct Item {
    char id[65] = {0};    // echoed back in workout.set — kept verbatim
    char name[49] = {0};  // free text: "Pushups x10", "Bench 135lb"
    int sets = 0;         // target set count (1..99)
    int done = 0;         // completed sets (0..sets)
  };

  // Copy the card's workout items + date into the fixed buffers and bump the
  // revision. Call ONLY for workout snapshot cards — the service's predicate
  // decides.
  void updateFromCard(const CompanionCardState& card);

  std::size_t count() const { return _count; }
  bool get(std::size_t index, Item& out) const;

  // Optimistic local +/-: clamp done to [0, sets], return the NEW done count,
  // or -1 when index is invalid or the value didn't change (already at a
  // clamp edge — callers skip the repaint and the BLE send). Bumps revision.
  int bumpDone(std::size_t index, int delta);

  // Exercises fully done / total, for the header line and the sleep frame.
  void tally(int& doneExercises, int& total) const;
  bool allDone() const;

  // The phone-local date the card was built for ("" until a card lands).
  const char* date() const { return _date; }

  // Bumped on every updateFromCard() AND bumpDone(); the main loop polls
  // this to mark the Workout scene dirty.
  uint32_t revision() const { return _revision; }

 private:
  Item _items[CAPACITY];
  std::size_t _count = 0;
  char _date[17] = {0};
  uint32_t _revision = 0;
};

extern WorkoutStore WORKOUT_STORE;
