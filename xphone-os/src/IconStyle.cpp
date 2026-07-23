#include "IconStyle.h"

#include <Preferences.h>

#include "art/LauncherIcons.h"

namespace {

constexpr const char* kPrefsNs = "xphone";
constexpr const char* kPrefsKey = "iconPack";

uint8_t gPack = 0;
bool gLoaded = false;

uint8_t clampPack(uint8_t i) {
  if (XPhoneIconPackCount <= 0) return 0;
  if (i >= static_cast<uint8_t>(XPhoneIconPackCount)) return 0;
  return i;
}

void ensureLoaded() {
  if (gLoaded) return;
  Preferences prefs;
  if (prefs.begin(kPrefsNs, /*readOnly=*/true)) {
    gPack = clampPack(static_cast<uint8_t>(prefs.getUChar(kPrefsKey, 0)));
    prefs.end();
  }
  gLoaded = true;
}

}  // namespace

namespace IconStyle {

uint8_t get() {
  ensureLoaded();
  return gPack;
}

void set(uint8_t packIndex) {
  ensureLoaded();
  const uint8_t next = clampPack(packIndex);
  if (next == gPack && gLoaded) {
    // Still persist in case NVS was empty on first boot.
  }
  gPack = next;
  Preferences prefs;
  if (prefs.begin(kPrefsNs, /*readOnly=*/false)) {
    prefs.putUChar(kPrefsKey, gPack);
    prefs.end();
  }
}

const uint8_t* iconForApp(int appIndex) {
  ensureLoaded();
  if (appIndex < 0 || appIndex >= XPhoneIconAppCount) return nullptr;
  return XPhoneIconPacks[gPack][appIndex];
}

const char* activeName() {
  ensureLoaded();
  return XPhoneIconPackNames[gPack];
}

const char* packName(uint8_t i) {
  if (i >= static_cast<uint8_t>(XPhoneIconPackCount)) return nullptr;
  return XPhoneIconPackNames[i];
}

uint8_t packCount() { return static_cast<uint8_t>(XPhoneIconPackCount); }

}  // namespace IconStyle
