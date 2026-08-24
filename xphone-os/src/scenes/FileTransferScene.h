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

  // W2: phone-initiated Direct mode (BLE "transfer.direct").
  void autoStartDirect();
  // A transfer card arrived while this scene is ALREADY up. Behave exactly
  // like a fresh entry: a live session re-announces itself; anything parked
  // (Idle, needs-wifi, a failure screen) re-runs the start decision. The
  // old behavior — silently ignoring the card — left a reader stuck on the
  // needs-wifi screen eating every start request until a human pressed
  // BACK (found live, 2026-08-23).
  void restartFromCard(bool direct);

  bool radioActive() const { return _state != State::Idle && _state != State::Failed; }

 private:
  enum class State : uint8_t { Idle, Connecting, Running, Failed };

  void startSta();
  void startAp();            // W2 Direct mode: the device's own hotspot
  void pollConnecting();
  void startServerOrFail();
  void exitScene();  // restart when the radio was ever up, else launcher

  void tryNextCandidate();   // W1: advance the scan-ordered join list

  State _state = State::Idle;
  bool _radioWasUp = false;  // any WiFi.mode() call happened -> exit restarts
  bool _directMode = false;  // W2: we ARE the access point (no STA link)
  int _shownApClients = -1;  // last reported hotspot client count
  uint32_t _apStartedMs = 0;
  uint32_t _apClientSeenMs = 0;
  // Idle guard for the INFRASTRUCTURE (STA) path. The hotspot path has had
  // a no-client watchdog since W2, but a LAN session had none: when the
  // phone died mid-upload the device sat in Wi-Fi mode for ever, with BLE
  // down, so the phone then reported "couldn't reach the device". Found by
  // killing the app mid-send (2026-08-19).
  uint32_t _lastActivityMs = 0;
  uint16_t _lastRequestCount = 0;
  uint32_t _lastMovedBytes = 0;
  char _ssid[64] = {0};
  char _password[64] = {0};
  // W1 scan-then-join: saved networks ordered by the session-start scan
  // (strongest seen first, last-joined tie-break, one hidden-network try).
  int _candidateSlots[8] = {0};  // indexes into WifiCreds::get order
  // Signal of each candidate at scan time, in the same order. Diagnostic
  // only: a 4-way handshake that times out (reason 15) looks exactly like
  // a wrong password in the logs, and the RSSI is what tells the two
  // apart. Added 2026-08-21 chasing an X3 that joined fine on a USB cable
  // and stopped once it moved to the pogo rig.
  int _candidateRssi[8] = {0};
  int _candidateCount = 0;
  int _candidateIdx = 0;
  char _ip[16] = {0};
  const char* _failReason = "";
  // Non-zero only for radio-down failures (join timeout / server start):
  // arms the Failed-state auto-restart that brings BLE back for the phone.
  uint32_t _failedAtMs = 0;
  // When this scene started PARKING (Idle with no session, or the
  // needs-wifi screen). An unattended reader must never sit on a dead-end
  // screen forever; after 2 minutes it returns to the launcher on its own.
  uint32_t _parkedSinceMs = 0;
  uint32_t _connectStartMs = 0;
  uint32_t _lastPollMs = 0;

  // On-glass activity line, refreshed only on meaningful change (e-ink).
  uint16_t _shownRequests = 0;
  uint32_t _shownKb = 0;

  FileTransferServer _server;
};
