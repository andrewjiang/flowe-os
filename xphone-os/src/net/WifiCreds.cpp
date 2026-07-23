#include "WifiCreds.h"

#include <Preferences.h>

#include <cstring>

namespace {
// Same NVS namespace as the rest of xphone-os (Sleep.cpp, ReaderScene.cpp).
constexpr const char* kNamespace = "xphone";
constexpr const char* kSsidKey = "wifiSsid";
constexpr const char* kPassKey = "wifiPass";
}  // namespace

namespace WifiCreds {

bool load(char* ssid, const size_t ssidSize, char* password, const size_t passwordSize) {
  if (ssid && ssidSize) ssid[0] = '\0';
  if (password && passwordSize) password[0] = '\0';

  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/true)) return false;
  if (ssid && ssidSize) prefs.getString(kSsidKey, ssid, ssidSize);
  if (password && passwordSize) prefs.getString(kPassKey, password, passwordSize);
  prefs.end();
  return ssid && ssid[0] != '\0';
}

bool save(const char* ssid, const char* password) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) return false;
  if (!ssid || ssid[0] == '\0') {
    prefs.remove(kSsidKey);
    prefs.remove(kPassKey);
  } else {
    prefs.putString(kSsidKey, ssid);
    prefs.putString(kPassKey, password ? password : "");
  }
  prefs.end();
  return true;
}

bool hasCreds() {
  char ssid[kMaxSsid];
  return load(ssid, sizeof(ssid), nullptr, 0);
}

void clear() { save("", nullptr); }

}  // namespace WifiCreds
