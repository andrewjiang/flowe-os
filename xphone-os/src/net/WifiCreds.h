#pragma once

// xphone-os W1 — Wi-Fi networks for File Transfer mode, stored in NVS.
//
// Up to eight networks (CrossPoint's number, proven on this chip),
// provisioned from the phone over the encrypted BLE link — the device has
// no keyboard, so there is no on-device picker or password entry. NVS is
// on-chip flash (not the removable SD card). Passwords are XOR-obfuscated
// at rest as a cheap courtesy (docs/plans/2026-08-18-wifi-experience.md
// P4); the radio link they cross is BLE-encrypted (bonded).
//
// The store also keeps three one-liners of session truth for the app:
// the last scan's SSID list (what the device SAW), the last-joined
// network, and the last join failure {ssid, 802.11 reason} — written
// before the failure restart, reported over BLE after it.

#include <cstddef>

namespace WifiCreds {
constexpr size_t kMaxSsid = 64;
constexpr size_t kMaxPassword = 64;
constexpr int kMaxNetworks = 8;

struct Network {
  char ssid[kMaxSsid];
  char pass[kMaxPassword];
};

// ---- multi-network store ----
int count();
bool get(int index, Network* out);                 // 0..count()-1, stable slot order
bool add(const char* ssid, const char* password);  // upsert; evicts the least-recently-joined when full
bool removeSsid(const char* ssid);
void replaceAll(const Network* nets, int n);       // the phone's vault push
void markJoined(const char* ssid);                 // recency for join order + eviction
bool lastJoinedSsid(char* out, size_t outSize);

// ---- session truth for the app ----
void saveSeen(const char* newlineJoinedSsids);     // last scan result (names only)
size_t loadSeen(char* out, size_t outSize);
void saveFailure(const char* ssid, int reason80211);
bool takeFailure(char* ssid, size_t ssidSize, int* reason);  // read + clear

// ---- W2 Direct mode: the device's own hotspot identity ----
// Minted once per device (stable SSID suffix from the AP MAC, random
// password), persisted in NVS, exchanged only over encrypted BLE
// (wifi.known "ap"). Never typed, never shown by default (Q3).
void apCredentials(char* ssid, size_t ssidSize, char* pass, size_t passSize);

// ---- legacy single-network API (shims over the store) ----
bool load(char* ssid, size_t ssidSize, char* password, size_t passwordSize);
bool save(const char* ssid, const char* password);
bool hasCreds();
void clear();
}  // namespace WifiCreds
