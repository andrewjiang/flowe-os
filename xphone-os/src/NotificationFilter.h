#pragma once

// Device-side notification blocklist.
//
// ANCS always exposes the iPhone's full Notification Center stream; iOS cannot
// ask it to suppress individual apps. Every app therefore shows by default,
// and the companion app sends bundle ids the user wants HIDDEN. This
// fixed-buffer module makes the decision at the one ingest boundary before
// NotificationStore::add(). Everything here is main-loop-only, matching the
// store and ANCS attribute parser, so no mutex is needed and notification
// delivery never allocates memory.

#include <cstddef>
#include <cstdint>

#include "NotificationStore.h"

class NotificationFilter final {
 public:
  static constexpr std::size_t CAPACITY = 16;

  // Lazy NVS load keeps the module self-contained and makes every public read
  // safe even if boot ordering changes later. Safe to call repeatedly.
  void ensureLoaded();

  // Replace the complete blocklist and persist it immediately. Bundle ids are
  // clipped to the same 31-byte payload used by NotificationStore, so a long
  // id compares consistently at configuration time and ANCS ingest time.
  // Main-loop only; null/empty entries and duplicates are ignored.
  void set(const char* const* bundleIds, std::size_t count);

  // Pass-through unless the id exactly matches a blocked entry (after the
  // same clipping). An empty list — the default — allows everything.
  bool allows(const char* bundleId);

  std::size_t count() {
    ensureLoaded();
    return _count;
  }
  uint32_t revision() const { return _revision; }

 private:
  void persist() const;

  bool _loaded = false;
  std::size_t _count = 0;
  char _bundles[CAPACITY][NotificationStore::APP_ID_CHARS] = {{0}};
  uint32_t _revision = 0;
};

extern NotificationFilter NOTIFICATION_FILTER;
