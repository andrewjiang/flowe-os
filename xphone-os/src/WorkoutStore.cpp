#include "WorkoutStore.h"

#include <cstdio>
#include <cstring>

#include "ble/CompanionProtocol.h"

WorkoutStore WORKOUT_STORE;

static_assert(WorkoutStore::CAPACITY == CompanionProtocol::MAX_WORKOUT_ITEMS,
              "WorkoutStore capacity must mirror the protocol");
static_assert(sizeof(WorkoutStore::Item::id) == CompanionProtocol::MAX_ID_CHARS + 1,
              "Item::id must fit MAX_ID_CHARS + NUL");
static_assert(sizeof(WorkoutStore::Item::name) == CompanionProtocol::MAX_WORKOUT_NAME_CHARS + 1,
              "Item::name must fit MAX_WORKOUT_NAME_CHARS + NUL");

void WorkoutStore::updateFromCard(const CompanionCardState& card) {
  // Same-day stale-snapshot guard: sets counted on the device while the phone
  // was away must not be regressed by a snapshot the phone built before it
  // heard about them. When the incoming card is for the SAME date, done takes
  // max(local, incoming) per matching id; a new date (daily reset) or a
  // structural change replaces outright. (Trade-off: a same-day manual
  // downward edit on the phone loses to fresher local progress — acceptable,
  // the phone reconverges on the next workout.set.)
  Item old[CAPACITY];
  std::size_t oldCount = 0;
  const bool sameDate = !card.workoutDate.empty() && strcmp(_date, card.workoutDate.c_str()) == 0;
  if (sameDate) {
    oldCount = _count;
    for (std::size_t i = 0; i < oldCount; i++) old[i] = _items[i];
  }

  _count = card.workoutItemCount < CAPACITY ? card.workoutItemCount : CAPACITY;
  for (std::size_t i = 0; i < _count; i++) {
    const auto& src = card.workoutItems[i];
    Item& dst = _items[i];
    snprintf(dst.id, sizeof(dst.id), "%s", src.id.c_str());
    snprintf(dst.name, sizeof(dst.name), "%s", src.name.c_str());
    dst.sets = src.sets < 0 ? 0 : (src.sets > 99 ? 99 : src.sets);
    dst.done = src.done < 0 ? 0 : (src.done > dst.sets ? dst.sets : src.done);
    for (std::size_t j = 0; j < oldCount; j++) {
      if (strcmp(old[j].id, dst.id) == 0) {
        if (old[j].done > dst.done && old[j].done <= dst.sets) dst.done = old[j].done;
        break;
      }
    }
  }
  snprintf(_date, sizeof(_date), "%s", card.workoutDate.c_str());
  ++_revision;
}

bool WorkoutStore::get(const std::size_t index, Item& out) const {
  if (index >= _count) return false;
  out = _items[index];
  return true;
}

int WorkoutStore::bumpDone(const std::size_t index, const int delta) {
  if (index >= _count) return -1;
  Item& item = _items[index];
  int next = item.done + delta;
  if (next < 0) next = 0;
  if (next > item.sets) next = item.sets;
  if (next == item.done) return -1;  // clamp edge — nothing changed
  item.done = next;
  ++_revision;
  return next;
}

void WorkoutStore::tally(int& doneExercises, int& total) const {
  doneExercises = 0;
  total = static_cast<int>(_count);
  for (std::size_t i = 0; i < _count; i++) {
    if (_items[i].sets > 0 && _items[i].done >= _items[i].sets) ++doneExercises;
  }
}

bool WorkoutStore::allDone() const {
  if (_count == 0) return false;
  int done = 0, total = 0;
  tally(done, total);
  return done == total;
}
