// xphone-os M2 — ported from x4-os src/companion/CompanionBleService.cpp.
// See CompanionBleService.h for the delta list (logging shim, camera strip,
// off-host-task card parsing, ANCS wiring, peer address capture).

#include "CompanionBleService.h"

#include <ArduinoJson.h>
#if !defined(CONFIG_NIMBLE_ENABLED)
#include <BLE2902.h>
#endif
#include <BLEAdvertising.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include <SDCardManager.h>

#include "../BlockStatusStore.h"
#include "../NotificationFilter.h"
#include "../PrioritiesStore.h"
#include "../WorkoutStore.h"
#include "../TodayStore.h"
#include "../net/WifiCreds.h"
#include "BleShim.h"
#include "CompanionAncsClient.h"

#if defined(CONFIG_NIMBLE_ENABLED)
#include <host/ble_att.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#endif

namespace {
// M2.1b advertising policy intervals, units of 0.625 ms (BLEAdvertising::
// setMin/MaxInterval write ble_gap_adv_params.itvl_min/max directly).
// Fast = 30-60 ms, the band NimBLE would default to anyway when itvl is 0
// (BLE_GAP_ADV_FAST_INTERVAL1) — kept explicit so the demotion is symmetric.
// Slow = 400-500 ms: ~8x fewer adv events; bonded iOS reconnects tolerate
// slow advertising well (discovery latency grows by at most one interval).
constexpr uint16_t kAdvFastMinItvl = 48;   // 30 ms
constexpr uint16_t kAdvFastMaxItvl = 96;   // 60 ms
constexpr uint16_t kAdvSlowMinItvl = 640;  // 400 ms
constexpr uint16_t kAdvSlowMaxItvl = 800;  // 500 ms
// Fast-advertising grace window after boot / disconnect.
constexpr uint32_t kAdvFastWindowMs = 60000;

constexpr uint8_t BLE_AD_TYPE_FLAGS = 0x01;
constexpr uint8_t BLE_AD_TYPE_SOL_SRV_UUID128 = 0x15;  // Service Solicitation, 128-bit

void addFlags(BLEAdvertisementData& data) {
  const char payload[] = {2, BLE_AD_TYPE_FLAGS,
                          static_cast<char>(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT)};
  data.addData(const_cast<char*>(payload), sizeof(payload));
}

// ANCS service solicitation (7905F431-B5CE-4E99-A40F-4B1E122D00D0, little-
// endian on the wire). Tells iOS this peripheral wants the phone's ANCS
// service — iOS uses it to surface the accessory as notification-capable and
// to auto-reconnect bonded accessories in the background. 18 bytes with
// header, so the device name lives in the scan response instead.
void addAncsSolicitation(BLEAdvertisementData& data) {
  const char payload[] = {17,
                          static_cast<char>(BLE_AD_TYPE_SOL_SRV_UUID128),
                          '\xd0', '\x00', '\x2d', '\x12', '\x1e', '\x4b', '\x0f', '\xa4',
                          '\x99', '\x4e', '\xce', '\xb5', '\x31', '\xf4', '\x05', '\x79'};
  data.addData(const_cast<char*>(payload), sizeof(payload));
}

void addCompleteName(BLEAdvertisementData& data, const char* name) {
  if (!name) return;
  const std::size_t length = std::strlen(name);
  if (length == 0 || length > 29) return;
  const char header[] = {static_cast<char>(length + 1), static_cast<char>(ESP_BLE_AD_TYPE_NAME_CMPL)};
  data.addData(const_cast<char*>(header), sizeof(header));
  data.addData(const_cast<char*>(name), length);
}

std::string clippedString(const char* value, std::size_t maxChars) {
  if (!value) return {};
  std::string out(value);
  if (out.size() > maxChars) {
    out.resize(maxChars);
  }
  return out;
}

bool hasPrefix(const char* value, const char* prefix) {
  if (!value || !prefix) return false;
  return std::strncmp(value, prefix, std::strlen(prefix)) == 0;
}

class CompanionServerCallbacks final : public BLEServerCallbacks {
 public:
  explicit CompanionServerCallbacks(CompanionBleService& service) : service(service) {}

  void onConnect(BLEServer*) override { service.markConnected(true); }

  void onDisconnect(BLEServer*) override { service.markConnected(false); }

#if defined(CONFIG_NIMBLE_ENABLED)
  void onConnect(BLEServer*, ble_gap_conn_desc* desc) override {
    if (desc) {
      service.setPeerAddress(desc->peer_ota_addr.val);
      COMPANION_ANCS.handleServerConnect(desc->conn_handle);
      // Arm the main-loop security pump — the framework's automatic
      // startSecurity-on-connect does not reliably surface the iOS pairing
      // popup (its ble_gap_security_initiate failures are silent), so
      // processPending() re-initiates with logging and a retry.
      service.armSecurity(desc->conn_handle);
    }
    service.markConnected(true);
  }

  void onDisconnect(BLEServer*, ble_gap_conn_desc* desc) override {
    if (desc) {
      COMPANION_ANCS.handleServerDisconnect(desc->conn_handle);
    }
    service.disarmSecurity();
    service.markConnected(false);
  }
#endif

 private:
  CompanionBleService& service;
};

class CompanionSecurityCallbacks final : public BLESecurityCallbacks {
 public:
#if defined(CONFIG_NIMBLE_ENABLED)
  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    if (desc) {
      LOG_INF("X4CMP", "BLE authentication complete conn=%u encrypted=%d bonded=%d", desc->conn_handle,
              desc->sec_state.encrypted, desc->sec_state.bonded);
      // Track encryption state (and schedule a pairing retry on failure)
      // before ANCS reacts — ANCS discovery is gated on isEncrypted().
      COMPANION_BLE.handleEncryptionChange(desc);
      // ANCS needs the encrypted/bonded link before it may discover the
      // Apple service — this is where iOS shows its pairing prompt first.
      COMPANION_ANCS.handleAuthenticationComplete(desc);
    }
  }
#endif
};

class CompanionCardWriteCallbacks final : public BLECharacteristicCallbacks {
 public:
  explicit CompanionCardWriteCallbacks(CompanionBleService& service) : service(service) {}

  void onWrite(BLECharacteristic* characteristic) override { service.handleCardWrite(characteristic); }

#if defined(CONFIG_BLUEDROID_ENABLED)
  void onWrite(BLECharacteristic* characteristic, esp_ble_gatts_cb_param_t*) override {
    service.handleCardWrite(characteristic);
  }
#endif

#if defined(CONFIG_NIMBLE_ENABLED)
  void onWrite(BLECharacteristic* characteristic, ble_gap_conn_desc*) override {
    service.handleCardWrite(characteristic);
  }
#endif

 private:
  CompanionBleService& service;
};
}  // namespace

CompanionBleService COMPANION_BLE;

void CompanionBleService::ensureMutex() const {
  if (!stateMutex) {
    stateMutex = xSemaphoreCreateMutex();
  }
}

void CompanionBleService::begin() {
  ensureMutex();
  if (started) return;

  LOG_INF("X4CMP", "Starting BLE companion service");
  BLEDevice::init(CompanionProtocol::DEVICE_NAME);
  BLEDevice::setMTU(517);

  // Bonding + Secure Connections, no MITM/IO — same security config as
  // x4-os CompanionBleService.cpp:146-151; ANCS requires a bonded link.
  BLESecurity* security = new (std::nothrow) BLESecurity();
  if (!security) {
    LOG_ERR("X4CMP", "OOM: BLESecurity");
    return;
  }
  security->setAuthenticationMode(true, false, true);
  security->setCapability(ESP_IO_CAP_NONE);
  security->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  auto* securityCallbacks = new (std::nothrow) CompanionSecurityCallbacks();
  if (!securityCallbacks) {
    LOG_ERR("X4CMP", "OOM: security callbacks");
    return;
  }
  BLEDevice::setSecurityCallbacks(securityCallbacks);

  server = BLEDevice::createServer();
  if (!server) {
    LOG_ERR("X4CMP", "BLE server create failed");
    return;
  }
  serverCallbacks = new (std::nothrow) CompanionServerCallbacks(*this);
  if (!serverCallbacks) {
    LOG_ERR("X4CMP", "OOM: server callbacks");
    return;
  }
  server->setCallbacks(serverCallbacks);

  BLEService* service = server->createService(CompanionProtocol::SERVICE_UUID);
  if (!service) {
    LOG_ERR("X4CMP", "BLE service create failed");
    return;
  }

  BLECharacteristic* cardCharacteristic = service->createCharacteristic(
      CompanionProtocol::CARD_WRITE_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  cardWriteCallbacks = new (std::nothrow) CompanionCardWriteCallbacks(*this);
  if (!cardCharacteristic || !cardWriteCallbacks) {
    LOG_ERR("X4CMP", "card characteristic setup failed");
    return;
  }
  cardCharacteristic->setCallbacks(cardWriteCallbacks);

  actionCharacteristic =
      service->createCharacteristic(CompanionProtocol::ACTION_NOTIFY_UUID,
                                    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  if (!actionCharacteristic) {
    LOG_ERR("X4CMP", "action characteristic setup failed");
    return;
  }
#if !defined(CONFIG_NIMBLE_ENABLED)
  actionCharacteristic->addDescriptor(new (std::nothrow) BLE2902());
#endif
  actionCharacteristic->setValue("{\"schemaVersion\":1,\"type\":\"ready\"}");

  service->start();

  advertising = BLEDevice::getAdvertising();
  // 31-byte adv packet budget: flags (3) + ANCS solicitation (18) = 21 bytes.
  // The per-device name ("xphone X3"/"xphone X4", 11 bytes with header) does
  // not fit alongside them, so it rides in the scan response next to the
  // companion service UUID (18 + 11 = 29 bytes) — the iOS app matches on the
  // service UUID, which CoreBluetooth also reads from the scan response.
  BLEAdvertisementData advertisementData;
  addFlags(advertisementData);
  addAncsSolicitation(advertisementData);
  advertising->setAdvertisementData(advertisementData);

  BLEAdvertisementData scanResponseData;
  scanResponseData.setCompleteServices(BLEUUID(CompanionProtocol::SERVICE_UUID));
  addCompleteName(scanResponseData, CompanionProtocol::DEVICE_NAME);
  advertising->setScanResponseData(scanResponseData);
  advertising->setScanResponse(true);
  // Preferred CONNECTION interval hint in the scan response (0x06/0x12 =
  // 7.5-22.5 ms): keep it aggressive so pairing + ANCS discovery complete
  // fast; the ANCS client renegotiates to low-duty params once subscribed
  // (CompanionAncsClient.cpp maybeRequestConnParams — M2.1b lever 2).
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);

  // M2.1b lever 1: boot starts in the fast-advertising window.
  applyAdvIntervals(AdvMode::Fast);
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  advMode = AdvMode::Fast;
  advFastUntilMs = millis() + kAdvFastWindowMs;
  xSemaphoreGive(stateMutex);

  started = true;
  startAdvertising();
}

// Writes only BLEAdvertising's cached ble_gap_adv_params (no BLE call), so it
// is safe from any task; the values take effect on the next start().
void CompanionBleService::applyAdvIntervals(AdvMode mode) {
  if (!advertising) return;
  if (mode == AdvMode::Fast) {
    advertising->setMinInterval(kAdvFastMinItvl);
    advertising->setMaxInterval(kAdvFastMaxItvl);
  } else {
    advertising->setMinInterval(kAdvSlowMinItvl);
    advertising->setMaxInterval(kAdvSlowMaxItvl);
  }
}

CompanionBleService::AdvMode CompanionBleService::getAdvMode() const {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const AdvMode value = advMode;
  xSemaphoreGive(stateMutex);
  return value;
}

// Main loop only. Demotes fast -> slow once the grace window expires, via
// stop -> set intervals -> start. NimBLE PRESERVES the adv + scan-response
// payloads across adv stop/start: setAdvertisementData()/setScanResponseData()
// hand the raw payload to ble_gap_adv_set_data()/ble_gap_adv_rsp_set_data()
// (host-side copies) and set m_customAdvData/m_customScanResponseData, so
// BLEAdvertising::start() skips its data rebuild and ble_gap_adv_start()
// re-uses the stored payload — only m_advParams (our new intervals) is passed
// again (verified in framework BLEAdvertising.cpp, NimBLE branch). The ANCS
// solicitation packet therefore survives the interval change byte-identical.
void CompanionBleService::tickAdvPolicy() {
  if (!started || !advertising) return;

  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool demote = advertisingWanted && !connected && advMode == AdvMode::Fast &&
                      static_cast<int32_t>(millis() - advFastUntilMs) >= 0;
  xSemaphoreGive(stateMutex);
  if (!demote) return;

  advertising->stop();
  applyAdvIntervals(AdvMode::Slow);
  // If a phone connected between the checks above and this start(), start()
  // fails ("max connections") — harmless: markConnected(false) restarts
  // advertising (back in the fast window) on the next disconnect.
  advertising->start();

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  advMode = AdvMode::Slow;
  xSemaphoreGive(stateMutex);
  LOG_INF("X4CMP", "Advertising demoted to slow interval (400-500 ms) after %lu ms fast window",
          static_cast<unsigned long>(kAdvFastWindowMs));
}

void CompanionBleService::startAdvertising() {
  begin();
  advertisingWanted = true;
  if (advertising) {
    advertising->start();
    setStatus(std::string("Advertising as ") + CompanionProtocol::DEVICE_NAME);
  }
}

void CompanionBleService::stopAdvertising() {
  advertisingWanted = false;
  if (advertising) {
    advertising->stop();
  }
  setStatus(connected ? "Connected" : "Not advertising");
}

void CompanionBleService::shutdownForTransfer() { shutdownRadio(/*releaseMemory=*/true, "File Transfer"); }

void CompanionBleService::suspendForReader() { shutdownRadio(/*releaseMemory=*/false, "Reading"); }

void CompanionBleService::resumeAfterReader() {
  if (started) return;
  LOG_INF("X4CMP", "BLE resume after reader: heap before %u", ESP.getFreeHeap());
  begin();
  COMPANION_ANCS.rearmAfterRadioResume();
  startAdvertising();
  LOG_INF("X4CMP", "BLE resume done: started=%d heap now %u", started ? 1 : 0, ESP.getFreeHeap());
}

void CompanionBleService::shutdownRadio(const bool releaseMemory, const char* reason) {
  if (!started) return;
  LOG_INF("X4CMP", "BLE shutdown (%s, release=%d): heap before %u", reason, releaseMemory ? 1 : 0,
          ESP.getFreeHeap());

  // Clear the flags that could re-enter the stack from callbacks fired
  // during teardown (markConnected -> startAdvertising, security pump).
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  advertisingWanted = false;
  securityPending = false;
  xSemaphoreGive(stateMutex);

  if (advertising) advertising->stop();

#if defined(CONFIG_NIMBLE_ENABLED)
  // Drop the phone link BEFORE deinit and wait for the host task to finish
  // processing the disconnect. BLEDevice::deinit deletes the BLEServer
  // object before stopping the NimBLE host task, so deinit with a live
  // connection races the disconnect event into freed memory (observed:
  // multi_heap_free assert via BLEServer::handleGATTServerEvent ->
  // removePeerDevice on the torn-down server).
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool wasConnected = connected;
  const uint16_t handle = secConnHandle;
  xSemaphoreGive(stateMutex);
  if (wasConnected && handle != 0xffff) {
    ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    // Wait for our onDisconnect callback (clears `connected`), up to 3 s.
    const uint32_t deadline = millis() + 3000;
    while (static_cast<int32_t>(millis() - deadline) < 0) {
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      const bool still = connected;
      xSemaphoreGive(stateMutex);
      if (!still) break;
      delay(50);
    }
    // onDisconnect fires mid-event; give the host task time to finish the
    // rest of the disconnect handling (removePeerDevice etc.).
    delay(500);
  }
#endif

  // The ANCS client caches the nimble_host task handle for stack HWM
  // reporting; that task dies in deinit, so drop the handle first or the
  // next 60 s stats tick dereferences a deleted TCB (observed load fault).
  COMPANION_ANCS.clearHostTaskHandle();

  // releaseMemory=true (File Transfer): controller + host memory returned
  // to the heap; BLE can't come back until reboot — the transfer session
  // ends in esp_restart(), so that is the designed path.
  // releaseMemory=false (Reader): stack stops but stays re-initializable;
  // resumeAfterReader() brings it back without a reboot.
  BLEDevice::deinit(releaseMemory);

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  started = false;
  connected = false;
  encrypted = false;
  secConnHandle = 0xffff;
  statusMessage = std::string("Bluetooth off (") + reason + ")";
  ++revision;
  xSemaphoreGive(stateMutex);
  advertising = nullptr;
  server = nullptr;
  actionCharacteristic = nullptr;

  LOG_INF("X4CMP", "BLE shutdown done: heap now %u", ESP.getFreeHeap());
}

bool CompanionBleService::isConnected() const {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool value = connected;
  xSemaphoreGive(stateMutex);
  return value;
}

uint32_t CompanionBleService::getRevision() const {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const uint32_t value = revision;
  xSemaphoreGive(stateMutex);
  return value;
}

std::string CompanionBleService::getStatusMessage() const {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const std::string value = statusMessage;
  xSemaphoreGive(stateMutex);
  return value;
}

CompanionCardState CompanionBleService::getCard() const {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const CompanionCardState value = card;
  xSemaphoreGive(stateMutex);
  return value;
}

CompanionCardState CompanionBleService::getBlockCard() const {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const CompanionCardState value = blockCard;
  xSemaphoreGive(stateMutex);
  return value;
}

uint32_t CompanionBleService::getBlockCardRevision() const {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const uint32_t value = blockCardRevision;
  xSemaphoreGive(stateMutex);
  return value;
}

bool CompanionBleService::getPeerAddress(char* buf, std::size_t bufSize) const {
  if (!buf || bufSize == 0) return false;
  buf[0] = '\0';
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool has = peerAddress[0] != '\0';
  if (has) snprintf(buf, bufSize, "%s", peerAddress);
  xSemaphoreGive(stateMutex);
  return has;
}

void CompanionBleService::setPeerAddress(const uint8_t* addrLe) {
  if (!addrLe) return;
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  // NimBLE stores addresses little-endian; render most-significant first.
  snprintf(peerAddress, sizeof(peerAddress), "%02x:%02x:%02x:%02x:%02x:%02x", addrLe[5], addrLe[4], addrLe[3],
           addrLe[2], addrLe[1], addrLe[0]);
  xSemaphoreGive(stateMutex);
}

void CompanionBleService::markConnected(bool value) {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  connected = value;
  statusMessage = connected ? "iPhone connected" : "iPhone disconnected";
  ++revision;
  const bool shouldAdvertise = advertisingWanted && !connected;
  xSemaphoreGive(stateMutex);

  LOG_INF("X4CMP", "BLE %s", value ? "connected" : "disconnected");

  if (shouldAdvertise) {
    LOG_INF("X4CMP", "BLE disconnected; restarting companion advertising (fast window)");
    // M2.1b: a disconnect re-enters the fast-adv window for quick-reconnect
    // UX; tickAdvPolicy() (main loop) demotes to slow after the window.
    applyAdvIntervals(AdvMode::Fast);  // param-struct write only, host-task safe
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    advMode = AdvMode::Fast;
    advFastUntilMs = millis() + kAdvFastWindowMs;
    xSemaphoreGive(stateMutex);
    BLEDevice::startAdvertising();
  }
}

bool CompanionBleService::getConnParams(uint16_t& itvl125, uint16_t& latency, uint16_t& timeout10ms) const {
#if defined(CONFIG_NIMBLE_ENABLED)
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool isConn = connected;
  const uint16_t handle = secConnHandle;  // armed on connect, 0xffff after disconnect
  xSemaphoreGive(stateMutex);
  if (!isConn || handle == 0xffff) return false;

  ble_gap_conn_desc desc;
  if (ble_gap_conn_find(handle, &desc) != 0) return false;
  itvl125 = desc.conn_itvl;                 // x 1.25 ms
  latency = desc.conn_latency;              // skipped events
  timeout10ms = desc.supervision_timeout;   // x 10 ms
  return true;
#else
  (void)itvl125;
  (void)latency;
  (void)timeout10ms;
  return false;
#endif
}

// NimBLE host task (connect callback): state flips only, no BLE calls here.
// The 600ms fuse lets the fresh connection settle before processPending()
// (main loop) calls ble_gap_security_initiate.
void CompanionBleService::armSecurity(uint16_t connHandle) {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  secConnHandle = connHandle;
  encrypted = false;
  securityPending = true;
  securityAttempts = 0;
  securityDueAtMs = millis() + 600;
  xSemaphoreGive(stateMutex);
}

void CompanionBleService::disarmSecurity() {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  secConnHandle = 0xffff;
  encrypted = false;
  securityPending = false;
  xSemaphoreGive(stateMutex);
}

bool CompanionBleService::isEncrypted() const {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool value = encrypted;
  xSemaphoreGive(stateMutex);
  return value;
}

// NimBLE host task (ENC_CHANGE via onAuthenticationComplete): keep it light —
// mutex state flips, status text and logs only; the pairing retry itself runs
// on the main loop via processPending().
void CompanionBleService::handleEncryptionChange(ble_gap_conn_desc* desc) {
#if defined(CONFIG_NIMBLE_ENABLED)
  if (!desc) return;

  if (desc->sec_state.encrypted) {
    ensureMutex();
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    encrypted = true;
    securityPending = false;
    xSemaphoreGive(stateMutex);
    // A repeat connection with an existing bond lands here with encrypted=1
    // and no popup — that is the normal path, not an error.
    LOG_INF("X4CMP", "BLE link encrypted bonded=%d", desc->sec_state.bonded);
    setStatus("Paired & encrypted");
    return;
  }

  // Auth completed without encryption = pairing failed or was rejected.
  bool retry = false;
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (securityAttempts == 0) {
    ++securityAttempts;
    securityPending = true;
    securityDueAtMs = millis() + 2000;
    retry = true;
  } else {
    securityPending = false;
  }
  xSemaphoreGive(stateMutex);
  LOG_ERR("X4CMP", "BLE pairing failed (auth complete, link not encrypted)%s", retry ? "; retrying" : "");
  setStatus(retry ? "Pairing failed; retrying" : "Pairing failed - check iPhone");
#else
  (void)desc;
#endif
}

void CompanionBleService::setStatus(const std::string& message) {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  statusMessage = message;
  ++revision;
  xSemaphoreGive(stateMutex);
}

void CompanionBleService::updateStatus(const std::string& message) { setStatus(message); }

void CompanionBleService::publishCard(const CompanionCardState& next, const std::string& message) {
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  CompanionCardState copy = next;
  copy.hasCard = true;
  copy.revision = card.revision + 1;
  card = copy;
  statusMessage = message;
  ++revision;
  xSemaphoreGive(stateMutex);
}

// Runs on the NimBLE host task: copy-and-flag only (small, bounded work).
// The x4-os original parsed the JSON right here and hit a nimble_host stack
// protection fault (docs/x4-core-learnings.md); the parse now happens in
// processPending() on the main loop.
void CompanionBleService::handleCardWrite(BLECharacteristic* characteristic) {
  if (!characteristic) return;
  String value = characteristic->getValue();
  if (value.length() == 0 || value.length() > CompanionProtocol::MAX_CARD_BYTES) {
    setStatus("Rejected card payload");
    return;
  }

  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (pendingCount < PENDING_CAPACITY) {
    pendingPayloads[(pendingHead + pendingCount) % PENDING_CAPACITY].assign(value.c_str(), value.length());
    pendingCount++;
  } else {
    // FIFO full (loop stalled for 4+ connection intervals): drop the OLDEST —
    // the newest write is the freshest state.
    pendingPayloads[pendingHead].assign(value.c_str(), value.length());
    pendingHead = (pendingHead + 1) % PENDING_CAPACITY;
  }
  xSemaphoreGive(stateMutex);
}

void CompanionBleService::processPending() {
  // Drain the whole FIFO in arrival order — parts of a split snapshot must
  // all land in the same wake of the loop when they queued together.
  ensureMutex();
  while (true) {
    std::string payload;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const bool ready = pendingCount > 0;
    if (ready) {
      payload.swap(pendingPayloads[pendingHead]);
      pendingHead = (pendingHead + 1) % PENDING_CAPACITY;
      pendingCount--;
    }
    xSemaphoreGive(stateMutex);
    if (!ready) break;
    applyCardPayload(payload);
  }

#if defined(CONFIG_NIMBLE_ENABLED)
  // Security pump: initiate pairing from the main loop (never the host task)
  // once the connection has settled. The framework attempts this on connect
  // too, but swallows failures — this path logs the rc and retries once.
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool wantSecurity = securityPending && connected && !encrypted;
  const uint32_t dueAtMs = securityDueAtMs;
  const uint16_t handle = secConnHandle;
  xSemaphoreGive(stateMutex);

  if (wantSecurity && millis() >= dueAtMs) {
    const int rc = ble_gap_security_initiate(handle);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
      // 0: pairing/encryption started, wait for ENC_CHANGE. EALREADY: the
      // framework's on-connect attempt is already in flight — same wait.
      LOG_INF("X4CMP", "Security initiate conn=%u rc=%d (%s)", handle, rc, rc == 0 ? "started" : "already running");
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      securityPending = false;
      xSemaphoreGive(stateMutex);
      setStatus("Pairing with iPhone...");
    } else {
      LOG_ERR("X4CMP", "ble_gap_security_initiate conn=%u failed rc=%d", handle, rc);
      bool retry = false;
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      if (securityAttempts == 0) {
        ++securityAttempts;
        securityDueAtMs = millis() + 2000;
        retry = true;
      } else {
        securityPending = false;
      }
      xSemaphoreGive(stateMutex);
      if (!retry) {
        char failMsg[48];
        snprintf(failMsg, sizeof(failMsg), "Pairing failed (rc=%d)", rc);
        setStatus(failMsg);
      }
    }
  }
#endif
}

bool CompanionBleService::applyCardPayload(const std::string& payload) {
  if (payload.empty() || payload.size() > CompanionProtocol::MAX_CARD_BYTES) {
    setStatus("Rejected card payload");
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    LOG_ERR("X4CMP", "Card JSON parse error: %s", err.c_str());
    setStatus("Bad card JSON");
    return false;
  }

  const char* type = doc["type"] | "";
  if (hasPrefix(type, "camera.image.")) {
    // Camera transfer is stripped in xphone-os M2 — drop gracefully so an
    // unmodified iOS app doesn't wedge (it just sees no preview progress).
    setStatus("Camera not supported");
    return false;
  }

  // R2 Read — control messages from the phone that never become cards.
  // Latched here (main loop) and consumed by main.cpp's pump; the pump owns
  // any SD scanning / scene switching so this parser stays cheap.
  if (std::strcmp(type, "reader.shelf.request") == 0) {
    shelfRequested = true;
    return true;
  }
  if (std::strcmp(type, "notif.filter") == 0) {
    // "apps" is the complete blocklist (hidden apps); an empty array clears
    // it. The JsonDocument owns these string pointers until this function
    // returns; NotificationFilter::set() copies and clips them synchronously
    // into its fixed buffers before persisting the replacement to NVS.
    const char* bundleIds[NotificationFilter::CAPACITY] = {nullptr};
    std::size_t bundleCount = 0;
    JsonArray apps = doc["apps"].as<JsonArray>();
    for (JsonVariant app : apps) {
      if (bundleCount >= NotificationFilter::CAPACITY) break;
      const char* bundleId = app.as<const char*>();
      if (bundleId && bundleId[0]) bundleIds[bundleCount++] = bundleId;
    }
    NOTIFICATION_FILTER.set(bundleIds, bundleCount);
    char message[48];
    std::snprintf(message, sizeof(message), "Notifications: %u app(s) hidden",
                  static_cast<unsigned>(bundleCount));
    setStatus(message);
    LOG_INF("X4CMP", "Notification blocklist applied apps=%u", static_cast<unsigned>(bundleCount));
    return true;
  }
  if (std::strcmp(type, "notif.apps.request") == 0) {
    sendNotifApps();
    return true;
  }
  if (std::strcmp(type, "transfer.start") == 0) {
    transferRequest = TransferRequest::Start;
    return true;
  }
  if (std::strcmp(type, "transfer.stop") == 0) {
    transferRequest = TransferRequest::Stop;
    return true;
  }
  if (std::strcmp(type, "transfer.wifi") == 0) {
    // Wi-Fi credentials for File Transfer STA mode, provisioned over the
    // encrypted BLE link into NVS (the device has no keyboard).
    const char* ssid = doc["ssid"] | "";
    const char* password = doc["password"] | "";
    const bool ok = WifiCreds::save(ssid, password);
    LOG_INF("X4CMP", "Wi-Fi creds %s (ssid=%s)", ok ? "saved" : "SAVE FAILED", ssid);
    sendTransferStatus(ok ? "wifi-saved" : "wifi-error", nullptr, ssid);
    return ok;
  }

  auto* next = new (std::nothrow) CompanionCardState();
  if (!next) {
    setStatus("Card parse OOM");
    return false;
  }

  next->hasCard = true;
  next->id = clippedString(doc["id"] | "card", CompanionProtocol::MAX_ID_CHARS);
  next->kind = clippedString(doc["kind"] | (doc["type"] | ""), CompanionProtocol::MAX_ID_CHARS);
  next->title = clippedString(doc["title"] | "Untitled", CompanionProtocol::MAX_TITLE_CHARS);
  next->body = clippedString(doc["body"] | "", CompanionProtocol::MAX_BODY_CHARS);
  next->state = clippedString(doc["state"] | "", CompanionProtocol::MAX_ID_CHARS);
  next->preset = clippedString(doc["preset"] | "", CompanionProtocol::MAX_TITLE_CHARS);
  next->todayWeather = clippedString(doc["weather"] | "", CompanionProtocol::MAX_TITLE_CHARS);
  next->todayHighLow = clippedString(doc["highLow"] | "", CompanionProtocol::MAX_TITLE_CHARS);
  next->todaySync = clippedString(doc["sync"] | "", CompanionProtocol::MAX_TITLE_CHARS);
  next->mailSource = clippedString(doc["mailSource"] | "", CompanionProtocol::MAX_SOURCE_CHARS);
  next->mailSync = clippedString(doc["mailSync"] | "", CompanionProtocol::MAX_TITLE_CHARS);
  next->endsAtLabel = clippedString(doc["endsAtLabel"] | "", CompanionProtocol::MAX_TODAY_FIELD_CHARS);
  next->part = doc["part"] | 0;
  next->parts = doc["parts"] | 1;
  next->durationMinutes = doc["durationMinutes"] | 0;
  next->remainingMinutes = doc["remainingMinutes"] | 0;
  next->blocksToday = doc["blocksToday"] | 0;
  next->blockStreak = doc["blockStreak"] | 0;
  next->blocksTotal = doc["blocksTotal"] | 0;

  JsonObject source = doc["source"].as<JsonObject>();
  if (!source.isNull()) {
    next->source =
        clippedString(source["displayName"] | (source["app"] | "iPhone"), CompanionProtocol::MAX_SOURCE_CHARS);
  } else {
    next->source = clippedString(doc["source"] | "iPhone", CompanionProtocol::MAX_SOURCE_CHARS);
  }

  JsonArray actions = doc["actions"].as<JsonArray>();
  for (JsonVariant action : actions) {
    if (next->actionCount >= CompanionProtocol::MAX_ACTIONS) break;
    auto& target = next->actions[next->actionCount++];
    target.id = clippedString(action["id"] | "", CompanionProtocol::MAX_ID_CHARS);
    target.label = clippedString(action["label"] | "Send", CompanionProtocol::MAX_ACTION_LABEL_CHARS);
    if (target.id.empty()) target.id = target.label;
  }

  JsonArray todayItems = doc["items"].as<JsonArray>();
  for (JsonVariant item : todayItems) {
    if (next->todayItemCount >= CompanionProtocol::MAX_TODAY_ITEMS) break;
    auto& target = next->todayItems[next->todayItemCount++];
    if (item.is<JsonArray>()) {
      JsonArray fields = item.as<JsonArray>();
      target.kind = clippedString(fields[0] | "", CompanionProtocol::MAX_ID_CHARS);
      target.time = clippedString(fields[1] | "", CompanionProtocol::MAX_TODAY_FIELD_CHARS);
      target.title = clippedString(fields[2] | "", CompanionProtocol::MAX_TITLE_CHARS);
      target.subtitle = clippedString(fields[3] | "", CompanionProtocol::MAX_TODAY_FIELD_CHARS);
      target.state = clippedString(fields[4] | "", CompanionProtocol::MAX_ID_CHARS);
    } else if (item.is<JsonObject>()) {
      JsonObject fields = item.as<JsonObject>();
      target.kind = clippedString(fields["kind"] | "", CompanionProtocol::MAX_ID_CHARS);
      target.time = clippedString(fields["time"] | "", CompanionProtocol::MAX_TODAY_FIELD_CHARS);
      target.title = clippedString(fields["title"] | "", CompanionProtocol::MAX_TITLE_CHARS);
      target.subtitle = clippedString(fields["subtitle"] | "", CompanionProtocol::MAX_TODAY_FIELD_CHARS);
      target.state = clippedString(fields["state"] | "", CompanionProtocol::MAX_ID_CHARS);
    }
  }

  JsonArray mailItems = doc["mailItems"].as<JsonArray>();
  for (JsonVariant item : mailItems) {
    if (next->mailItemCount >= CompanionProtocol::MAX_MAIL_ITEMS) break;
    auto& target = next->mailItems[next->mailItemCount++];
    if (item.is<JsonArray>()) {
      JsonArray fields = item.as<JsonArray>();
      target.id = clippedString(fields[0] | "", CompanionProtocol::MAX_ID_CHARS);
      target.from = clippedString(fields[1] | "", CompanionProtocol::MAX_SOURCE_CHARS);
      target.subject = clippedString(fields[2] | "", CompanionProtocol::MAX_TITLE_CHARS);
      target.preview = clippedString(fields[3] | "", CompanionProtocol::MAX_MAIL_FIELD_CHARS);
      target.time = clippedString(fields[4] | "", CompanionProtocol::MAX_TODAY_FIELD_CHARS);
      target.state = clippedString(fields[5] | "", CompanionProtocol::MAX_ID_CHARS);
    } else if (item.is<JsonObject>()) {
      JsonObject fields = item.as<JsonObject>();
      target.id = clippedString(fields["id"] | "", CompanionProtocol::MAX_ID_CHARS);
      target.from = clippedString(fields["from"] | "", CompanionProtocol::MAX_SOURCE_CHARS);
      target.subject = clippedString(fields["subject"] | "", CompanionProtocol::MAX_TITLE_CHARS);
      target.preview = clippedString(fields["preview"] | "", CompanionProtocol::MAX_MAIL_FIELD_CHARS);
      target.time = clippedString(fields["time"] | "", CompanionProtocol::MAX_TODAY_FIELD_CHARS);
      target.state = clippedString(fields["state"] | "", CompanionProtocol::MAX_ID_CHARS);
    }
  }

  // M3 Priorities — the iOS PrioritiesManager sends priorityItems as
  // 4-element arrays [id, title, note, done] (PrioritiesManager.swift:
  // 124-146); the object form is accepted too, mirroring the x4-os parser
  // (x4-os CompanionBleService.cpp:479-496).
  JsonArray priorityItems = doc["priorityItems"].as<JsonArray>();
  for (JsonVariant item : priorityItems) {
    if (next->priorityItemCount >= CompanionProtocol::MAX_PRIORITY_ITEMS) break;
    auto& target = next->priorityItems[next->priorityItemCount++];
    if (item.is<JsonArray>()) {
      JsonArray fields = item.as<JsonArray>();
      target.id = clippedString(fields[0] | "", CompanionProtocol::MAX_ID_CHARS);
      target.title = clippedString(fields[1] | "", CompanionProtocol::MAX_TITLE_CHARS);
      target.note = clippedString(fields[2] | "", CompanionProtocol::MAX_PRIORITY_FIELD_CHARS);
      target.done = fields[3] | false;
    } else if (item.is<JsonObject>()) {
      JsonObject fields = item.as<JsonObject>();
      target.id = clippedString(fields["id"] | "", CompanionProtocol::MAX_ID_CHARS);
      target.title = clippedString(fields["title"] | "", CompanionProtocol::MAX_TITLE_CHARS);
      target.note = clippedString(fields["note"] | "", CompanionProtocol::MAX_PRIORITY_FIELD_CHARS);
      target.done = fields["done"] | false;
    }
  }

  // Workout — WorkoutManager.swift sends workoutItems as 4-element arrays
  // [id, name, sets, done]; object form accepted for forward compat.
  JsonArray workoutItems = doc["workoutItems"].as<JsonArray>();
  for (JsonVariant item : workoutItems) {
    if (next->workoutItemCount >= CompanionProtocol::MAX_WORKOUT_ITEMS) break;
    auto& target = next->workoutItems[next->workoutItemCount++];
    if (item.is<JsonArray>()) {
      JsonArray fields = item.as<JsonArray>();
      target.id = clippedString(fields[0] | "", CompanionProtocol::MAX_ID_CHARS);
      target.name = clippedString(fields[1] | "", CompanionProtocol::MAX_WORKOUT_NAME_CHARS);
      target.sets = fields[2] | 0;
      target.done = fields[3] | 0;
    } else if (item.is<JsonObject>()) {
      JsonObject fields = item.as<JsonObject>();
      target.id = clippedString(fields["id"] | "", CompanionProtocol::MAX_ID_CHARS);
      target.name = clippedString(fields["name"] | "", CompanionProtocol::MAX_WORKOUT_NAME_CHARS);
      target.sets = fields["sets"] | 0;
      target.done = fields["done"] | 0;
    }
  }
  next->workoutDate = clippedString(doc["workoutDate"] | "", 16);

  // Service-level priorities capture (predicate matches x4-os
  // CompanionBleService.cpp:60 and its isPrioritiesSnapshotCard branch at
  // :502-507): every snapshot lands in the dedicated store, whether or not
  // the Priorities scene has ever been on glass — the sleep frame reads the
  // store, not the card. Safe without the mutex: applyCardPayload runs only
  // on the main loop (processPending marshalling), the store's only task.
  if (next->kind == "priorities.snapshot" || next->id.find("priorities-sync-") == 0 || next->priorityItemCount > 0) {
    PRIORITIES_STORE.updateFromCard(*next);
  }

  // M3 Today — service-level capture, predicate identical to x4-os
  // TodayActivity.cpp:13-15 isTodayCard (kind == "today.snapshot" OR id
  // prefixed "today-sync-"). Every snapshot lands in the dedicated store even
  // while another scene is on glass — the service's single card slot would
  // otherwise be clobbered by the next Block/priorities push. Same
  // mutex-free reasoning as the priorities capture above.
  if (next->kind == "today.snapshot" || next->id.find("today-sync-") == 0) {
    TODAY_STORE.updateFromCard(*next);
    lastTodayCardJson = payload;  // persisted at sleep, re-seeded on wake
  }

  // Workout — service-level capture, same pattern as priorities/today above.
  if (next->kind == "workout.snapshot" || next->id.find("workout-sync-") == 0) {
    WORKOUT_STORE.updateFromCard(*next);
    lastWorkoutCardJson = payload;  // persisted at sleep, re-seeded on wake
  }

  // M4.2 Block status — durable capture so checkAutoSleep() and the dormant
  // sleep frame keep the block's active/remaining/end-time state even after a
  // priorities/today push clobbers the service's single card slot. Unconditional
  // for every block-status card (predicate matches BlockScene.cpp isBlockCard).
  // Same mutex-free reasoning as the captures above: main loop only.
  if (next->id == "block-status") {
    BLOCK_STATUS.updateFromCard(*next);
  }

  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  next->revision = card.revision + 1;
  card = *next;
  // M4.2 — snapshot the full block-status card into its own slot so a later
  // priorities/today card overwriting `card` cannot strand BlockScene on
  // "Syncing...". Its dedicated counter advances only for block traffic.
  if (next->id == "block-status") {
    blockCard = *next;
    ++blockCardRevision;
  }
  statusMessage = "Card received";
  ++revision;
  xSemaphoreGive(stateMutex);

  LOG_INF("X4CMP", "Card received id=%s source=%s titleBytes=%u bodyBytes=%u actions=%u today=%u mail=%u prio=%u",
          next->id.c_str(), next->source.c_str(), static_cast<unsigned>(next->title.size()),
          static_cast<unsigned>(next->body.size()), static_cast<unsigned>(next->actionCount),
          static_cast<unsigned>(next->todayItemCount), static_cast<unsigned>(next->mailItemCount),
          static_cast<unsigned>(next->priorityItemCount));
  delete next;
  return true;
}

bool CompanionBleService::sendAction(std::size_t actionIndex) {
  CompanionCardState snapshot;
  bool shouldNotify = false;
  uint32_t sequence = 0;

  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (card.hasCard && actionIndex < card.actionCount) {
    snapshot = card;
    sequence = ++actionSequence;
    statusMessage = "Action sent";
    shouldNotify = connected && actionCharacteristic;
  } else {
    statusMessage = "No action selected";
  }
  ++revision;
  xSemaphoreGive(stateMutex);

  if (!shouldNotify) return false;

  JsonDocument doc;
  doc["schemaVersion"] = 1;
  doc["type"] = "card.action";
  doc["cardId"] = snapshot.id;
  doc["actionId"] = snapshot.actions[actionIndex].id;
  doc["label"] = snapshot.actions[actionIndex].label;
  doc["sequence"] = sequence;

  String json;
  serializeJson(doc, json);
  actionCharacteristic->setValue(json);
  actionCharacteristic->notify();
  LOG_INF("X4CMP", "Card action sent cardId=%s actionId=%s sequence=%lu", snapshot.id.c_str(),
          snapshot.actions[actionIndex].id.c_str(), static_cast<unsigned long>(sequence));
  return true;
}

bool CompanionBleService::sendBlockStart(uint16_t minutes, const char* presetId) {
  return sendBlockCommand("block.start", minutes, presetId);
}

bool CompanionBleService::sendBlockBreak(uint16_t minutes) { return sendBlockCommand("block.break", minutes); }

bool CompanionBleService::sendBlockStop() { return sendBlockCommand("block.stop", 0); }

bool CompanionBleService::sendBlockStatus() { return sendBlockCommand("block.status", 0); }

bool CompanionBleService::sendPrioritiesSyncRequest() { return sendCommand("priorities.sync.request"); }

bool CompanionBleService::sendTodaySyncRequest() { return sendCommand("today.sync.request"); }

// Same shape as sendBlockCommand, with the priority.toggle fields the iOS
// side reads (PrioritiesManager.swift handleActionPayload: "id" + "done").
bool CompanionBleService::sendPriorityToggle(const char* itemId, const bool done) {
  if (!itemId || itemId[0] == '\0') return false;

  bool shouldNotify = false;
  uint32_t sequence = 0;

  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  sequence = ++actionSequence;
  statusMessage = connected && actionCharacteristic ? "Priority update sent" : "Companion unavailable";
  shouldNotify = connected && actionCharacteristic;
  ++revision;
  xSemaphoreGive(stateMutex);

  if (!shouldNotify) return false;

  JsonDocument doc;
  doc["schemaVersion"] = 1;
  doc["type"] = "priority.toggle";
  doc["id"] = itemId;
  doc["done"] = done;
  doc["sequence"] = sequence;

  String json;
  serializeJson(doc, json);
  actionCharacteristic->setValue(json);
  actionCharacteristic->notify();
  LOG_INF("X4CMP", "Priority toggle sent id=%s done=%d sequence=%lu", itemId, done ? 1 : 0,
          static_cast<unsigned long>(sequence));
  return true;
}

bool CompanionBleService::sendWorkoutSyncRequest() { return sendCommand("workout.sync.request"); }

// Absolute count, not a delta: a retried/duplicated BLE packet cannot
// double-count a set (WorkoutManager.swift handleActionPayload: "id" + "done").
bool CompanionBleService::sendWorkoutSet(const char* itemId, const int done) {
  if (!itemId || itemId[0] == '\0') return false;

  bool shouldNotify = false;
  uint32_t sequence = 0;

  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  sequence = ++actionSequence;
  statusMessage = connected && actionCharacteristic ? "Workout update sent" : "Companion unavailable";
  shouldNotify = connected && actionCharacteristic;
  ++revision;
  xSemaphoreGive(stateMutex);

  if (!shouldNotify) return false;

  JsonDocument doc;
  doc["schemaVersion"] = 1;
  doc["type"] = "workout.set";
  doc["id"] = itemId;
  doc["done"] = done;
  doc["sequence"] = sequence;

  String json;
  serializeJson(doc, json);
  actionCharacteristic->setValue(json);
  actionCharacteristic->notify();
  LOG_INF("X4CMP", "Workout set sent id=%s done=%d sequence=%lu", itemId, done,
          static_cast<unsigned long>(sequence));
  return true;
}

bool CompanionBleService::sendCommand(const char* type) {
  bool shouldNotify = false;
  uint32_t sequence = 0;

  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  sequence = ++actionSequence;
  statusMessage = connected && actionCharacteristic ? "Command sent" : "Companion unavailable";
  shouldNotify = connected && actionCharacteristic;
  ++revision;
  xSemaphoreGive(stateMutex);

  if (!shouldNotify) return false;

  JsonDocument doc;
  doc["schemaVersion"] = 1;
  doc["type"] = type;
  doc["sequence"] = sequence;

  String json;
  serializeJson(doc, json);
  actionCharacteristic->setValue(json);
  actionCharacteristic->notify();
  LOG_INF("X4CMP", "Command sent type=%s sequence=%lu bytes=%u", type, static_cast<unsigned long>(sequence),
          static_cast<unsigned>(json.length()));
  return true;
}

bool CompanionBleService::sendBlockCommand(const char* type, uint16_t minutes, const char* presetId) {
  bool shouldNotify = false;
  uint32_t sequence = 0;

  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  sequence = ++actionSequence;
  statusMessage = connected && actionCharacteristic ? "Block command sent" : "Block unavailable";
  shouldNotify = connected && actionCharacteristic;
  ++revision;
  xSemaphoreGive(stateMutex);

  if (!shouldNotify) return false;

  JsonDocument doc;
  doc["schemaVersion"] = 1;
  doc["type"] = type;
  if (presetId && presetId[0] != '\0') {
    doc["presetId"] = presetId;
    doc["preset"] = presetId;
  }
  if (minutes > 0) {
    doc["minutes"] = minutes;
  }
  doc["sequence"] = sequence;

  String json;
  serializeJson(doc, json);
  actionCharacteristic->setValue(json);
  actionCharacteristic->notify();
  LOG_INF("X4CMP", "Block command sent type=%s sequence=%lu bytes=%u", type, static_cast<unsigned long>(sequence),
          static_cast<unsigned>(json.length()));
  return true;
}

// --- R2 Read: shelf listing + transfer status --------------------------------

bool CompanionBleService::consumeShelfRequest() {
  const bool was = shelfRequested;
  shelfRequested = false;
  return was;
}

CompanionBleService::TransferRequest CompanionBleService::consumeTransferRequest() {
  const TransferRequest req = transferRequest;
  transferRequest = TransferRequest::None;
  return req;
}

void CompanionBleService::sendTransferStatus(const char* state, const char* ip, const char* detail) {
  if (!isConnected() || !actionCharacteristic) return;

  JsonDocument doc;
  doc["schemaVersion"] = 1;
  doc["type"] = "transfer.status";
  doc["state"] = state;
  if (ip && ip[0]) doc["ip"] = ip;
  if (detail && detail[0]) doc["detail"] = detail;

  String json;
  serializeJson(doc, json);
  actionCharacteristic->setValue(json);
  actionCharacteristic->notify();
  LOG_INF("X4CMP", "Transfer status sent state=%s ip=%s", state, ip ? ip : "-");
}

void CompanionBleService::sendReaderShelf() {
  if (!isConnected() || !actionCharacteristic) {
    return;
  }

  // Notify size ceiling: NimBLE truncates a notification to ATT_MTU-3 bytes
  // (it does not fragment), and a truncated chunk is unparseable JSON — so
  // size chunks to the live MTU. iPhones negotiate 527 in practice; the 185
  // fallback covers the pre-exchange window.
  uint16_t mtu = 185;
#if defined(CONFIG_NIMBLE_ENABLED)
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const uint16_t connHandle = secConnHandle;
  xSemaphoreGive(stateMutex);
  if (connHandle != 0xffff) {
    const uint16_t live = ble_att_mtu(connHandle);
    if (live >= 23) mtu = live;
  }
#endif
  const size_t chunkBudget = static_cast<size_t>(mtu) - 3;

  // Envelope worst case: tok is millis() (10 digits), seq 3 digits.
  // {"schemaVersion":1,"type":"reader.shelf","tok":4294967295,"seq":999,"done":false,"books":[...]}
  constexpr size_t kEnvelopeOverhead = 96;
  const size_t booksBudget = chunkBudget > kEnvelopeOverhead ? chunkBudget - kEnvelopeOverhead : 0;

  const uint32_t token = millis();
  uint16_t seq = 0;
  String books;  // accumulated '["name",size]' entries, comma-joined
  books.reserve(booksBudget + 8);

  auto sendChunk = [&](const bool done) {
    char json[560];
    snprintf(json, sizeof(json),
             "{\"schemaVersion\":1,\"type\":\"reader.shelf\",\"tok\":%lu,\"seq\":%u,\"done\":%s,\"books\":[%s]}",
             static_cast<unsigned long>(token), static_cast<unsigned>(seq), done ? "true" : "false", books.c_str());
    actionCharacteristic->setValue(json);
    actionCharacteristic->notify();
    seq++;
    books = "";
    // Let the NimBLE host task drain its notify buffers between chunks.
    delay(20);
  };

  // Scan /books (where app uploads land and the Reader looks first). All SD
  // access on the main loop, same rule as ReaderScene::scanDir.
  int count = 0;
  if (SdMan.ready() || SdMan.begin()) {
    FsFile dir = SdMan.open("/books", O_RDONLY);
    if (dir && dir.isDir()) {
      FsFile f;
      JsonDocument entryDoc;  // per-entry, for correct JSON string escaping
      while (f.openNext(&dir, O_RDONLY) && count < 128) {
        char name[96];
        const size_t got = f.getName(name, sizeof(name));
        const size_t len = got > 0 ? std::strlen(name) : 0;
        const bool isEpub = len >= 5 && strcasecmp(name + len - 5, ".epub") == 0;
        const bool isTxt = len >= 4 && strcasecmp(name + len - 4, ".txt") == 0;
        if (got > 0 && got < sizeof(name) && !f.isDir() && name[0] != '.' && (isEpub || isTxt)) {
          entryDoc.clear();
          JsonArray entry = entryDoc.to<JsonArray>();
          entry.add(name);
          entry.add(static_cast<uint32_t>(f.fileSize()));
          String piece;
          serializeJson(entryDoc, piece);
          if (!books.isEmpty() && books.length() + 1 + piece.length() > booksBudget) {
            sendChunk(false);
          }
          if (piece.length() <= booksBudget) {
            if (!books.isEmpty()) books += ',';
            books += piece;
            count++;
          }
        }
        f.close();
        yield();
      }
      dir.close();
    }
  }

  sendChunk(true);  // final chunk carries done=true (and any remaining books)
  LOG_INF("X4CMP", "Reader shelf sent: %d book(s), %u chunk(s), mtu=%u", count, static_cast<unsigned>(seq), mtu);
}

void CompanionBleService::sendNotifApps() {
  if (!isConnected() || !actionCharacteristic) return;

  // Like reader.shelf, every notification must fit in one ATT payload: NimBLE
  // truncates instead of fragmenting. The app cache is only 24 entries, but
  // full bundle ids plus display names easily exceed one iPhone-sized notify.
  uint16_t mtu = 185;
#if defined(CONFIG_NIMBLE_ENABLED)
  ensureMutex();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const uint16_t connHandle = secConnHandle;
  xSemaphoreGive(stateMutex);
  if (connHandle != 0xffff) {
    const uint16_t live = ble_att_mtu(connHandle);
    if (live >= 23) mtu = live;
  }
#endif
  const std::size_t chunkBudget = static_cast<std::size_t>(mtu) - 3;

  // Worst-case envelope includes a 10-digit token and a 2-digit sequence.
  // Individual entries are serialized separately so names remain valid JSON
  // even if an app uses quotes or another escapable character.
  constexpr std::size_t kEnvelopeOverhead = 100;
  const std::size_t appsBudget = chunkBudget > kEnvelopeOverhead ? chunkBudget - kEnvelopeOverhead : 0;
  const uint32_t token = millis();
  uint16_t sequence = 0;
  String appsJson;
  appsJson.reserve(appsBudget + 8);

  auto sendChunk = [&](const bool done) {
    char json[560];
    std::snprintf(json, sizeof(json),
                  "{\"schemaVersion\":1,\"type\":\"notif.apps\",\"tok\":%lu,\"seq\":%u,\"done\":%s,\"apps\":[%s]}",
                  static_cast<unsigned long>(token), static_cast<unsigned>(sequence), done ? "true" : "false",
                  appsJson.c_str());
    actionCharacteristic->setValue(json);
    actionCharacteristic->notify();
    ++sequence;
    appsJson = "";
    // A short yield avoids filling NimBLE's finite notify buffers when the
    // cache needs several chunks at the negotiated MTU.
    delay(20);
  };

  std::size_t sentApps = 0;
  for (std::size_t i = 0; i < COMPANION_ANCS.appCount(); ++i) {
    char bundle[NotificationStore::APP_ID_CHARS];
    char name[24];
    uint16_t notifCount = 0;
    if (!COMPANION_ANCS.getApp(i, bundle, sizeof(bundle), name, sizeof(name), &notifCount)) continue;

    JsonDocument entryDoc;
    entryDoc["id"] = bundle;
    entryDoc["name"] = name;
    entryDoc["n"] = notifCount;  // notifications seen this power cycle (picker sort)
    String piece;
    serializeJson(entryDoc, piece);

    if (!appsJson.isEmpty() && appsJson.length() + 1 + piece.length() > appsBudget) sendChunk(false);
    if (piece.length() > appsBudget) {
      LOG_ERR("X4CMP", "Notification app entry exceeds MTU budget id=%s", bundle);
      continue;
    }
    if (!appsJson.isEmpty()) appsJson += ',';
    appsJson += piece;
    ++sentApps;
  }

  sendChunk(true);
  LOG_INF("X4CMP", "Notification apps sent: %u app(s), %u chunk(s), mtu=%u",
          static_cast<unsigned>(sentApps), static_cast<unsigned>(sequence), mtu);
}
