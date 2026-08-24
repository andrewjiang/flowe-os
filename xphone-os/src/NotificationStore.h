#pragma once

// xphone-os M2 — bounded notification store.
//
// Fixed-size ring buffer of 24 entries with fixed char fields — no
// std::string, no heap growth, ~5.3KB static RAM total. ANCS notifications
// are copied out of the ANCS client's fixed parse buffers into
// this store at the hand-off point (CompanionAncsClient.cpp handleDataSource,
// which runs on the MAIN loop task — see the ANCS packet queue there), so
// every accessor here is main-loop-only by construction: no mutex needed.
// Scenes read it with count()/get(i) (newest-first) and render via snprintf
// into stack buffers.

#include <cstddef>
#include <cstdint>

class NotificationStore {
 public:
  // M2.1d right-sizing: the Notifications scene renders title and message
  // each truncated to ONE line (~50-60 visible chars at 792 px with margins),
  // so 56/112-byte fields never clip anything that was going to render, and
  // 24 entries is still several full scroll pages. Metadata brings the current
  // Entry layout to 224 B, or 5,376 B for the ring.
  static constexpr std::size_t CAPACITY = 24;

  // Field sizes (bytes incl. NUL). Shared with the ANCS parse path
  // (CompanionAncsClient.cpp) so its fixed staging buffers shrink/grow in
  // lockstep with the store — nothing is parsed larger than it can be kept.
  static constexpr std::size_t APP_ID_CHARS = 32;   // e.g. "com.google.ios.youtube"
  static constexpr std::size_t TITLE_CHARS = 56;
  static constexpr std::size_t MESSAGE_CHARS = 112;
  static constexpr uint32_t RESTORED_UID_BIT = 0x40000000u;
  // Synthetic UIDs for phone-pushed notifications (companion "notif.push"
  // card — Android has no ANCS). Distinct from RESTORED_UID_BIT so a pushed
  // entry can never collide with a live ANCS UID or a restored one; after a
  // sleep/wake restore the two bits compose (0x20000001 -> 0x60000001), which
  // stays outside both live namespaces.
  static constexpr uint32_t PUSHED_UID_BIT = 0x20000000u;
  static constexpr std::size_t TOMBSTONE_CAPACITY = 8;

  struct Entry {
    uint32_t uid = 0;          // ANCS notification UID
    uint32_t timestampMs = 0;  // millis() at store time
    uint64_t sortKey = 0;      // chronological key from the ANCS date (see add())
    // Runtime ANCS connection generation. Restored entries always use 0, and
    // prior-session entries retain their old generation, so CLEAR never sends
    // an action using a UID that is no longer valid on the current connection.
    uint32_t sessionId = 0;
    char appId[APP_ID_CHARS] = {0};
    char title[TITLE_CHARS] = {0};
    char message[MESSAGE_CHARS] = {0};
    uint8_t categoryId = 0;
    uint8_t flags = 0;
  };  // 224 bytes on ESP32 (raw snapshot format; restore rejects old sizes)

  struct Tombstone {
    uint64_t sortKey = 0;
    char title[TITLE_CHARS] = {0};
  };

  // Insert (or update in place when `uid` is already stored — ANCS re-sends
  // attributes for modified notifications). Overwrites the oldest entry (by
  // date, i.e. the lowest sortKey) when full. Null/overlong inputs are clipped
  // into the fixed fields.
  //
  // `dateKey` is a chronological sort key derived from the notification's own
  // ANCS date (YYYYMMDDHHMMSS as a decimal, 0 when unknown). The used block is
  // kept sorted ascending by this key so get(0) is always the NEWEST BY DATE —
  // the reconnect backfill can therefore replay preexisting notifications in
  // any order and the store still presents them newest-first. A 0 key sinks to
  // the oldest slot (so undated entries are the first dropped when full).
  void add(uint32_t uid, const char* appId, const char* title, const char* message, uint64_t dateKey,
           uint8_t categoryId, uint8_t flags, uint32_t sessionId);

  std::size_t count() const { return _used; }
  // Newest-first access: index 0 is the most recently added entry.
  // Returns false when newestIndex >= count().
  bool get(std::size_t newestIndex, Entry& out) const;
  // The store is an inbox log and retains rows after iOS removes them, but the
  // UID is no longer actionable. Clear only its runtime session provenance.
  void markUidStale(uint32_t uid);
  void clearAll();

  // M5 sleep persistence (Sleep.cpp): pack/unpack the newest entries as a raw
  // blob for NVS. snapshot() returns bytes written (multiple of sizeof(Entry));
  // restore() re-adds entries with a UID namespace bit so live ANCS UIDs can't
  // collide (the add() date+title dedupe folds backfill re-ingest into them).
  std::size_t snapshot(uint8_t* out, std::size_t cap) const;
  void restore(const uint8_t* data, std::size_t len);

  // Individual-clear retry memory. ANCS has no durable identifier, so a
  // dated notification is matched by the same date+title identity used to
  // fold reconnect replays into restored entries. dateKey==0 is deliberately
  // not recorded: title alone is too collision-prone to suppress safely.
  bool recordTombstone(uint64_t dateKey, const char* title);
  bool isTombstoned(uint64_t dateKey, const char* title) const;
  std::size_t tombstoneSnapshot(uint8_t* out, std::size_t cap) const;
  void restoreTombstones(const uint8_t* data, std::size_t len);
  // M3 detail view: remove ONE entry by its newest-first index (the same
  // index space get() uses). Entries NEWER than it each shift one slot toward
  // the vacated position and _head steps back over the retired newest slot,
  // so the ring stays contiguous and every surviving index keeps its
  // newest-first order. Returns false when newestIndex >= count().
  bool removeAt(std::size_t newestIndex);

  // Bumped on every add()/clearAll(); the main loop polls this to mark the
  // Notifications scene dirty (e-ink discipline: no redraw from BLE paths).
  uint32_t revision() const { return _revision; }

 private:
  // Bubble the entry at newest-first index `newestIndex` into its sorted place
  // so the used block stays ascending by sortKey (get(0) = highest = newest).
  // Single element out of place -> at most one directional pass, O(used).
  void reorderFromNewest(std::size_t newestIndex);
  Entry& atNewest(std::size_t newestIndex) { return _entries[(_head + CAPACITY - 1 - newestIndex) % CAPACITY]; }

  Entry _entries[CAPACITY];
  std::size_t _head = 0;  // next write slot
  std::size_t _used = 0;
  Tombstone _tombstones[TOMBSTONE_CAPACITY];
  std::size_t _tombstoneHead = 0;  // next write slot (oldest when full)
  std::size_t _tombstoneUsed = 0;
  uint32_t _revision = 0;
};

extern NotificationStore NOTIFICATION_STORE;
