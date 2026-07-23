#include "PrioritiesStore.h"

#include <cstdio>
#include <cstring>

#include "ble/CompanionProtocol.h"

PrioritiesStore PRIORITIES_STORE;

// Keep the fixed fields sized to what the wire protocol can deliver (see the
// header comment) — a protocol bump must grow the store with it.
static_assert(PrioritiesStore::CAPACITY == CompanionProtocol::MAX_PRIORITY_ITEMS, "store capacity != protocol");
static_assert(sizeof(PrioritiesStore::Item::id) == CompanionProtocol::MAX_ID_CHARS + 1, "id field != protocol");
static_assert(sizeof(PrioritiesStore::Item::title) == CompanionProtocol::MAX_TITLE_CHARS + 1,
              "title field != protocol");
static_assert(sizeof(PrioritiesStore::Item::note) == CompanionProtocol::MAX_PRIORITY_FIELD_CHARS + 1,
              "note field != protocol");

void PrioritiesStore::updateFromCard(const CompanionCardState& card) {
  // A new snapshot id (or an explicit first part) restarts the staging
  // cursor; a plain single-part card is just the degenerate one-slice case.
  if (card.part == 0 || strcmp(_stageId, card.id.c_str()) != 0) {
    _stageCount = 0;
    snprintf(_stageId, sizeof(_stageId), "%s", card.id.c_str());
  }
  for (std::size_t i = 0; i < card.priorityItemCount && _stageCount < CAPACITY; i++, _stageCount++) {
    const CompanionPriorityItem& src = card.priorityItems[i];
    Item& dst = _items[_stageCount];
    // snprintf clips + always null-terminates (strncpy does not).
    snprintf(dst.id, sizeof(dst.id), "%s", src.id.c_str());
    snprintf(dst.title, sizeof(dst.title), "%s", src.title.c_str());
    snprintf(dst.note, sizeof(dst.note), "%s", src.note.c_str());
    dst.done = src.done;
  }
  // Commit only on the final part: count/sync line/revision move together, so
  // renders between parts (~one connection interval apart) see the OLD list,
  // never a truncated new one. A dropped final part leaves the old snapshot
  // on glass; the next sync repairs it.
  if (card.part + 1 >= card.parts) {
    _count = _stageCount;
    snprintf(_syncLine, sizeof(_syncLine), "%s", card.body.c_str());
    _revision++;
  }
}

bool PrioritiesStore::get(std::size_t index, Item& out) const {
  if (index >= _count) return false;
  out = _items[index];
  return true;
}

void PrioritiesStore::tally(int& active, int& done) const {
  active = 0;
  done = 0;
  for (std::size_t i = 0; i < _count; i++) {
    if (_items[i].done) {
      done++;
    } else {
      active++;
    }
  }
}
