#include "WifiCreds.h"

#include <Preferences.h>
#include <esp_mac.h>
#include <esp_random.h>

#include <cstdio>
#include <cstring>

namespace {
// Same NVS namespace as the rest of xphone-os (Sleep.cpp, ReaderScene.cpp).
constexpr const char* kNamespace = "xphone";
// Legacy single-network keys (pre-W1); migrated into slot storage on first
// touch and then removed.
constexpr const char* kLegacySsidKey = "wifiSsid";
constexpr const char* kLegacyPassKey = "wifiPass";

// Slot keys: wN<i>s / wN<i>p / wN<i>u (ssid, obfuscated pass, last-use seq).
constexpr const char* kSeqKey = "wSeq";
constexpr const char* kLastKey = "wLast";
constexpr const char* kSeenKey = "wSeen";
constexpr const char* kFailSsidKey = "wFailS";
constexpr const char* kFailReasonKey = "wFailR";

void slotKey(char* out, size_t n, int i, char kind) { snprintf(out, n, "wN%d%c", i, kind); }

// Cheap at-rest obfuscation (P4 courtesy, not cryptography — the threat
// model note in the header still applies).
//
// The XOR result is stored as HEX, not as raw bytes. A raw XOR produces a
// 0x00 byte whenever a password character equals its key character, and
// NVS putString() stops at the first NUL: the tail of the password was
// silently thrown away forever, and the device then failed the 4-way
// handshake with the exact reason=15 loop this release set out to kill.
// (Caught in the release audit, 2026-08-18. About 8% of passwords hit it,
// and the key letters are common ones.) Hex costs two flash bytes per
// character and cannot contain a NUL.
constexpr char kObfKey[] = "flowe-w1";

void obfuscateToHex(const char* plain, char* out, const size_t outSize) {
  size_t o = 0;
  for (size_t i = 0; plain[i] != '\0' && o + 3 <= outSize; i++) {
    const uint8_t b = (uint8_t)plain[i] ^ (uint8_t)kObfKey[i % (sizeof(kObfKey) - 1)];
    static const char* kHex = "0123456789abcdef";
    out[o++] = kHex[b >> 4];
    out[o++] = kHex[b & 0x0F];
  }
  out[o] = '\0';
}

int hexVal(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool looksLikeHex(const char* s) {
  size_t n = 0;
  for (; s[n] != '\0'; n++) {
    if (hexVal(s[n]) < 0) return false;
  }
  return n > 0 && (n % 2) == 0;
}

void deobfuscateFromHex(const char* hex, char* out, const size_t outSize) {
  // Legacy compat: slots written before the hex fix hold raw XOR bytes
  // (possibly already truncated by the NUL bug). Decode those with the
  // old transform so an intact password keeps working; the next add()
  // re-saves in hex. (Caught live: "pw len=0" on a pre-fix slot.)
  if (!looksLikeHex(hex)) {
    size_t o = 0;
    for (; hex[o] != '\0' && o + 1 < outSize; o++) {
      out[o] = (char)((uint8_t)hex[o] ^ (uint8_t)kObfKey[o % (sizeof(kObfKey) - 1)]);
    }
    out[o] = '\0';
    return;
  }
  size_t o = 0;
  for (size_t i = 0; hex[i] != '\0' && hex[i + 1] != '\0' && o + 1 < outSize; i += 2) {
    const int hi = hexVal(hex[i]), lo = hexVal(hex[i + 1]);
    if (hi < 0 || lo < 0) break;
    out[o] = (char)(((uint8_t)((hi << 4) | lo)) ^ (uint8_t)kObfKey[o % (sizeof(kObfKey) - 1)]);
    o++;
  }
  out[o] = '\0';
}

// One-time migration of the legacy pair into the slot store.
void migrateLegacy(Preferences& p) {
  if (!p.isKey(kLegacySsidKey)) return;
  char ssid[WifiCreds::kMaxSsid] = {0};
  char pass[WifiCreds::kMaxPassword] = {0};
  p.getString(kLegacySsidKey, ssid, sizeof(ssid));
  p.getString(kLegacyPassKey, pass, sizeof(pass));
  p.remove(kLegacySsidKey);
  p.remove(kLegacyPassKey);
  if (ssid[0] == '\0') return;
  char ks[8], kp[8], ku[8];
  slotKey(ks, sizeof(ks), 0, 's');
  slotKey(kp, sizeof(kp), 0, 'p');
  slotKey(ku, sizeof(ku), 0, 'u');
  char hex[2 * WifiCreds::kMaxPassword + 1];
  obfuscateToHex(pass, hex, sizeof(hex));
  p.putString(ks, ssid);
  p.putString(kp, hex);
  p.putUInt(ku, 1);
  p.putUInt(kSeqKey, 1);
  p.putString(kLastKey, ssid);
}

int findSlot(Preferences& p, const char* ssid) {
  char ks[8], buf[WifiCreds::kMaxSsid];
  for (int i = 0; i < WifiCreds::kMaxNetworks; i++) {
    slotKey(ks, sizeof(ks), i, 's');
    buf[0] = '\0';
    p.getString(ks, buf, sizeof(buf));
    if (buf[0] != '\0' && strcmp(buf, ssid) == 0) return i;
  }
  return -1;
}

int freeOrOldestSlot(Preferences& p) {
  char ks[8], ku[8], buf[WifiCreds::kMaxSsid];
  int oldest = 0;
  uint32_t oldestUse = 0xFFFFFFFF;
  for (int i = 0; i < WifiCreds::kMaxNetworks; i++) {
    slotKey(ks, sizeof(ks), i, 's');
    buf[0] = '\0';
    p.getString(ks, buf, sizeof(buf));
    if (buf[0] == '\0') return i;
    slotKey(ku, sizeof(ku), i, 'u');
    const uint32_t use = p.getUInt(ku, 0);
    if (use < oldestUse) {
      oldestUse = use;
      oldest = i;
    }
  }
  return oldest;
}
}  // namespace

namespace WifiCreds {

int count() {
  Preferences p;
  if (!p.begin(kNamespace, /*readOnly=*/false)) return 0;
  migrateLegacy(p);
  char ks[8], buf[kMaxSsid];
  int n = 0;
  for (int i = 0; i < kMaxNetworks; i++) {
    slotKey(ks, sizeof(ks), i, 's');
    buf[0] = '\0';
    p.getString(ks, buf, sizeof(buf));
    if (buf[0] != '\0') n++;
  }
  p.end();
  return n;
}

bool get(const int index, Network* out) {
  if (!out || index < 0) return false;
  Preferences p;
  if (!p.begin(kNamespace, /*readOnly=*/false)) return false;
  migrateLegacy(p);
  char ks[8], kp[8];
  int seen = 0;
  bool found = false;
  for (int i = 0; i < kMaxNetworks && !found; i++) {
    slotKey(ks, sizeof(ks), i, 's');
    out->ssid[0] = '\0';
    p.getString(ks, out->ssid, sizeof(out->ssid));
    if (out->ssid[0] == '\0') continue;
    if (seen++ != index) continue;
    slotKey(kp, sizeof(kp), i, 'p');
    char hex[2 * kMaxPassword + 1] = {0};
    p.getString(kp, hex, sizeof(hex));
    deobfuscateFromHex(hex, out->pass, sizeof(out->pass));
    found = true;
  }
  p.end();
  return found;
}

bool add(const char* ssid, const char* password) {
  if (!ssid || ssid[0] == '\0') return false;
  Preferences p;
  if (!p.begin(kNamespace, /*readOnly=*/false)) return false;
  migrateLegacy(p);
  int slot = findSlot(p, ssid);
  if (slot < 0) slot = freeOrOldestSlot(p);
  char ks[8], kp[8], ku[8];
  slotKey(ks, sizeof(ks), slot, 's');
  slotKey(kp, sizeof(kp), slot, 'p');
  slotKey(ku, sizeof(ku), slot, 'u');
  char pass[kMaxPassword] = {0};
  snprintf(pass, sizeof(pass), "%s", password ? password : "");
  char hex[2 * kMaxPassword + 1];
  obfuscateToHex(pass, hex, sizeof(hex));
  const uint32_t seq = p.getUInt(kSeqKey, 0) + 1;
  p.putUInt(kSeqKey, seq);
  p.putString(ks, ssid);
  p.putString(kp, hex);
  p.putUInt(ku, seq);
  p.end();
  return true;
}

bool removeSsid(const char* ssid) {
  if (!ssid) return false;
  Preferences p;
  if (!p.begin(kNamespace, /*readOnly=*/false)) return false;
  migrateLegacy(p);
  const int slot = findSlot(p, ssid);
  if (slot >= 0) {
    char k[8];
    slotKey(k, sizeof(k), slot, 's');
    p.remove(k);
    slotKey(k, sizeof(k), slot, 'p');
    p.remove(k);
    slotKey(k, sizeof(k), slot, 'u');
    p.remove(k);
  }
  p.end();
  return slot >= 0;
}

void replaceAll(const Network* nets, const int n) {
  Preferences p;
  if (!p.begin(kNamespace, /*readOnly=*/false)) return;
  // Drain the legacy pair FIRST: otherwise the add() loop below re-imports
  // it and resurrects a network this push meant to replace.
  migrateLegacy(p);
  char k[8];
  for (int i = 0; i < kMaxNetworks; i++) {
    slotKey(k, sizeof(k), i, 's');
    p.remove(k);
    slotKey(k, sizeof(k), i, 'p');
    p.remove(k);
    slotKey(k, sizeof(k), i, 'u');
    p.remove(k);
  }
  p.end();
  const int cap = n < kMaxNetworks ? n : kMaxNetworks;
  for (int i = 0; i < cap; i++) add(nets[i].ssid, nets[i].pass);
}

void markJoined(const char* ssid) {
  if (!ssid || ssid[0] == '\0') return;
  Preferences p;
  if (!p.begin(kNamespace, /*readOnly=*/false)) return;
  const int slot = findSlot(p, ssid);
  if (slot >= 0) {
    char ku[8];
    slotKey(ku, sizeof(ku), slot, 'u');
    const uint32_t seq = p.getUInt(kSeqKey, 0) + 1;
    p.putUInt(kSeqKey, seq);
    p.putUInt(ku, seq);
  }
  p.putString(kLastKey, ssid);
  p.end();
}

bool lastJoinedSsid(char* out, const size_t outSize) {
  if (!out || outSize == 0) return false;
  out[0] = '\0';
  Preferences p;
  if (!p.begin(kNamespace, /*readOnly=*/true)) return false;
  p.getString(kLastKey, out, outSize);
  p.end();
  return out[0] != '\0';
}

void saveSeen(const char* newlineJoinedSsids) {
  Preferences p;
  if (!p.begin(kNamespace, /*readOnly=*/false)) return;
  p.putString(kSeenKey, newlineJoinedSsids ? newlineJoinedSsids : "");
  p.end();
}

size_t loadSeen(char* out, const size_t outSize) {
  if (!out || outSize == 0) return 0;
  out[0] = '\0';
  Preferences p;
  if (!p.begin(kNamespace, /*readOnly=*/true)) return 0;
  p.getString(kSeenKey, out, outSize);
  p.end();
  return strlen(out);
}

void saveFailure(const char* ssid, const int reason80211) {
  Preferences p;
  if (!p.begin(kNamespace, /*readOnly=*/false)) return;
  p.putString(kFailSsidKey, ssid ? ssid : "");
  p.putInt(kFailReasonKey, reason80211);
  p.end();
}

bool takeFailure(char* ssid, const size_t ssidSize, int* reason) {
  Preferences p;
  if (!p.begin(kNamespace, /*readOnly=*/false)) return false;
  if (!p.isKey(kFailSsidKey)) {
    p.end();
    return false;
  }
  if (ssid && ssidSize) {
    ssid[0] = '\0';
    p.getString(kFailSsidKey, ssid, ssidSize);
  }
  if (reason) *reason = p.getInt(kFailReasonKey, 0);
  p.remove(kFailSsidKey);
  p.remove(kFailReasonKey);
  p.end();
  return true;
}

void apCredentials(char* ssid, const size_t ssidSize, char* pass, const size_t passSize) {
  if (ssid && ssidSize) ssid[0] = '\0';
  if (pass && passSize) pass[0] = '\0';
  Preferences p;
  if (!p.begin(kNamespace, /*readOnly=*/false)) return;
  char s[33] = {0}, pw[17] = {0};
  p.getString("apSsid", s, sizeof(s));
  p.getString("apPass", pw, sizeof(pw));
  if (s[0] == '\0' || pw[0] == '\0') {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s, sizeof(s), "Flowe-XT-%02X%02X", mac[4], mac[5]);
    // No 0/O/1/l lookalikes: the QR-less fallback is a human reading glass.
    static const char kCs[] = "abcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < 12; i++) pw[i] = kCs[esp_random() % (sizeof(kCs) - 1)];
    pw[12] = '\0';
    p.putString("apSsid", s);
    p.putString("apPass", pw);
  }
  p.end();
  if (ssid && ssidSize) snprintf(ssid, ssidSize, "%s", s);
  if (pass && passSize) snprintf(pass, passSize, "%s", pw);
}

// ---- legacy shims ----

bool load(char* ssid, const size_t ssidSize, char* password, const size_t passwordSize) {
  if (ssid && ssidSize) ssid[0] = '\0';
  if (password && passwordSize) password[0] = '\0';
  // Last-joined first; any stored network otherwise.
  char last[kMaxSsid] = {0};
  Network net;
  bool found = false;
  if (lastJoinedSsid(last, sizeof(last))) {
    for (int i = 0; get(i, &net); i++) {
      if (strcmp(net.ssid, last) == 0) {
        found = true;
        break;
      }
    }
  }
  if (!found) found = get(0, &net);
  if (!found) return false;
  if (ssid && ssidSize) snprintf(ssid, ssidSize, "%s", net.ssid);
  if (password && passwordSize) snprintf(password, passwordSize, "%s", net.pass);
  return true;
}

bool save(const char* ssid, const char* password) {
  if (!ssid || ssid[0] == '\0') {
    clear();
    return true;
  }
  const bool ok = add(ssid, password);
  if (ok) markJoined(ssid);
  return ok;
}

bool hasCreds() { return count() > 0; }

void clear() {
  Network none;
  (void)none;
  replaceAll(nullptr, 0);
  Preferences p;
  if (p.begin(kNamespace, /*readOnly=*/false)) {
    p.remove(kLastKey);
    p.end();
  }
}

}  // namespace WifiCreds
