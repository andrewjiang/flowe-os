#pragma once

// xphone-os R2 — Wi-Fi credentials for File Transfer mode, stored in NVS.
//
// One network only (the user's home Wi-Fi), provisioned from the iOS app
// over the encrypted BLE link ("transfer.wifi" card) — the device has no
// keyboard, so unlike CrossPoint there is no on-device network picker or
// password entry to port. NVS is on-chip flash (not the removable SD card),
// so plaintext-at-rest matches the threat model of every other ESP32 Wi-Fi
// project; the radio link the password crosses is BLE-encrypted (bonded).

#include <cstddef>

namespace WifiCreds {
constexpr size_t kMaxSsid = 64;
constexpr size_t kMaxPassword = 64;

// Copy stored credentials into the buffers (empty strings when unset).
// Returns true when an SSID is present.
bool load(char* ssid, size_t ssidSize, char* password, size_t passwordSize);

// Persist (overwrites the previous network). Empty ssid clears the store.
bool save(const char* ssid, const char* password);

bool hasCreds();
void clear();
}  // namespace WifiCreds
