#pragma once

// xphone-os R2 — File Transfer scene: Wi-Fi on, HTTP server up, books move.
//
// Modeled on CrossPoint's CrossPointWebServerActivity (x4-os
// src/activities/network/CrossPointWebServerActivity.cpp) with the setup
// maze removed: instead of an on-device network picker + password keyboard,
// STA credentials arrive from the Flowe app over BLE (WifiCreds/NVS) and the
// phone can start/stop the whole mode remotely ("transfer.start" command —
// main.cpp pumps it into showFileTransferAutoStart()).
//
// Station mode only: the device joins the user's Wi-Fi. (A softAP/hotspot
// fallback shipped briefly but was unreliable and saved almost no RAM to
// remove-at-compile-time, so it's gone — provisioning happens in the app.)
//
// States: Idle -> Connecting -> Running | Failed. The scene's handleInput
// tick pumps FileTransferServer::handleClient(); all SD access stays on the
// main loop. Exiting after the radio was up restarts the device
// (CrossPoint's silentRestart lesson: rebooting is the reliable way to
// defragment the heap after a Wi-Fi session).

#include "../Scene.h"
#include "../net/FileTransferServer.h"

class FileTransferScene : public Scene {
 public:
  void onEnter() override;
  void onExit() override;
  void handleInput(Input& in) override;
  void render(Gfx& gfx) override;
  const char* const* softKeys() const override;

  // Phone-initiated start (BLE "transfer.start"): skip the Idle menu and
  // bring up STA with saved creds; reports "needs-wifi" if none saved.
  void autoStart();
  // Phone-initiated stop (BLE "transfer.stop"): ack + restart.
  void stopAndRestart();

  bool radioActive() const { return _state != State::Idle && _state != State::Failed; }

 private:
  enum class State : uint8_t { Idle, Connecting, Running, Failed };

  void startSta();
  void pollConnecting();
  void startServerOrFail();
  void exitScene();  // restart when the radio was ever up, else launcher

  State _state = State::Idle;
  bool _radioWasUp = false;  // any WiFi.mode() call happened -> exit restarts
  char _ssid[64] = {0};
  char _password[64] = {0};
  char _ip[16] = {0};
  const char* _failReason = "";
  uint32_t _connectStartMs = 0;
  uint32_t _lastPollMs = 0;

  // On-glass activity line, refreshed only on meaningful change (e-ink).
  uint16_t _shownRequests = 0;
  uint32_t _shownKb = 0;

  FileTransferServer _server;
};
