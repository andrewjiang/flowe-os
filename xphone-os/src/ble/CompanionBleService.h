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
class BLEService;
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
  // NOTE deliberately no getCard()/getBlockCard(): scenes render from the
  // bounded fixed stores (BLOCK_STATUS/TODAY_STORE/...), never by copying the
  // 3.6 KB parse slot. `card` below is the parse/assembly scratch only.

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
  enum class TransferRequest : uint8_t { None, Start, StartDirect, Stop };
  bool consumeShelfRequest();
  // Same latch, for reading progress + stats.
  bool consumeProgressRequest();
  bool consumeWifiKnownRequest();  // W1: app asked for the network report
  TransferRequest consumeTransferRequest();

  // Item 6: a reading place pushed from a phone over the (encrypted) link.
  // Latched here by applyCardPayload; the main loop writes the matching
  // book's .pos so the next open resumes there. The reader suspends BLE
  // while open, so a push only ever arrives off the reading screen — no
  // live-jump into an open book is needed.
  struct PlacePush {
    char key[64];       // canonical book key
    uint32_t page;
    uint32_t pageCount;
    uint64_t seq;
    uint64_t atMillis;
  };
  bool consumePlacePush(PlacePush& out);
  // Bench: inject a place push through the same latch the card handler
  // uses, so the write+resume path is provable without the app's sender.
  void benchInjectPlace(const char* key, uint32_t page, uint32_t pageCount);

  // Item 6 step 3, OUTBOUND. The reader suspends BLE while open, so the
  // moment the phone can hear where the device got to is book-close, when
  // the radio comes back. ReaderScene::onExit queues the last-read place
  // here; the pump sends it once the link is up and encrypted.
  void queueReaderPlace(const char* key, uint32_t page, uint32_t pageCount);
  // Main loop: if a place is queued and the link is encrypted, notify it.
  void pumpReaderPlace();
  // Set when the phone writes (proving it is subscribed); cleared on
  // disconnect. Gates the outbound place past the re-subscription race.
  void notePhoneGone();
  // GAP SUBSCRIBE events, routed from the gap hook: the truth about whether
  // the action channel has a live listener this connection.
  void noteActionSubscribe(uint16_t connHandle, uint16_t attrHandle, bool curNotify);
  void noteConnHandle(uint16_t connHandle);

  // Scan /books on the SD card and notify it as chunked "reader.shelf"
  // JSON messages sized to the live ATT MTU (a notify larger than MTU-3 is
  // silently truncated by NimBLE, which would corrupt the JSON). Main loop
  // only (SD access + notifies).
  void sendReaderShelf();
  // W1: saved network names + last-scan sightings + any pending join
  // failure (read-and-clear), as one chunked notify.
  void sendWifiKnown();
  // Reading progress + lifetime stats, chunked like the shelf. This is what
  // makes the app correct whenever you open it: without it, progress only
  // reaches the phone during a Wi-Fi transfer session started by hand.
  void sendReaderProgress();
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
  // Parse/assembly scratch: incoming cards land here (multi-part snapshots
  // accumulate across parts), the fixed stores capture what scenes render,
  // and releaseReaderTransients() frees its string heap at reader suspend.
  CompanionCardState card;
  std::string lastTodayCardJson;       // raw JSON of the last today.snapshot card
  std::string lastWorkoutCardJson;     // raw JSON of the last workout.snapshot card
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
  bool progressRequested = false;
  bool wifiKnownRequested = false;
  TransferRequest transferRequest = TransferRequest::None;
  bool placePushPending = false;
  PlacePush pendingPlace{};
  bool outPlacePending = false;
  PlacePush pendingOut{};
  uint32_t placeSeq = 0;  // monotonic within a boot; orders our own sends
  bool phoneReadyForNotify = false;
  // iOS NEVER re-writes the CCCD for a bonded peer — the spec says the
  // device must remember it, and ours forgets on every reboot. When no
  // subscribe arrived but the link is encrypted, notifyAction() sends
  // straight through the stack so iOS's assumption is true. (Found
  // 2026-08-23: three healthy connections, zero notifications received.)
  bool actionSubscribed = false;
  uint16_t encConnHandle = 0xFFFF;
  void notifyAction();

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
  // Kept ONLY so shutdownRadio can free them after BLEDevice::deinit:
  // upstream BLEServer has no destructor, so deinit deletes the server but
  // orphans the service/characteristic objects each begin() creates —
  // measured 1.76 KB leaked per reader BLE cycle (X4 bench, 2026-08-11).
  BLEService* gattService = nullptr;
  BLECharacteristic* cardCharacteristic = nullptr;
  BLEAdvertising* advertising = nullptr;
  BLECharacteristic* actionCharacteristic = nullptr;

  void ensureMutex() const;
  void applyAdvIntervals(AdvMode mode);  // writes m_advParams only; applied at next start()
  void setStatus(const std::string& message);
  bool sendBlockCommand(const char* type, uint16_t minutes, const char* presetId = nullptr);
  bool sendCommand(const char* type);
  bool applyCardPayload(const std::string& payload);
};

extern CompanionBleService COMPANION_BLE;
