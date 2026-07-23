#pragma once

// xphone-os M2 — ANCS (Apple Notification Center Service) client, ported
// from x4-os src/companion/CompanionAncsClient.h (copied, not symlinked).
// The X4 stays a BLE peripheral; once the iPhone connects and the link is
// encrypted/bonded, this GATT *client* discovers Apple's ANCS service on the
// phone and subscribes to Notification Source + Data Source.
//
// Differences from the x4-os original:
//   * <Logging.h> -> BleShim.h (Serial printf).
//   * ANCS notify payloads are NOT parsed on the NimBLE host task. x4-os
//     handleGapEvent() parsed inline; here it only copies the raw bytes into
//     a fixed FreeRTOS queue and the main loop drains it via processQueue()
//     (docs/x4-core-learnings.md: nimble_host has a small protected stack;
//     attribute pulls on it overflowed during reconnect bursts).
//   * M2.1d: no duplicate notification array here. The fixed-buffer
//     NotificationStore is the single copy of notification text; this class
//     only reassembles the in-flight Data Source response in a fixed 512B
//     buffer and hands completed attributes straight to the store
//     (handleDataSource, main-loop context). No std::string / std::vector
//     anywhere on the notification path.

#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "../NotificationStore.h"  // APP_ID_CHARS (bundle-id field size)

#if defined(CONFIG_NIMBLE_ENABLED)
#include <host/ble_uuid.h>
struct ble_gap_conn_desc;
struct ble_gap_event;
#endif

class CompanionAncsClient final {
 public:
  void begin();
  void requestPairing();

  // Main-loop pump: drains raw ANCS packets queued by the NimBLE host task
  // and parses them here. Call once per loop() tick.
  void processQueue();

  // Open-triggers-sync (NotificationsScene::onEnter): force a fresh replay of
  // everything still in iOS Notification Center by re-writing the Notification
  // Source CCCD (off, then on in the write callback). iOS re-sends every
  // current notification flagged PreExisting and the normal backfill path
  // re-ingests them — idempotent, because the store upserts by UID and folds
  // restored duplicates by date+title. This is ANCS's only pull mechanism: the
  // protocol is push-only, so a dropped burst or timed-out fetch mid-session
  // has no other repair until the next reconnect. STICKY: when the link is
  // down or ANCS is not yet subscribed (opening the app mid-reconnect after a
  // wake is the common case), the request stays armed and pumpResync() fires
  // it once the link is ready. Throttled so rapid re-opens don't restart a
  // replay already in flight. Returns true when the CCCD rewrite started
  // immediately, false when it was only armed. Main-loop only.
  bool requestResync();

  // User-initiated individual clear. The entry metadata is deliberately
  // supplied by the store: this method only queues a negative ANCS action
  // when that UID belongs to the current connection and iOS advertised the
  // NegativeAction capability. Main-loop only; no-op when any gate fails.
  void dismissNotification(uint32_t uid, uint8_t categoryId, uint8_t flags, uint32_t sessionId);

  uint32_t getRevision() const;
  // Mutex-guarded copy of the short status line into a caller buffer
  // (always NUL-terminated; clipped to outSize).
  void getStatusMessage(char* out, std::size_t outSize) const;

  // Resolve a bundle id ("com.apple.MobileSMS") to the iOS display name
  // ("Messages") via the ANCS GetAppAttributes cache. Falls back to the last
  // dotted segment of the bundle ("net.whatsapp.WhatsApp" -> "WhatsApp") until
  // the name resolves. Main-loop only (reads the cache the ingest path fills).
  void appDisplayName(const char* bundleId, char* out, std::size_t outSize) const;

  // Snapshot enumeration for the companion's notification-app picker. The
  // cache is owned and mutated by the main-loop ANCS parser; sendNotifApps()
  // also runs on that loop, so copying here follows the cache's existing
  // single-task discipline rather than adding a mutex only for enumeration.
  std::size_t appCount() const;
  bool getApp(std::size_t index, char* bundle, std::size_t bundleSize, char* name,
              std::size_t nameSize, uint16_t* notifCount = nullptr) const;

  // Bumped when a GetAppAttributes lookup resolves a new display name, so the
  // main loop can repaint the Notifications list (a bundle id -> friendly name).
  uint32_t getAppNameRevision() const { return appNameRevision; }

  // Instrumentation: Notification Source queue high-water mark and drop count
  // (the NS queue is where a reconnect replay burst lands, so that is where any
  // drop occurs). Written only by the nimble_host producer, read-only elsewhere.
  uint8_t getQueueHighWater() const { return queueHighWater; }
  uint32_t getQueueDropCount() const { return queueDropCount; }
  TaskHandle_t getHostTaskHandle() const { return hostTaskHandle; }
  // Must be called when the NimBLE stack is torn down (File Transfer trades
  // BLE for Wi-Fi): the cached handle points at a deleted task and querying
  // its stack HWM is a load fault.
  void clearHostTaskHandle() { hostTaskHandle = nullptr; }

  // After the Reader scene suspends and resumes the radio, BLEDevice::init
  // runs again: re-register the custom GAP handler and re-arm pairing so
  // ANCS discovery restarts on the next phone connection.
  void rearmAfterRadioResume();

  // Sync-indicator queries (main-loop only, matching all backfill state).
  // remaining = queued UIDs still to fetch + any single in-flight fetch, so
  // the status-bar sync dot stays lit until the whole backfill drains.
  uint8_t getBackfillRemaining() const {
    return static_cast<uint8_t>(backfillCount + (fetchInFlight ? 1 : 0));
  }
  bool isBackfillFetchInFlight() const { return fetchInFlight; }

#if defined(CONFIG_NIMBLE_ENABLED)
  void handleServerConnect(uint16_t connHandle);
  void handleServerDisconnect(uint16_t connHandle);
  void handleAuthenticationComplete(ble_gap_conn_desc* desc);
  int handleGapEvent(ble_gap_event* event);
  void handleServiceDiscovered(uint16_t startHandle, uint16_t endHandle);
  void handleCharacteristicDiscovered(const ble_uuid_t* uuid, uint16_t valueHandle);
  void handleCharacteristicDiscoveryComplete();
  void handleDataSourceSubscribed();
  void handleNotificationSourceSubscribed();
  void handleNotificationSourceUnsubscribed();  // resync step 2: re-enable the CCCD
  void handleControlPointWriteComplete(int status);  // NimBLE host callback; mutex-guarded state only
#endif

 private:
  bool started = false;
  bool pairingRequested = false;
  bool discovering = false;
  bool ancsReady = false;
  uint16_t connHandle = 0xffff;
  uint16_t serviceStartHandle = 0;
  uint16_t serviceEndHandle = 0;
  uint16_t notificationSourceHandle = 0;
  uint16_t controlPointHandle = 0;
  uint16_t dataSourceHandle = 0;
  uint8_t lastEventFlags = 0;
  uint32_t lastNotificationUid = 0;
  // Incremented once per BLE connection. Unlike a UID bit test, this also
  // rejects entries retained in RAM from the immediately previous session.
  // Guarded by stateMutex; 0 is reserved for persisted/restored entries.
  uint32_t currentSessionId = 0;
  uint32_t revision = 0;
  // M2.1b (power lever 2): armed by handleNotificationSourceSubscribed()
  // (host task) once BOTH ANCS CCCDs are written; consumed on the main loop
  // by maybeRequestConnParams() (via processQueue) which issues
  // ble_gap_update_params for low-duty connection parameters.
  bool connParamUpdatePending = false;
  uint32_t connParamUpdateDueMs = 0;
  // Short status line for the About scene. Fixed buffer, guarded by
  // stateMutex (setStatus is called from host-task callbacks too).
  char statusMessage[48] = "ANCS idle";
  // Data Source reassembly buffer for one in-flight GetNotificationAttributes
  // response (fragments arrive per-notify at <= MTU). ONLY touched from
  // main-loop context (handleDataSource / removeNotification, both reached
  // via processQueue) — no locking needed.
  uint8_t dataSourceBuffer[512];
  uint16_t dataSourceLen = 0;
  uint32_t dataSourceBufferUid = 0;
  // Which command's response the buffer currently holds (GetNotificationAttributes
  // or GetAppAttributes) — set on the first fragment, dispatches the parse.
  uint8_t dataSourceCmd = 0;

  // ANCS GetAppAttributes display-name cache. Bundle ids are stable per app, so
  // each app's iOS display name ("Messages" for com.apple.MobileSMS) is fetched
  // once per session and cached. An entry (with a prettified fallback name) is
  // added the first time a notification from that app is ingested; the actual
  // GetAppAttributes fetch is serialized through the SAME single-in-flight gate
  // as the notification backfill because both responses land on the shared Data
  // Source buffer and must not overlap. Main-loop-only, like all backfill state.
  static constexpr std::size_t APP_NAME_CACHE_SIZE = 24;
  static constexpr std::size_t APP_NAME_CHARS = 24;  // iOS display names are short
  struct AppNameEntry {
    char bundle[NotificationStore::APP_ID_CHARS] = {0};
    char name[APP_NAME_CHARS] = {0};  // resolved name, or prettified fallback until then
    uint8_t state = 0;                // 0 = needs request, 1 = requested/in-flight, 2 = resolved
    uint16_t notifCount = 0;          // notifications seen this power cycle (picker sort hint)
  };
  AppNameEntry appNameCache[APP_NAME_CACHE_SIZE];
  uint8_t appNameCacheCount = 0;
  uint32_t appNameRevision = 0;

  // ANCS preexisting-notification backfill (§3 of
  // docs/xphone-notifications-architecture-research.md). On reconnect iOS
  // replays every notification still in Notification Center with
  // EventFlagPreExisting set; pulling attributes for all of them at once
  // floods the link / overflows the nimble_host stack (the reason these events
  // used to be skipped outright). Instead we enqueue the replayed UIDs into
  // this fixed ring and drain it ONE attribute fetch at a time via
  // pumpBackfill(). Live (non-preexisting) events jump to the FRONT so a
  // notification arriving mid-backfill is served first; preexisting events
  // queue at the back. When the ring is full the OLDEST queued UID is dropped
  // so the most-recent replayed notifications win (the store shows newest-first
  // and old ones scroll off anyway). All backfill state is main-loop-only
  // (touched only via processQueue -> handleNotificationSource /
  // handleDataSource / pumpBackfill) — no locking, matching the
  // dataSourceBuffer discipline above.
  static constexpr std::size_t BACKFILL_CAPACITY = 20;
  struct BackfillItem {
    uint32_t uid = 0;
    uint32_t sessionId = 0;
    uint8_t categoryId = 0;
    uint8_t flags = 0;
  };
  BackfillItem backfillQueue[BACKFILL_CAPACITY];
  uint8_t backfillHead = 0;         // front: next UID to fetch
  uint8_t backfillCount = 0;        // entries currently queued
  uint32_t backfillDropCount = 0;   // instrumentation: UIDs dropped (queue full)
  // Single-in-flight gate: at most one GetNotificationAttributes outstanding.
  // Set when the pump issues a fetch, cleared when its Data Source response
  // completes in handleDataSource OR the deadline passes (rollover-safe millis)
  // so a lost response can't wedge the queue.
  bool fetchInFlight = false;
  uint32_t fetchInFlightUid = 0;
  uint32_t fetchInFlightSessionId = 0;
  uint8_t fetchInFlightCategoryId = 0;
  uint8_t fetchInFlightFlags = 0;
  uint32_t fetchDeadlineMs = 0;
  // One-retry memory for timed-out notification fetches: the first timeout
  // re-enqueues the UID (a single lost Data Source response must not silently
  // drop the notification until the next reconnect); a second timeout for the
  // SAME UID gives up. Cleared on link-down flush.
  uint32_t fetchRetryUid = 0;
  // requestResync() throttle/in-flight stamp (0 = never ran). Main-loop only.
  uint32_t lastResyncMs = 0;
  // Discovery watchdog state (main-loop-only, via maybeKickDiscovery).
  uint32_t notReadySinceMs = 0;  // 0 = timer disarmed
  uint8_t discoveryKicks = 0;    // kicks this connection (capped)
  // When the in-flight fetch is a GetAppAttributes lookup (not a notification
  // attribute fetch) so the gate release / timeout can revert the right state.
  bool fetchInFlightIsApp = false;
  char fetchInFlightBundle[NotificationStore::APP_ID_CHARS] = {0};

  // Perform Notification Action has no Data Source response. Keep its writes
  // in a second fixed ring, but pump them through the same Control Point gate
  // as attribute requests: NimBLE permits only one GATT procedure at a time.
  // The ring is main-loop-only; disconnect merely sets dismissFlushPending
  // under the mutex and the next pump clears the now-stale UIDs.
  static constexpr std::size_t DISMISS_CAPACITY = 8;
  struct DismissItem {
    uint32_t uid = 0;
    uint32_t sessionId = 0;
  };
  DismissItem dismissQueue[DISMISS_CAPACITY];
  uint8_t dismissHead = 0;
  uint8_t dismissCount = 0;
  bool dismissFlushPending = false;  // stateMutex: host disconnect -> main-loop pump

  enum ControlPointWriteKind : uint8_t {
    ControlPointNone = 0,
    ControlPointNotificationAttributes,
    ControlPointAppAttributes,
    ControlPointDismiss,
  };
  bool controlPointWriteInFlight = false;  // stateMutex: main-loop start, host callback completion
  uint8_t controlPointWriteKind = ControlPointNone;

  // M2.1d instrumentation (see getters above). Single writer: nimble_host.
  volatile uint8_t queueHighWater = 0;
  volatile uint32_t queueDropCount = 0;
  TaskHandle_t hostTaskHandle = nullptr;

  mutable SemaphoreHandle_t stateMutex = nullptr;
  QueueHandle_t packetQueue = nullptr;  // Data Source fragments: host task -> main loop (static)
  QueueHandle_t nsQueue = nullptr;      // Notification Source events: deep burst queue (static)

  void ensureMutex() const;
  void ensureQueue();
  void setStatus(const char* message);
  bool isPairingRequested() const;

#if defined(CONFIG_NIMBLE_ENABLED)
  void maybeRequestConnParams();  // main loop only (via processQueue)
  void startDiscovery(uint16_t handle);
  void discoverCharacteristics();
  void subscribeNotificationSource();
  void subscribeDataSource();
  bool requestNotificationAttributes(uint32_t notificationUid);
  bool requestAppAttributes(const char* bundleId);  // main loop only (via processQueue)
  bool writeControlPoint(const uint8_t* data, std::size_t len, uint8_t kind);
  void handleAppAttributesResponse();                 // main loop only (via handleDataSource)
  // M5 discovery watchdog (main loop, via processQueue): discovery only ever
  // started from the encryption-change event, which is unreliable on wake
  // re-bonds (same edge problem the Block resync hit) — and a failed
  // ble_gattc_disc_svc_by_uuid left `discovering` latched with no retry. If
  // the phone is connected and ANCS is wanted but not ready for 4 s (12 s when
  // a discovery is nominally in flight), kick startDiscovery again; capped
  // per-connection. Gated on isConnected() (reliable), NOT isEncrypted().
  void maybeKickDiscovery();
  void pumpDismiss();   // main loop only (via processQueue; runs before backfill)
  void pumpBackfill();  // main loop only (via processQueue)
  void enqueueBackfill(uint32_t uid, uint8_t categoryId, uint8_t flags, uint32_t sessionId,
                       bool front);  // main loop only
  void handleNotificationSource(const uint8_t* data, std::size_t length, uint32_t sessionId);
  void handleDataSource(const uint8_t* data, std::size_t length, uint32_t sessionId);
  void removeNotification(uint32_t notificationUid);
#endif

  // App display-name cache helpers (main-loop only, no BLE — safe outside the
  // NimBLE guard). noteAppSeen() adds a fallback entry + marks it for lookup;
  // findAppEntry() is the linear cache search shared by the pump/response paths.
  void noteAppSeen(const char* bundleId);
  AppNameEntry* findAppEntry(const char* bundleId);
};

extern CompanionAncsClient COMPANION_ANCS;
