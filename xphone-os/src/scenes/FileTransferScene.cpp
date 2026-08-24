#include <Preferences.h>
#include "FileTransferScene.h"

#include <ESPmDNS.h>
#include <InflateReader.h>
#include <WiFi.h>
#include <esp_system.h>

#include <cstdio>
#include <cstring>

#include "../Fonts.h"
#include "../ble/CompanionBleService.h"
#include "../net/WifiCreds.h"
#include "AppScenes.h"

namespace {
constexpr int kMarginX = 20;  // matches SettingsScene / CrossPoint contentSidePadding
constexpr int kHeaderH = 46;

constexpr uint32_t kStaTimeoutMs = 20000;
// How long a radio-down failure lingers on glass before the auto-restart
// that restores BLE (long enough to read; short enough that the phone's
// wait feels like a hiccup, not a hang).
constexpr uint32_t kFailedLingerMs = 6000;
// CrossPoint pumps up to 500 handleClient calls per activity loop; our main
// loop already ticks every 10 ms and one call drains one full request
// synchronously, so a small burst per tick is enough.
constexpr int kPumpPerTick = 8;
// W2: how long the hotspot waits for a phone before giving the radio back.
// The phone's join dialog is a human step; two minutes is generous for it
// and short enough that a dismissed dialog cannot flatten the battery.
constexpr uint32_t kApNoClientTimeoutMs = 120000;
// Same patience for a LAN session that goes quiet. A phone that dies
// mid-upload used to strand the device in Wi-Fi mode for ever, holding BLE
// down. Generous, because a big book can pause between HTTP requests while
// the phone reads it off storage. (2026-08-19)
constexpr uint32_t kStaIdleTimeoutMs = 180000;
// Parked = on this scene with NO session running (Idle, or the needs-wifi
// RETRY screen). BLE stays up in these states, so nothing is lost by
// leaving; staying forever is what ate start requests on an unattended
// reader.
constexpr uint32_t kParkedTimeoutMs = 120000;

constexpr const char* kHostname = "xphone";

// Last 802.11 disconnect reason the STA event hook saw (2=AUTH_EXPIRE,
// 15/204=4-way handshake i.e. wrong password, 201=NO_AP_FOUND). Written by
// the WiFi event task, read by the scene tick — volatile is enough for a
// single int flag.
volatile int gLastStaDisconnectReason = 0;
}  // namespace

void FileTransferScene::onEnter() {
  _state = State::Idle;
  _parkedSinceMs = millis();
  _radioWasUp = false;
  _directMode = false;
  _shownApClients = -1;
  _failReason = "";
  _failedAtMs = 0;
  _ip[0] = '\0';
  _shownRequests = 0;
  _shownKb = 0;
  WifiCreds::load(_ssid, sizeof(_ssid), _password, sizeof(_password));
  markDirty();
}

void FileTransferScene::onExit() {
  // Normal exits go through exitScene() (restart). This covers the OS-wide
  // long-press-BACK -> launcher jump in SceneManager::loop, which bypasses
  // the scene: if the radio ever came up, restarting is the only clean
  // teardown (CrossPoint silentRestart lesson — heap defrag after Wi-Fi).
  if (_radioWasUp) {
    Serial.println("[xphone-os] transfer: exiting with radio up; restarting");
    Serial.flush();
    COMPANION_BLE.sendTransferStatus("stopped");
    delay(120);  // let the notify leave the NimBLE queue
    esp_restart();
  }
}

const char* const* FileTransferScene::softKeys() const {
  static constexpr const char* kIdleKeys[4] = {"BACK", "SYNC", nullptr, nullptr};
  static constexpr const char* kIdleNoCredsKeys[4] = {"BACK", nullptr, nullptr, nullptr};
  static constexpr const char* kConnectingKeys[4] = {"CANCEL", nullptr, nullptr, nullptr};
  static constexpr const char* kRunningKeys[4] = {"EXIT", nullptr, nullptr, nullptr};
  static constexpr const char* kFailedKeys[4] = {"BACK", "RETRY", nullptr, nullptr};
  switch (_state) {
    case State::Idle:       return _ssid[0] ? kIdleKeys : kIdleNoCredsKeys;
    case State::Connecting: return kConnectingKeys;
    case State::Running:    return kRunningKeys;
    case State::Failed:     return kFailedKeys;
  }
  return kIdleKeys;
}

void FileTransferScene::autoStart() {
  WifiCreds::load(_ssid, sizeof(_ssid), _password, sizeof(_password));
  if (_ssid[0]) {
    startSta();
  } else {
    // No creds: tell the app so it can prompt for Wi-Fi instead of hanging.
    _failReason = "No Wi-Fi saved";
    _state = State::Failed;
    COMPANION_BLE.sendTransferStatus("needs-wifi");
    markDirty();
  }
}

void FileTransferScene::autoStartDirect() { startAp(); }

void FileTransferScene::restartFromCard(const bool direct) {
  switch (_state) {
    case State::Connecting:
      COMPANION_BLE.sendTransferStatus("connecting", nullptr,
                                       _directMode ? nullptr : _ssid);
      return;
    case State::Running:
      // A session is already serving; tell the phone where, so it can
      // adopt instead of hunting.
      COMPANION_BLE.sendTransferStatus("running", _ip,
                                       _directMode ? nullptr : _ssid);
      return;
    default:
      // Idle, Failed, needs-wifi: exactly as if the scene were entered
      // fresh for this card.
      onEnter();
      if (direct) autoStartDirect();
      else autoStart();
      return;
  }
}

// W2 Direct mode: no infrastructure Wi-Fi at all. The phone already holds
// the AP credentials (wifi.known "ap", minted once, BLE-encrypted); it told
// us to raise the hotspot and will join programmatically. CrossPoint ships
// this AP shape on the same chip: channel 1, max 4 clients, sleep off.
void FileTransferScene::startAp() {
  _failedAtMs = 0;
  char apSsid[33], apPass[17];
  WifiCreds::apCredentials(apSsid, sizeof(apSsid), apPass, sizeof(apPass));
  Serial.printf("[xphone-os] transfer: direct mode, raising \"%s\" (heap %u)\n", apSsid,
                ESP.getFreeHeap());
  COMPANION_BLE.sendTransferStatus("connecting", nullptr, apSsid);
  delay(600);  // one low-duty conn interval so the notify actually transmits
  COMPANION_BLE.shutdownForTransfer();
  InflateReader::releaseSharedDict();

  _radioWasUp = true;
  _directMode = true;
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(apSsid, apPass, /*channel=*/1, /*ssid_hidden=*/0, /*max_connection=*/4)) {
    Serial.println("[xphone-os] transfer: softAP failed");
    _failReason = "Hotspot failed to start";
    _state = State::Failed;
    _failedAtMs = millis();
    markDirty();
    return;
  }
  // Off the ESP default 192.168.4.x: home LANs use it too (Andrew's does),
  // so a phone-side request to the AP address could reach a ROUTER instead
  // of us. Config after softAP() — before, some cores overwrite it.
  delay(100);
  if (!WiFi.softAPConfig(IPAddress(192, 168, 44, 1), IPAddress(192, 168, 44, 1),
                         IPAddress(255, 255, 255, 0))) {
    Serial.println("[xphone-os] transfer: softAPConfig failed, staying on default subnet");
  }
  snprintf(_ssid, sizeof(_ssid), "%s", apSsid);  // the panel names the hotspot
  snprintf(_ip, sizeof(_ip), "%s", WiFi.softAPIP().toString().c_str());
  Serial.printf("[xphone-os] transfer: hotspot up, ip %s heap %u\n", _ip, ESP.getFreeHeap());
  _apStartedMs = millis();
  _apClientSeenMs = 0;
  _state = State::Running;
  startServerOrFail();
}

void FileTransferScene::stopAndRestart() {
  Serial.println("[xphone-os] transfer: stop requested; restarting");
  Serial.flush();
  COMPANION_BLE.sendTransferStatus("stopped");
  delay(120);
  SCENES.waitFlushIdle();
  esp_restart();
}

void FileTransferScene::startSta() {
  _failedAtMs = 0;  // a RETRY re-arms the join; the linger clock belongs to Failed only
  // pw LENGTH only, never the text: length mismatches expose paste/truncation
  // bugs (reason=15 loops) without putting the secret on a serial port.
  Serial.printf("[xphone-os] transfer: joining \"%s\" (pw len=%u, heap %u)\n", _ssid,
                static_cast<unsigned>(strlen(_password)), ESP.getFreeHeap());
  // Tell the phone what is about to happen, THEN drop BLE: the X3 idles at
  // ~39 KB free with BLE+ANCS up and esp_wifi needs ~50 KB, so the two
  // stacks cannot coexist (measured abort on WiFi.mode with BLE running).
  // The phone finds the server over mDNS/HTTP from here on.
  COMPANION_BLE.sendTransferStatus("connecting", nullptr, _ssid);
  // Give the notify real time to leave. 600 ms was one connection interval,
  // but ANCS asks for 90-180 ms with slave latency 4, so the phone may skip
  // four intervals — up to ~900 ms — before it listens at all. On
  // 2026-08-19 a session logged "Transfer status sent" and "BLE
  // authentication complete" in the same second, then shut BLE down: the
  // phone never saw the notify and waited while the device served an empty
  // network for three minutes. The phone now starts looking on its own
  // after six seconds, so this delay is belt to that braces.
  delay(1200);
  COMPANION_BLE.shutdownForTransfer();
  // If ReaderScene left a 32 KB inflate dict parked, esp_wifi needs that
  // heap back. Usually a no-op (the dict lives only while Reader is up).
  InflateReader::releaseSharedDict();

  _radioWasUp = true;
  WiFi.persistent(false);   // creds live in our NVS keys, not the SDK blob
  // Diagnosis rail: the join loop only ever sees WL_DISCONNECTED (status=6)
  // and can't say WHY association fails. The SDK's disconnect event carries
  // the 802.11 reason code (2=AUTH_EXPIRE, 15/204=4-way handshake = wrong
  // password, 201=NO_AP_FOUND — see esp_wifi_types.h wifi_err_reason_t);
  // log every one. Captureless lambda: no heap, survives the scene.
  WiFi.onEvent(
      [](WiFiEvent_t, WiFiEventInfo_t info) {
        gLastStaDisconnectReason = static_cast<int>(info.wifi_sta_disconnected.reason);
        Serial.printf("[xphone-os] transfer: STA disconnect reason=%d\n",
                      static_cast<int>(info.wifi_sta_disconnected.reason));
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);     // modem sleep costs throughput (CrossPoint keeps it off too)
  WiFi.setHostname(kHostname);
  // Do NOT try raising TX power for a weak link: measured 2026-08-21, it
  // already comes up at 80 (0.25 dBm units) = 20 dBm, the maximum, so
  // setTxPower(WIFI_POWER_19_5dBm) only turns it DOWN. There is no
  // headroom here and the lever does not exist.

  // W1: scan once, then try saved networks strongest-first. The scan also
  // feeds the app's honesty copy ("your device saw CafeBlue") — names are
  // persisted and reported over BLE after the session restart.
  const int found = WiFi.scanNetworks(/*async=*/false, /*hidden=*/false);
  char seen[384];
  size_t seenLen = 0;
  seen[0] = '\0';
  for (int i = 0; i < found && seenLen + 34 < sizeof(seen); i++) {
    const String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    seenLen += (size_t)snprintf(seen + seenLen, sizeof(seen) - seenLen, "%s%s",
                                seenLen ? "\n" : "", s.c_str());
  }
  WifiCreds::saveSeen(seen);
  Serial.printf("[xphone-os] transfer: scan saw %d networks\n", found);

  // Rank the store: saved networks present in the scan by RSSI, then one
  // hidden-network try (last-joined) when nothing was seen at all.
  char last[WifiCreds::kMaxSsid] = {0};
  WifiCreds::lastJoinedSsid(last, sizeof(last));
  _candidateCount = 0;
  int rssiOf[8];
  WifiCreds::Network net;
  for (int slot = 0; WifiCreds::get(slot, &net) && _candidateCount < 8; slot++) {
    int best = -1000;
    for (int i = 0; i < found; i++) {
      if (WiFi.SSID(i) == net.ssid && WiFi.RSSI(i) > best) best = WiFi.RSSI(i);
    }
    if (best == -1000) continue;  // not visible; hidden fallback below
    if (strcmp(net.ssid, last) == 0) best += 3;  // last-joined tie-break
    int j = _candidateCount++;
    while (j > 0 && rssiOf[j - 1] < best) {
      rssiOf[j] = rssiOf[j - 1];
      _candidateSlots[j] = _candidateSlots[j - 1];
      _candidateRssi[j] = _candidateRssi[j - 1];
      j--;
    }
    rssiOf[j] = best;
    _candidateSlots[j] = slot;
    _candidateRssi[j] = best;
  }
  if (_candidateCount == 0 && last[0] != '\0') {
    // Nothing visible: hidden APs don't show in a passive-name scan. Try
    // the last-joined network once before failing.
    for (int slot = 0; WifiCreds::get(slot, &net); slot++) {
      if (strcmp(net.ssid, last) == 0) {
        _candidateSlots[_candidateCount++] = slot;
        break;
      }
    }
  }
  WiFi.scanDelete();

  if (_candidateCount == 0) {
    Serial.println("[xphone-os] transfer: no saved network in sight");
    WifiCreds::saveFailure(_ssid[0] ? _ssid : "", 201 /* NO_AP_FOUND */);
    _failReason = "No saved Wi-Fi in range";
    _state = State::Failed;
    _failedAtMs = millis();
    markDirty();
    return;
  }
  _candidateIdx = -1;
  tryNextCandidate();
}

void FileTransferScene::tryNextCandidate() {
  _candidateIdx++;
  if (_candidateIdx >= _candidateCount) {
    // Out of candidates. Remember why for the post-restart BLE report
    // (W3): the last 802.11 reason the event hook saw is the truth.
    WifiCreds::saveFailure(_ssid, gLastStaDisconnectReason);
    WiFi.disconnect(true);
    _failReason = "Could not join Wi-Fi";
    _state = State::Failed;
    _failedAtMs = millis();
    markDirty();
    return;
  }
  WifiCreds::Network net;
  if (!WifiCreds::get(_candidateSlots[_candidateIdx], &net)) {
    tryNextCandidate();
    return;
  }
  snprintf(_ssid, sizeof(_ssid), "%s", net.ssid);
  snprintf(_password, sizeof(_password), "%s", net.pass);
  Serial.printf("[xphone-os] transfer: trying \"%s\" (%d of %d, pw len=%u, rssi=%d dBm)\n", _ssid,
                _candidateIdx + 1, _candidateCount, static_cast<unsigned>(strlen(_password)),
                _candidateRssi[_candidateIdx]);
  WiFi.begin(_ssid, _password);
  _connectStartMs = millis();
  _lastPollMs = 0;
  _state = State::Connecting;
  markDirty();
}

void FileTransferScene::pollConnecting() {
  const uint32_t now = millis();
  if (now - _lastPollMs < 250) return;
  _lastPollMs = now;

  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    snprintf(_ip, sizeof(_ip), "%s", WiFi.localIP().toString().c_str());
    Serial.printf("[xphone-os] transfer: connected, ip %s\n", _ip);
    WifiCreds::markJoined(_ssid);
    _state = State::Running;
    startServerOrFail();
    return;
  }
  if (now - _connectStartMs > kStaTimeoutMs) {
    Serial.printf("[xphone-os] transfer: join timed out (status=%d)\n", status);
    WiFi.disconnect(/*wifioff=*/false);
    // W1: next saved network before giving up. When the list runs out,
    // tryNextCandidate records the failure for the post-restart BLE report
    // and arms the Failed auto-restart (BLE is down here; the phone would
    // otherwise wait for a server that never comes).
    tryNextCandidate();
  }
}

void FileTransferScene::startServerOrFail() {
  if (MDNS.begin(kHostname)) {
    Serial.printf("[xphone-os] transfer: mDNS http://%s.local/\n", kHostname);
  }
  // Keep the activity line moving during a multi-minute upload: the whole
  // body arrives inside one handleClient() call, so without this hook the
  // panel sits on stale numbers until the file ends. (2026-08-18)
  _server.progressHook = [] {
    if (SCENES.active()) SCENES.active()->markDirty();
    SCENES.renderNow();
  };
  if (!_server.begin()) {
    _failReason = "Server failed to start";
    _state = State::Failed;
    _failedAtMs = millis();  // BLE is down here too — same stranding as the join timeout
    markDirty();
    return;
  }
  // Arm the STA idle guard from the moment we start serving, so a phone
  // that never arrives times out too.
  _lastActivityMs = millis();
  _lastRequestCount = 0;
  _lastMovedBytes = 0;
  COMPANION_BLE.sendTransferStatus("running", _ip, _ssid);
  markDirty();
}

void FileTransferScene::exitScene() {
  if (_radioWasUp) {
    // onExit() (via showLauncher's switchTo) performs the restart; stop the
    // server first so no request is mid-flight during teardown.
    _server.stop();
    MDNS.end();
    showLauncher();  // triggers onExit -> restart
  } else {
    showLauncher();
  }
}

void FileTransferScene::handleInput(Input& in) {
  switch (_state) {
    case State::Idle:
      if (in.wasPressed(Btn::Back)) {
        exitScene();
        return;
      }
      if (in.wasPressed(Btn::Confirm) && _ssid[0]) startSta();
      // Unattended parking guard: this screen with no session running
      // gives up after 2 minutes and goes home. BLE is up; nothing ends.
      if (_parkedSinceMs && millis() - _parkedSinceMs > kParkedTimeoutMs) {
        Serial.println("[xphone-os] transfer: parked 2 min with no session; back to launcher");
        showLauncher();
        return;
      }
      break;

    case State::Connecting:
      if (in.wasPressed(Btn::Back)) {
        WiFi.disconnect(true);
        _state = State::Idle;
        markDirty();
        return;
      }
      pollConnecting();
      break;

    case State::Running: {
      if (in.wasPressed(Btn::Back)) {
        exitScene();
        return;
      }
      for (int i = 0; i < kPumpPerTick; i++) _server.handleClient();

      // STA link health (CrossPoint checks every 2 s; driver auto-reconnects).
      // Direct mode has no STA link to lose — WiFi.status() is never
      // WL_CONNECTED while we ARE the access point, so this check would
      // cry "dropped" every 2 s at a perfectly healthy hotspot. Report
      // client count there instead. (Found live, 2026-08-18.)
      const uint32_t now = millis();
      if (now - _lastPollMs > 2000) {
        _lastPollMs = now;
        if (_directMode) {
          const int clients = WiFi.softAPgetStationNum();
          if (clients != _shownApClients) {
            Serial.printf("[xphone-os] transfer: hotspot clients %d\n", clients);
            _shownApClients = clients;
            if (clients > 0) _apClientSeenMs = now;
            markDirty();
          }
          // Nobody ever joined (the user dismissed the phone's join dialog)
          // or everybody left mid-session: BLE is down, so the phone cannot
          // tell us to stop and auto-sleep is pinned. Come home by
          // ourselves rather than hold the radio up until the battery dies.
          // (Release audit, 2026-08-18.)
          if (clients == 0 && _apClientSeenMs == 0 &&
              now - _apStartedMs > kApNoClientTimeoutMs) {
            Serial.println("[xphone-os] transfer: hotspot had no client; restarting to restore BLE");
            Serial.flush();
            SCENES.waitFlushIdle();
            esp_restart();
          }
          if (clients == 0 && _apClientSeenMs != 0 &&
              now - _apClientSeenMs > kApNoClientTimeoutMs) {
            Serial.println("[xphone-os] transfer: hotspot client left; restarting to restore BLE");
            Serial.flush();
            SCENES.waitFlushIdle();
            esp_restart();
          }
        } else if (WiFi.status() != WL_CONNECTED) {
          Serial.println("[xphone-os] transfer: Wi-Fi dropped; waiting for auto-reconnect");
        } else {
          // STA idle guard. Any HTTP request counts as life. Bytes count
          // too, so one long upload never looks idle.
          const uint32_t moved = _server.bytesUploaded() + _server.bytesDownloaded();
          if (_server.requestCount() != _lastRequestCount || moved != _lastMovedBytes) {
            _lastRequestCount = _server.requestCount();
            _lastMovedBytes = moved;
            _lastActivityMs = now;
          } else if (_lastActivityMs != 0 && now - _lastActivityMs > kStaIdleTimeoutMs) {
            Serial.println("[xphone-os] transfer: no phone for 3 min; restarting to restore BLE");
            Serial.flush();
            SCENES.waitFlushIdle();
            esp_restart();
          }
        }
      }

      // Activity line refresh, only on meaningful change (e-ink discipline):
      // request count moved, or another 256 KB crossed the wire.
      const uint32_t kb = (_server.bytesUploaded() + _server.bytesDownloaded()) / 1024;
      if (_server.requestCount() != _shownRequests || kb / 256 != _shownKb / 256) {
        _shownRequests = _server.requestCount();
        _shownKb = kb;
        markDirty();
      }
      break;
    }

    case State::Failed:
      if (in.wasPressed(Btn::Back)) {
        exitScene();
        return;
      }
      if (in.wasPressed(Btn::Confirm) && _ssid[0]) {
        startSta();
        return;
      }
      // Radio-down failures (join timeout / server start) can't notify the
      // phone — BLE is gone. After a readable pause, exit via the normal
      // restart path so BLE returns and the phone reconnects. _failedAtMs
      // stays 0 for needs-wifi (BLE still up), which keeps its RETRY screen.
      if (_failedAtMs == 0 && _parkedSinceMs &&
          millis() - _parkedSinceMs > kParkedTimeoutMs) {
        // The needs-wifi RETRY screen, unattended. BLE is up; just leave.
        Serial.println("[xphone-os] transfer: parked 2 min with no session; back to launcher");
        showLauncher();
        return;
      }
      if (_failedAtMs && millis() - _failedAtMs > kFailedLingerMs) {
        // The phone is waiting and cannot be told — BLE is gone. Leave the
        // reason where the next boot can find it; CompanionBleService
        // announces it on the next encrypted connect. Without this the
        // phone polled a network that never came up for 75 seconds and
        // showed nothing (2026-08-22).
        Preferences p;
        if (p.begin("transfer", /*readOnly=*/false)) {
          p.putString("lastFail", _failReason[0] ? _failReason : "Wi-Fi failed");
          p.end();
        }
        Serial.println("[xphone-os] transfer: failed with radio down; restarting to restore BLE");
        exitScene();
        return;
      }
      break;
  }
}

void FileTransferScene::render(Gfx& gfx) {
  gfx.drawText(kFontBold, kMarginX, 8, "File Transfer");
  gfx.fillRect(0, kHeaderH - 2, gfx.width(), 2, true);

  const int w = gfx.width();
  const int lineReg = gfx.lineHeight(kFontRegular);
  const int lineBold = gfx.lineHeight(kFontBold);
  int y = kHeaderH + 24;

  switch (_state) {
    case State::Idle:
      gfx.drawText(kFontRegular, kMarginX, y, "Move books with the Flowe app");
      y += lineReg + 4;
      gfx.drawText(kFontRegular, kMarginX, y, "over Wi-Fi.");
      y += lineReg + 24;
      if (_ssid[0]) {
        gfx.drawText(kFontBold, kMarginX, y, "Saved network");
        y += lineBold + 4;
        gfx.drawText(kFontRegular, kMarginX, y, _ssid);
        y += lineReg + 20;
        gfx.drawText(kFontRegular, kMarginX, y, "Press SYNC to join it, or start");
        y += lineReg + 4;
        gfx.drawText(kFontRegular, kMarginX, y, "a sync from the Flowe app.");
      } else {
        gfx.drawText(kFontBold, kMarginX, y, "No Wi-Fi saved yet");
        y += lineBold + 4;
        gfx.drawText(kFontRegular, kMarginX, y, "Add your network in the Flowe");
        y += lineReg + 4;
        gfx.drawText(kFontRegular, kMarginX, y, "app (Read tab > Sync).");
      }
      break;

    case State::Connecting: {
      char line[96];
      snprintf(line, sizeof(line), "Joining %s...", _ssid);
      gfx.drawTextCentered(kFontBold, w / 2, gfx.height() / 2 - lineBold, line);
      break;
    }

    case State::Running: {
      gfx.drawText(kFontBold, kMarginX, y, "Ready to sync");
      y += lineBold + 4;
      gfx.drawText(kFontRegular, kMarginX, y, _ssid);
      y += lineReg + 20;

      char url[40];
      snprintf(url, sizeof(url), "http://%s/", _ip);
      gfx.drawTextCentered(kFontBold, w / 2, y, url);
      y += lineBold + 6;
      gfx.drawTextCentered(kFontRegular, w / 2, y, "Open Flowe > Read > Sync");
      y += lineReg + 24;

      char stats[64];
      const uint32_t kb = (_server.bytesUploaded() + _server.bytesDownloaded()) / 1024;
      snprintf(stats, sizeof(stats), "%u request%s   %lu KB moved", static_cast<unsigned>(_server.requestCount()),
               _server.requestCount() == 1 ? "" : "s", static_cast<unsigned long>(kb));
      gfx.drawTextCentered(kFontRegular, w / 2, y, stats);
      break;
    }

    case State::Failed:
      gfx.drawTextCentered(kFontBold, w / 2, gfx.height() / 2 - 2 * lineBold, _failReason);
      gfx.drawTextCentered(kFontRegular, w / 2, gfx.height() / 2 - lineBold + 8,
                           _ssid[0] ? "RETRY to try again" : "Add Wi-Fi in the Flowe app");
      break;
  }
}
