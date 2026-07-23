#include "NotificationStore.h"

#include <Arduino.h>

#include <cstdio>
#include <cstring>

NotificationStore NOTIFICATION_STORE;

namespace {
void copyField(char* dst, std::size_t dstSize, const char* src) {
  if (!src) src = "";
  // snprintf clips + always null-terminates (strncpy does not).
  snprintf(dst, dstSize, "%s", src);
}
}  // namespace

void NotificationStore::add(uint32_t uid, const char* appId, const char* title, const char* message,
                            uint64_t dateKey, uint8_t categoryId, uint8_t flags, uint32_t sessionId) {
  // Update in place if this UID is already stored (ANCS "modified" events).
  for (std::size_t i = 0; i < _used; i++) {
    Entry& e = _entries[(_head + CAPACITY - 1 - i) % CAPACITY];
    if (e.uid == uid) {
      if (dateKey != 0) e.sortKey = dateKey;  // a modified event may carry a fresh date
      e.timestampMs = millis();
      e.sessionId = sessionId;
      e.categoryId = categoryId;
      e.flags = flags;
      copyField(e.appId, sizeof(e.appId), appId);
      copyField(e.title, sizeof(e.title), title);
      copyField(e.message, sizeof(e.message), message);
      reorderFromNewest(i);  // date may have moved this entry's rank
      _revision++;
      return;
    }
  }

  // Same notification re-ingested under a NEW UID: ANCS UIDs are per-connection,
  // so after a sleep/wake the backfill replays entries the persisted seed
  // (restore()) already holds. Match by date+title and update in place —
  // adopting the fresh UID so future modified/removed events find it.
  if (dateKey != 0) {
    for (std::size_t i = 0; i < _used; i++) {
      Entry& e = _entries[(_head + CAPACITY - 1 - i) % CAPACITY];
      if (e.sortKey == dateKey && strcmp(e.title, title ? title : "") == 0) {
        e.uid = uid;
        e.timestampMs = millis();
        e.sessionId = sessionId;
        e.categoryId = categoryId;
        e.flags = flags;
        copyField(e.appId, sizeof(e.appId), appId);
        copyField(e.message, sizeof(e.message), message);
        _revision++;
        return;
      }
    }
  }

  // New entry. _head is the oldest logical slot (lowest sortKey once the ring
  // is full), so overwriting it drops the oldest-by-date entry — exactly the
  // "keep the most-recent" policy. Place at _head, then bubble into date order.
  Entry& slot = _entries[_head];
  _head = (_head + 1) % CAPACITY;
  if (_used < CAPACITY) _used++;
  slot.uid = uid;
  slot.timestampMs = millis();
  slot.sortKey = dateKey;
  slot.sessionId = sessionId;
  slot.categoryId = categoryId;
  slot.flags = flags;
  copyField(slot.appId, sizeof(slot.appId), appId);
  copyField(slot.title, sizeof(slot.title), title);
  copyField(slot.message, sizeof(slot.message), message);
  reorderFromNewest(0);  // the just-written entry is at newest-first index 0
  _revision++;
}

// Keep the used block sorted ascending by sortKey (so newest-first index 0 has
// the highest key). The block is sorted before the call, so the entry at
// `idx` is the only one possibly out of place: bubble it one direction.
void NotificationStore::reorderFromNewest(std::size_t idx) {
  // Toward OLDER (higher newest-index) while this entry is older (lower key).
  while (idx + 1 < _used && atNewest(idx).sortKey < atNewest(idx + 1).sortKey) {
    Entry tmp = atNewest(idx);
    atNewest(idx) = atNewest(idx + 1);
    atNewest(idx + 1) = tmp;
    ++idx;
  }
  // Toward NEWER (lower newest-index) while this entry is newer (higher key).
  while (idx > 0 && atNewest(idx).sortKey > atNewest(idx - 1).sortKey) {
    Entry tmp = atNewest(idx);
    atNewest(idx) = atNewest(idx - 1);
    atNewest(idx - 1) = tmp;
    --idx;
  }
}

bool NotificationStore::get(std::size_t newestIndex, Entry& out) const {
  if (newestIndex >= _used) return false;
  out = _entries[(_head + CAPACITY - 1 - newestIndex) % CAPACITY];
  return true;
}

void NotificationStore::markUidStale(uint32_t uid) {
  for (std::size_t i = 0; i < _used; ++i) {
    Entry& e = _entries[(_head + CAPACITY - 1 - i) % CAPACITY];
    if (e.uid == uid) {
      e.sessionId = 0;
      return;
    }
  }
}

bool NotificationStore::removeAt(std::size_t newestIndex) {
  if (newestIndex >= _used) return false;
  // Physical slot of newest-first index j is (_head + CAPACITY - 1 - j) %
  // CAPACITY. Overwrite the removed slot by sliding each NEWER entry one
  // slot toward it (walk j from the removed index up to the newest)...
  for (std::size_t j = newestIndex; j > 0; j--) {
    _entries[(_head + CAPACITY - 1 - j) % CAPACITY] = _entries[(_head + CAPACITY - j) % CAPACITY];
  }
  // ...then retire the now-duplicated newest slot: _head steps back over it
  // (it becomes the next write slot) and is scrubbed so stale text never
  // lingers in a "free" slot.
  _head = (_head + CAPACITY - 1) % CAPACITY;
  _entries[_head] = Entry{};
  _used--;
  _revision++;
  return true;
}

// M5 sleep persistence: pack the newest entries (raw trivially-copyable
// structs) into `out`, newest first. Returns bytes written.
std::size_t NotificationStore::snapshot(uint8_t* out, std::size_t cap) const {
  std::size_t n = 0;
  for (std::size_t i = 0; i < _used; i++) {
    if (n + sizeof(Entry) > cap) break;
    const Entry& e = _entries[(_head + CAPACITY - 1 - i) % CAPACITY];
    memcpy(out + n, &e, sizeof(Entry));
    n += sizeof(Entry);
  }
  return n;
}

// Seed from a persisted snapshot (boot, before BLE). Entries re-add through
// add() (keeps date ordering); UIDs get bit 30 set so they can never collide
// with this session's live ANCS UIDs — the date+title dedupe in add() then
// folds the backfill's re-ingest into these rows instead of duplicating.
void NotificationStore::restore(const uint8_t* data, std::size_t len) {
  // Entry is persisted as a raw struct. Reject a stale firmware's blob rather
  // than interpreting shifted string/metadata fields after sizeof(Entry)
  // changes (this release grows Entry from 216 to 224 bytes).
  if (!data || len == 0 || len % sizeof(Entry) != 0) return;
  const std::size_t count = len / sizeof(Entry);
  if (count > CAPACITY) return;
  // Oldest first so add()'s newest-first ordering lands naturally.
  for (std::size_t i = count; i > 0; i--) {
    Entry e;
    memcpy(&e, data + (i - 1) * sizeof(Entry), sizeof(Entry));
    e.appId[sizeof(e.appId) - 1] = '\0';  // defensive NUL after raw copy
    e.title[sizeof(e.title) - 1] = '\0';
    e.message[sizeof(e.message) - 1] = '\0';
    add(e.uid | RESTORED_UID_BIT, e.appId, e.title, e.message, e.sortKey, e.categoryId, e.flags,
        /*sessionId=*/0);
  }
}

bool NotificationStore::recordTombstone(uint64_t dateKey, const char* title) {
  // Undated ANCS notifications cannot be identified safely across sessions:
  // title-only matching could hide an unrelated future notification. The
  // caller still attempts an immediate dismiss with the live UID.
  if (dateKey == 0) return false;
  if (!title) title = "";

  // Re-clearing the same replay is idempotent and should not consume another
  // slot in the small fixed ring.
  for (std::size_t i = 0; i < _tombstoneUsed; ++i) {
    const Tombstone& t = _tombstones[(_tombstoneHead + TOMBSTONE_CAPACITY - 1 - i) % TOMBSTONE_CAPACITY];
    if (t.sortKey == dateKey && strcmp(t.title, title) == 0) return true;
  }

  Tombstone& slot = _tombstones[_tombstoneHead];
  slot.sortKey = dateKey;
  copyField(slot.title, sizeof(slot.title), title);
  _tombstoneHead = (_tombstoneHead + 1) % TOMBSTONE_CAPACITY;
  if (_tombstoneUsed < TOMBSTONE_CAPACITY) ++_tombstoneUsed;
  return true;
}

bool NotificationStore::isTombstoned(uint64_t dateKey, const char* title) const {
  if (dateKey == 0) return false;
  if (!title) title = "";
  for (std::size_t i = 0; i < _tombstoneUsed; ++i) {
    const Tombstone& t = _tombstones[(_tombstoneHead + TOMBSTONE_CAPACITY - 1 - i) % TOMBSTONE_CAPACITY];
    if (t.sortKey == dateKey && strcmp(t.title, title) == 0) return true;
  }
  return false;
}

std::size_t NotificationStore::tombstoneSnapshot(uint8_t* out, std::size_t cap) const {
  if (!out) return 0;
  std::size_t n = 0;
  for (std::size_t i = 0; i < _tombstoneUsed; ++i) {
    if (n + sizeof(Tombstone) > cap) break;
    const Tombstone& t = _tombstones[(_tombstoneHead + TOMBSTONE_CAPACITY - 1 - i) % TOMBSTONE_CAPACITY];
    memcpy(out + n, &t, sizeof(Tombstone));
    n += sizeof(Tombstone);
  }
  return n;
}

void NotificationStore::restoreTombstones(const uint8_t* data, std::size_t len) {
  if (!data || len == 0 || len % sizeof(Tombstone) != 0) return;
  const std::size_t count = len / sizeof(Tombstone);
  if (count > TOMBSTONE_CAPACITY) return;

  // Snapshot order is newest-first; replay oldest-first into the ring.
  for (std::size_t i = count; i > 0; --i) {
    Tombstone t;
    memcpy(&t, data + (i - 1) * sizeof(Tombstone), sizeof(Tombstone));
    t.title[sizeof(t.title) - 1] = '\0';
    recordTombstone(t.sortKey, t.title);
  }
}

void NotificationStore::clearAll() {
  for (auto& e : _entries) e = Entry{};
  _head = 0;
  _used = 0;
  _revision++;
}
