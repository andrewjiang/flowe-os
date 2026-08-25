#pragma once

// xphone-os M3/M4 — dedicated priorities store.
//
// NotificationStore's mold applied to the Priorities snapshot: fixed char
// fields, no std::string, no heap growth, ~2.9KB static RAM total. The
// companion service copies every priorities snapshot card into this store at
// the parse point (CompanionBleService.cpp applyCardPayload), so the snapshot
// is captured even if the Priorities scene was never opened — the same
// service-level capture x4-os does (its CompanionBleService.cpp:504-506 keeps
// a prioritiesSnapshot member and :518-520 persists it to
// /.xphone/priorities.json on every card; SD persistence is out of scope
// here, this store is RAM-only). Consumers are the Priorities scene and the
// dormant sleep frame.
//
// applyCardPayload runs ONLY on the Arduino main loop (handleCardWrite on the
// NimBLE host task just stashes raw bytes; processPending() parses them from
// loop()/Sleep.cpp), and scenes + Sleep render on the main loop too, so every
// accessor here is main-loop-only by construction: no mutex needed.

#include <cstddef>
#include <cstdint>

struct CompanionCardState;

class PrioritiesStore {
 public:
  // Capacity/field sizes mirror CompanionProtocol (MAX_PRIORITY_ITEMS,
  // MAX_ID_CHARS + NUL, MAX_TITLE_CHARS + NUL, MAX_PRIORITY_FIELD_CHARS +
  // NUL); static_asserts in the .cpp keep them in lockstep.
  static constexpr std::size_t CAPACITY = 10;

  struct Item {
    char id[65] = {0};     // what priority.toggle sends back — kept verbatim
    char title[97] = {0};
    char note[121] = {0};
    bool done = false;
  };  // 284 bytes

  // Copy the card's priority items + its body ("N active / M done" from the
  // iPhone) into the fixed buffers and bump the revision. Call ONLY when the
  // card actually is a priorities snapshot — the service's predicate decides.
  // Multi-part snapshots (card.parts > 1, slices sharing one card id) stage
  // into the item slots per part and commit count/sync line/revision on the
  // final part, so a mid-assembly render never sees a half list.
  void updateFromCard(const CompanionCardState& card);

  std::size_t count() const { return _count; }
  // Bounds-checked copy-out; returns false when index >= count().
  bool get(std::size_t index, Item& out) const;
  // Active/done tallies for the progress lines.
  void tally(int& active, int& done) const;
  // The snapshot card's body line ("" until the first snapshot lands).
  const char* syncLine() const { return _syncLine; }

  // Bumped on every updateFromCard() AND setDoneLocal(); the main loop polls
  // this to mark the Priorities scene dirty, and Sleep.cpp polls it to detect
  // the sync reply.
  uint32_t revision() const { return _revision; }

  // --- Optimistic offline toggles (durable pending overlay) -----------------
  // A DONE press flips the item at once (setDoneLocal) and records the desired
  // state keyed by item id. updateFromCard keeps that desired state on top of
  // any snapshot the phone sends until the phone's snapshot agrees, so a stale
  // snapshot can't undo an offline toggle — the same durability the
  // notification tombstones give clears. main.cpp re-sends the pending toggles
  // on the reconnect edge; the phone's confirming snapshot then clears them.
  //
  // Flip _items[index].done to `done`, record/refresh a pending entry keyed by
  // its id, and bump the revision. Returns false on a bad index or an item
  // with no id (nothing addressable to send).
  bool setDoneLocal(std::size_t index, bool done);
  // Unsent toggles waiting for the phone. main.cpp enumerates these on the
  // reconnect edge and re-sends each as a priority.toggle.
  std::size_t pendingCount() const { return _pendingCount; }
  bool pendingGet(std::size_t i, char (&idOut)[65], bool& doneOut) const;

 private:
  Item _items[CAPACITY];
  std::size_t _count = 0;
  char _syncLine[97] = {0};
  uint32_t _revision = 0;
  // Multi-part assembly cursor: the card id the staged slices belong to and
  // how many item slots they have filled so far.
  char _stageId[65] = {0};
  std::size_t _stageCount = 0;

  // Pending optimistic toggles: one desired state per item id, at most one per
  // item so CAPACITY is an ample bound. Reconciled against every committed
  // snapshot (see updateFromCard).
  struct Pending {
    char id[65] = {0};
    bool done = false;
  };
  Pending _pending[CAPACITY];
  std::size_t _pendingCount = 0;
  void recordPending(const char* id, bool done);
};

extern PrioritiesStore PRIORITIES_STORE;
