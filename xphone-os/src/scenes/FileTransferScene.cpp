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
// CrossPoint pumps up to 500 handleClient calls per activity loop; our main
// loop already ticks every 10 ms and one call drains one full request
// synchronously, so a small burst per tick is enough.
constexpr int kPumpPerTick = 8;

constexpr const char* kHostname = "xphone";
}  // namespace

void FileTransferScene::onEnter() {
  _state = State::Idle;
  _radioWasUp = false;
  _failReason = "";
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

void FileTransferScene::stopAndRestart() {
  Serial.println("[xphone-os] transfer: stop requested; restarting");
  Serial.flush();
  COMPANION_BLE.sendTransferStatus("stopped");
  delay(120);
  SCENES.waitFlushIdle();
  esp_restart();
}

void FileTransferScene::startSta() {
  Serial.printf("[xphone-os] transfer: joining \"%s\" (heap %u)\n", _ssid, ESP.getFreeHeap());
  // Tell the phone what is about to happen, THEN drop BLE: the X3 idles at
  // ~39 KB free with BLE+ANCS up and esp_wifi needs ~50 KB, so the two
  // stacks cannot coexist (measured abort on WiFi.mode with BLE running).
  // The phone finds the server over mDNS/HTTP from here on.
  COMPANION_BLE.sendTransferStatus("connecting", nullptr, _ssid);
  delay(600);  // one low-duty conn interval so the notify actually transmits
  COMPANION_BLE.shutdownForTransfer();
  // The reader can't run during a transfer; give any reserved decompression
  // memory to Wi-Fi. The session always ends in esp_restart().
  InflateReader::releaseSharedDict();

  _radioWasUp = true;
  WiFi.persistent(false);   // creds live in our NVS keys, not the SDK blob
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);     // modem sleep costs throughput (CrossPoint keeps it off too)
  WiFi.setHostname(kHostname);
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
    _state = State::Running;
    startServerOrFail();
    return;
  }
  if (now - _connectStartMs > kStaTimeoutMs) {
    Serial.printf("[xphone-os] transfer: join timed out (status=%d)\n", status);
    WiFi.disconnect(true);
    _failReason = "Could not join Wi-Fi";
    _state = State::Failed;
    COMPANION_BLE.sendTransferStatus("failed", nullptr, _failReason);
    markDirty();
  }
}

void FileTransferScene::startServerOrFail() {
  if (MDNS.begin(kHostname)) {
    Serial.printf("[xphone-os] transfer: mDNS http://%s.local/\n", kHostname);
  }
  if (!_server.begin()) {
    _failReason = "Server failed to start";
    _state = State::Failed;
    COMPANION_BLE.sendTransferStatus("failed", nullptr, _failReason);
    markDirty();
    return;
  }
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
      const uint32_t now = millis();
      if (now - _lastPollMs > 2000) {
        _lastPollMs = now;
        if (WiFi.status() != WL_CONNECTED) {
          Serial.println("[xphone-os] transfer: Wi-Fi dropped; waiting for auto-reconnect");
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
      if (in.wasPressed(Btn::Confirm) && _ssid[0]) startSta();
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
