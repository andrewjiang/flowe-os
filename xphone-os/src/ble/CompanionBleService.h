#pragma once

// xphone-os M2 — companion BLE peripheral, ported from x4-os
// src/companion/CompanionBleService.h (copied, not symlinked; x4-os is
// read-only reference). Differences from the x4-os original:
//   * <Logging.h> replaced by the local BleShim.h Serial-printf macros.
//   * Camera image transfer (CompanionCameraImageState + buffer fields,
//     sendCameraPreview/sendCameraRequest) stripped — camera.image.* card
//     writes are dropped with a status message, everything else in the
//     card/command JSON protocol and all UUIDs are unchanged so the existing
//     iOS companion app connects as before.
//   * Card payload parsing moved OFF the BLE host task: handleCardWrite()
//     (NimBLE host callback) only copies the raw bytes into a small FIFO;
//     processPending() — called from the Arduino main loop — runs the JSON
//     parse (x4-os learning: a Block card parsed on the nimble_host stack
//     caused a Stack protection fault; docs/x4-core-learnings.md "BLE
//     callback stability learning").
//   * Peer address capture (getPeerAddress) for the About scene, and
//     connect/disconnect/auth-complete forwarding into CompanionAncsClient.

#include <cstddef>
#include <cstdint>
#include <string>

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "CompanionProtocol.h"

class BLEAdvertising;
class BLECharacteristic;
class BLECharacteristicCallbacks;
class BLEServer;
class BLEServerCallbacks;
struct ble_gap_conn_desc;

class CompanionBleService final {
 public:
  void begin();
  void startAdvertising();
  void stopAdvertising();
  // R2 File Transfer — tear the whole NimBLE stack down and release its
  // memory. The X3 idles at ~39 KB free heap with BLE+ANCS up and Wi-Fi
  // bring-up needs ~50 KB, so they cannot coexist here (measured: WiFi.mode
  // aborted the firmware). One-way until reboot — the transfer session
  // already ends with esp_restart(), which brings BLE back.
  void shutdownForTransfer();

  // Reader <-> radio time-sharing (CrossPoint's model). The X3 has no PSRAM;
  // with BLE+ANCS resident the reader can't get its 32 KB inflate window +
  // parser buffers, so first-time chapter indexing fails. The Reader scene
  // suspends BLE on entry (deinit keeps memory so no reboot is needed) and
  // resumes it on exit.
  void suspendForReader();
  void resumeAfterReader();
  // Free every heap block this service still owns from the connected era
  // (card JSON mirrors, pending inbound slices, status string). Runs as the
  // tail of suspendForReader(): tiny survivors otherwise sit mid-heap and
  // split the contiguous 32 KB the reader needs (measured on X3). All of it
  // is recoverable — parsed stores are fixed buffers, NVS keeps the last
  // real card, the phone re-pushes on reconnect.
  void releaseReaderTransients();

  bool isStarted() const { return started; }
  bool isConnected() const;
  uint32_t getRevision() const;
  std::string getStatusMessage() const;
  CompanionCardState getCard() const;
  // M4.2 Block scene — dedicated cache of the LAST parsed "block-status" card,
  // kept intact regardless of later priorities/today cards clobbering the
  // single generic `card` slot (priorities/today snapshot cards land in the same
  // slot, so getCard() no longer reliably holds the block card by the time
  // BlockScene renders). Mutex-guarded copy, same cost/pattern as getCard(); the counter
  // bumps only when a block-status card lands, so BlockScene can drive its
  // fresh-card detection off block traffic alone.
  CompanionCardState getBlockCard() const;
  uint32_t getBlockCardRevision() const;

  // Last Today / Priorities card JSON (raw payload), stashed so Sleep can persist
  // them to NVS and re-seed the stores on wake — skipping the blank "Syncing"
  // screen. Main-loop only (set in applyCardPayload). "" until one arrives.
  // (Priorities persists from PRIORITIES_STORE instead — a multi-part
  // snapshot's last raw payload is only the tail slice.)
  const std::string& getLastTodayCard() const { return lastTodayCardJson; }
  const std::string& getLastWorkoutCard() const { return lastWorkoutCardJson; }
  // Re-apply a persisted card JSON at boot to seed a store before BLE is up.
  void seedPersistedCard(const std::string& json) {
    if (!json.empty()) applyCardPayload(json);
  }
  // Copies the last connected peer's OTA address ("aa:bb:cc:dd:ee:ff") into
  // buf; returns false (buf = "") when no peer has connected since boot.
  bool getPeerAddress(char* buf, std::size_t bufSize) const;

  // M2.1b (power lever 1) — advertising duty policy. Fast (30-60 ms, the
  // NimBLE connectable default band) for kAdvFastWindowMs after boot or a
  // disconnect for quick-discovery UX, then slow (400-500 ms) indefinitely.
  enum class AdvMode : uint8_t { Fast, Slow };
  AdvMode getAdvMode() const;
  // Main-loop tick: demotes fast -> slow advertising once the window expires
  // (restart of advertising must not run on the NimBLE host task).
  void tickAdvPolicy();

  // M2.1b (power lever 2 readout) — live parameters of the current
  // connection from the NimBLE descriptor (ble_gap_conn_find). Units are raw
  // BLE: itvl125 x 1.25 ms, timeout10ms x 10 ms. False when not connected.
  bool getConnParams(uint16_t& itvl125, uint16_t& latency, uint16_t& timeout10ms) const;

  void updateStatus(const std::string& message);
  void publishCard(const CompanionCardState& next, const std::string& message);
  bool sendBlockStart(uint16_t minutes = 0, const char* presetId = nullptr);
  bool sendBlockBreak(uint16_t minutes = 5);
  bool sendBlockStop();
  bool sendBlockStatus();
  // M3 Priorities — same command JSON the x4-os service sends (x4-os
  // CompanionBleService.cpp:915-918/984-1014); iOS answers both with a fresh
  // "priorities.snapshot" card (PrioritiesManager.swift handleActionPayload).
  bool sendPrioritiesSyncRequest();
  bool sendPriorityToggle(const char* itemId, bool done);
  bool sendWorkoutSyncRequest();
  // Absolute completed-set count for one exercise (idempotent on the phone).
  bool sendWorkoutSet(const char* itemId, int done);
  // M3 Today — "today.sync.request" command, same generic command JSON shape
  // as the priorities sync request. NOTE: x4-os TodayActivity never sends a
  // sync (its Sync button is a placeholder no-op) and the current iOS app has
  // no handler for this type — unknown types fall through its dispatch
  // harmlessly (BluetoothManager.swift didUpdateValueFor), so this is the
  // forward-compatible hook for when the iOS producer lands.
  bool sendTodaySyncRequest();
  bool sendAction(std::size_t actionIndex);
  void markConnected(bool value);
  void setPeerAddress(const uint8_t* addrLe);  // 6 bytes, little-endian (NimBLE order)

  // Pairing/encryption pump. iOS shows its pairing popup only once the
  // peripheral initiates security — the framework's automatic
  // startSecurity-on-connect path does not reliably do so on hardware, so
  // the connect callback arms this and processPending() (main loop) calls
  // ble_gap_security_initiate with logging and one retry.
  void armSecurity(uint16_t connHandle);  // NimBLE host task: state flips only
  void disarmSecurity();                  // NimBLE host task: state flips only
  // Called from the security callback's onAuthenticationComplete (host task)
  // before ANCS sees the event; state flips + status only, no BLE calls.
  void handleEncryptionChange(ble_gap_conn_desc* desc);
  bool isEncrypted() const;

  // BLE host-task side: stash the raw write, never parse here.
  void handleCardWrite(BLECharacteristic* characteristic);
  // Main-loop side: parse at most one stashed payload per call.
  void processPending();

  // R2 Read — phone-initiated requests, latched by applyCardPayload (main
  // loop) and consumed by main.cpp's pump. No mutex needed: both sides run
  // on the main loop.
  enum class TransferRequest : uint8_t { None, Start, Stop };
  bool consumeShelfRequest();
  TransferRequest consumeTransferRequest();

  // Scan /books on the SD card and notify it as chunked "reader.shelf"
  // JSON messages sized to the live ATT MTU (a notify larger than MTU-3 is
  // silently truncated by NimBLE, which would corrupt the JSON). Main loop
  // only (SD access + notifies).
  void sendReaderShelf();
  // Notify the phone with the ANCS app-name cache as chunked "notif.apps"
  // messages. Main-loop-only for the same reason as the cache enumerator.
  void sendNotifApps();
  // {"type":"transfer.status","state":...,"ip":...,"detail":...} notify —
  // how the File Transfer scene reports Wi-Fi progress back to the phone.
  void sendTransferStatus(const char* state, const char* ip = nullptr, const char* detail = nullptr);

 private:
  void shutdownRadio(bool releaseMemory, const char* reason);

  bool started = false;
  bool advertisingWanted = false;
  bool connected = false;
  uint32_t revision = 0;
  uint32_t actionSequence = 0;
  CompanionCardState card;
  // M4.2 — full copy of the last "block-status" card + its own revision, so
  // BlockScene reads durable block state instead of the clobbered `card` slot.
  CompanionCardState blockCard;
  std::string lastTodayCardJson;       // raw JSON of the last today.snapshot card
  std::string lastWorkoutCardJson;     // raw JSON of the last workout.snapshot card
  uint32_t blockCardRevision = 0;
  std::string statusMessage = "Not advertising";
  char peerAddress[18] = {0};

  // Raw card JSON queued by handleCardWrite(). A small FIFO, not a single
  // slot: multi-part priorities snapshots arrive one GATT write per
  // connection interval, and a single slot dropped earlier parts whenever
  // the main loop was busy composing a frame between drains.
  static constexpr std::size_t PENDING_CAPACITY = 4;
  std::string pendingPayloads[PENDING_CAPACITY];
  std::size_t pendingHead = 0;   // next slot to drain
  std::size_t pendingCount = 0;  // filled slots

  // R2 Read — main-loop-only latches (see consumeShelfRequest above).
  bool shelfRequested = false;
  TransferRequest transferRequest = TransferRequest::None;

  // Security pump state — written from the NimBLE host task (arm/disarm/
  // encryption-change) and the main loop (processPending), so every touch
  // holds stateMutex.
  uint16_t secConnHandle = 0xffff;
  bool encrypted = false;
  bool securityPending = false;
  uint8_t securityAttempts = 0;
  uint32_t securityDueAtMs = 0;

  // M2.1b adv policy state — advMode/advFastUntilMs are written from the
  // main loop (begin/tickAdvPolicy) and the host task (markConnected on
  // disconnect), so every touch holds stateMutex.
  AdvMode advMode = AdvMode::Fast;
  uint32_t advFastUntilMs = 0;

  mutable SemaphoreHandle_t stateMutex = nullptr;

  BLEServer* server = nullptr;
  BLEAdvertising* advertising = nullptr;
  BLECharacteristic* actionCharacteristic = nullptr;
  BLEServerCallbacks* serverCallbacks = nullptr;
  BLECharacteristicCallbacks* cardWriteCallbacks = nullptr;

  void ensureMutex() const;
  void applyAdvIntervals(AdvMode mode);  // writes m_advParams only; applied at next start()
  void setStatus(const std::string& message);
  bool sendBlockCommand(const char* type, uint16_t minutes, const char* presetId = nullptr);
  bool sendCommand(const char* type);
  bool applyCardPayload(const std::string& payload);
};

extern CompanionBleService COMPANION_BLE;
