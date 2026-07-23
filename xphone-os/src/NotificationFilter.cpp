#include "NotificationFilter.h"

#include <Preferences.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr const char* kPrefsNamespace = "xphone";
// ESP-IDF NVS keys are limited to 15 characters including the terminator.
// "notifBlkList" is deliberately distinct from the retired allowlist key
// ("notifFltList") so a device upgraded across the semantics flip starts from
// the safe default (empty blocklist = show everything) instead of hiding the
// exact apps the user had previously chosen to KEEP.
constexpr const char* kListKey = "notifBlkList";
constexpr uint8_t kListSchemaVersion = 1;

struct PersistedList {
  uint8_t schemaVersion = kListSchemaVersion;
  uint8_t count = 0;
  char bundles[NotificationFilter::CAPACITY][NotificationStore::APP_ID_CHARS] = {{0}};
};

void copyClippedBundle(char* destination, const char* source) {
  if (!destination) return;
  destination[0] = '\0';
  if (!source) return;
  // Bundle identifiers are ASCII, so the device's byte clipping is also a
  // character boundary. Explicit termination makes comparisons safe even for
  // an overlong or malformed phone payload.
  std::strncpy(destination, source, NotificationStore::APP_ID_CHARS - 1);
  destination[NotificationStore::APP_ID_CHARS - 1] = '\0';
}

bool containsBundle(const char bundles[][NotificationStore::APP_ID_CHARS], const std::size_t count,
                    const char* candidate) {
  for (std::size_t i = 0; i < count; ++i) {
    if (std::strncmp(bundles[i], candidate, NotificationStore::APP_ID_CHARS) == 0) return true;
  }
  return false;
}

}  // namespace

NotificationFilter NOTIFICATION_FILTER;

void NotificationFilter::ensureLoaded() {
  if (_loaded) return;

  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, /*readOnly=*/true)) {
    PersistedList saved;
    if (prefs.getBytesLength(kListKey) == sizeof(saved) &&
        prefs.getBytes(kListKey, &saved, sizeof(saved)) == sizeof(saved) &&
        saved.schemaVersion == kListSchemaVersion) {
      const std::size_t savedCount = std::min<std::size_t>(saved.count, CAPACITY);
      for (std::size_t i = 0; i < savedCount; ++i) {
        char clipped[NotificationStore::APP_ID_CHARS] = {0};
        copyClippedBundle(clipped, saved.bundles[i]);
        if (!clipped[0] || containsBundle(_bundles, _count, clipped)) continue;
        copyClippedBundle(_bundles[_count++], clipped);
      }
    }
    prefs.end();
  }

  _loaded = true;
}

void NotificationFilter::set(const char* const* bundleIds, const std::size_t count) {
  ensureLoaded();

  // Build the replacement off to the side. This makes the public update
  // atomic from the main loop's point of view and avoids partially replacing
  // the live filter if the input contains blanks or duplicates.
  char next[CAPACITY][NotificationStore::APP_ID_CHARS] = {{0}};
  std::size_t nextCount = 0;
  const std::size_t inputCount = std::min<std::size_t>(count, CAPACITY);
  for (std::size_t i = 0; bundleIds && i < inputCount; ++i) {
    char clipped[NotificationStore::APP_ID_CHARS] = {0};
    copyClippedBundle(clipped, bundleIds[i]);
    if (!clipped[0] || containsBundle(next, nextCount, clipped)) continue;
    copyClippedBundle(next[nextCount++], clipped);
  }

  bool changed = _count != nextCount;
  if (!changed) {
    for (std::size_t i = 0; i < nextCount; ++i) {
      if (std::strncmp(_bundles[i], next[i], NotificationStore::APP_ID_CHARS) != 0) {
        changed = true;
        break;
      }
    }
  }

  _count = nextCount;
  std::memset(_bundles, 0, sizeof(_bundles));
  if (nextCount > 0) std::memcpy(_bundles, next, nextCount * NotificationStore::APP_ID_CHARS);
  if (changed) ++_revision;

  // Filter changes are rare and losing one is surprising, so pay the flash
  // write immediately instead of deferring it to sleep (which may never run
  // after a crash or hard power-off).
  persist();
}

bool NotificationFilter::allows(const char* bundleId) {
  ensureLoaded();
  if (_count == 0) return true;

  char clipped[NotificationStore::APP_ID_CHARS] = {0};
  copyClippedBundle(clipped, bundleId);
  if (!clipped[0]) return true;  // unidentifiable apps default to visible
  return !containsBundle(_bundles, _count, clipped);
}

void NotificationFilter::persist() const {
  PersistedList saved;
  saved.count = static_cast<uint8_t>(_count);
  if (_count > 0) std::memcpy(saved.bundles, _bundles, _count * NotificationStore::APP_ID_CHARS);

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, /*readOnly=*/false)) return;
  prefs.putBytes(kListKey, &saved, sizeof(saved));
  prefs.end();
}
