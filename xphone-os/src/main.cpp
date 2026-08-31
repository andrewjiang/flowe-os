// xphone-os M1 — input + scene manager + launcher shell.
//
// Staged boot (kept from M0.1): serial -> display -> SD self-update (early,
// so a bad build can always be replaced from the card) -> input -> launcher.
// loop() is input update + scene manager only: scenes render exclusively on
// state change (dirty flag), FULL refresh on scene switches, FAST refresh on
// selection moves. Single task, no FreeRTOS render task, no heap in the loop.
//
// Display init sequence copied from x4-os (see M0 notes):
//   * HalDisplay::begin() (x4-os/lib/hal/HalDisplay.cpp:13-19):
//     setDisplayX3() when the device is an X3, then EInkDisplay::begin().
//   * EInkDisplay::begin() selects the panel driver, runs SPI.begin() itself
//     with pins from BoardConfig::ACTIVE.display, and clears the framebuffer
//     to 0xFF (freeink-sdk FreeInkDisplay.cpp:124-176, EpdBus.cpp:5-32).
// Each env compiles exactly one FREEINK_DEVICE_* so there is no runtime
// detection: BoardConfig::DEFAULT_DEVICE is already the right profile
// (BoardConfig.h:874-899).

#include <Arduino.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <InflateReader.h>
#include <SPI.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "BatteryGauge.h"
#include "net/WifiCreds.h"
#include <Preferences.h>
#include "BenchGlass.h"
#include "BlockStatusStore.h"
#include "ClockStore.h"
#include "CompanionSync.h"
#include "Fonts.h"
#include "Gfx.h"
#include "Input.h"
#include "NotificationStore.h"
#include "PrioritiesStore.h"
#include "Scene.h"
#include "TodayStore.h"
#include "WorkoutStore.h"
#include "SdUpdate.h"
#include "StallWatch.h"
#include "Sleep.h"
#include "ble/CompanionAncsClient.h"
#include "ble/CompanionBleService.h"
#include "reader/FbpBook.h"
#include "art/FloweLogo.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "soc/usb_serial_jtag_struct.h"
#include "scenes/AppScenes.h"

// Constructor pins are legacy and unused — EInkDisplay::begin() reads the
// active BoardProfile instead (FreeInkDisplay.cpp:130-137). Pass the profile's
// own constexpr pins (same values HalDisplay passes via x4-os HalGPIO.h:7-12).
static EInkDisplay display(BoardConfig::DEFAULT_DEVICE.display.sclk, BoardConfig::DEFAULT_DEVICE.display.mosi,
                           BoardConfig::DEFAULT_DEVICE.display.cs, BoardConfig::DEFAULT_DEVICE.display.dc,
                           BoardConfig::DEFAULT_DEVICE.display.rst, BoardConfig::DEFAULT_DEVICE.display.busy);

static Gfx gfx(display);
// The one Gfx. Exposed so a scene's onExit() — which gets no Gfx — can put
// the panel back to portrait before the next scene lays out for it.
Gfx* G_GFX = &gfx;
static Input input;

// M2.1b (power lever 3): XP_CPU_MHZ (default 80) now lives in CpuBoost.h,
// together with the work-scoped 160 MHz guard the reader uses around
// CPU-bound jobs (indexing, page compose, cover decode). The park below at
// Stage 6 and the guard's restore target are the same constant by design.
#include "CpuBoost.h"

// ---------------------------------------------------------------------------
// Boot splash — the very first frame on glass. The panel's first two FULL
// refreshes after power-on flash black while the driver conditions the
// particles, so make both flashes carry content: flash 1 paints this splash,
// flash 2 is the launcher's first paint below — press -> splash -> launcher
// on every boot (cold boot and deep-sleep wake share this path). flowe mark
// (sun over water) above the wordmark, "waking up..." under the rule; no
// delays, the rest of boot runs while the splash sits on glass.
// ---------------------------------------------------------------------------

// 1bpp blitter (format per FloweLogo.h: MSB-first, bit 0 = ink; only ink
// pixels are drawn so the paper stays white).
static void drawFloweLogo(Gfx& g, const int x, const int y) {
  const int rowBytes = (FloweLogoWidth + 7) / 8;
  for (int row = 0; row < FloweLogoHeight; ++row) {
    for (int col = 0; col < FloweLogoWidth; ++col) {
      const uint8_t byte = FloweLogoBitmap[row * rowBytes + (col >> 3)];
      if (((byte >> (7 - (col & 7))) & 1) == 0) g.drawPixel(x + col, y + row, true);
    }
  }
}

static void drawBootSplash(Gfx& g) {
  g.clear();
  const int cx = g.width() / 2;
  const int wordmarkY = g.height() * 2 / 5;
  drawFloweLogo(g, cx - FloweLogoWidth / 2, wordmarkY - FloweLogoHeight - 18);
  g.drawTextCentered(kFontBold, cx, wordmarkY, "flowe");
  constexpr int kRuleW = 56;
  const int ruleY = wordmarkY + g.lineHeight(kFontBold) + 10;
  g.fillRect(cx - kRuleW / 2, ruleY, kRuleW, 2, true);
  g.drawTextCentered(kFontSmall, cx, ruleY + 26, "waking up...");
  g.flush(EInkDisplay::FULL_REFRESH);
}

// ---------------------------------------------------------------------------
// Boot sequence with timed stages.
// ---------------------------------------------------------------------------

#include <SDCardManager.h>
#include <XteinkDetect.h>

#include "DeviceKind.h"

bool gDeviceIsX3 = false;  // set in boot() by selectXteinkDevice(); X4 = SDK default

// Remote-debug breadcrumbs for units that never paint (field X3 that hangs
// under xphone-os with no serial access; CrossPoint works). Armed ONLY when
// /boot-trace.txt already exists on the card — the tester creates it, boots,
// pulls the card, and the last appended line names the stage that died.
// Normal boots: one existence probe, zero writes.
static bool gBootTrace = false;
static void bootTrace(const char* stage) {
  if (!gBootTrace) return;
  FsFile f = SdMan.open("/boot-trace.txt", O_WRONLY | O_APPEND);
  if (!f) return;
  char line[96];
  const int n = snprintf(line, sizeof(line), "%8lu ms  %s\n", millis(), stage);
  if (n > 0) f.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(n));
  f.flush();
  f.close();
}

// The same breadcrumb, reachable from the scenes (see BootTrace.h). It also
// feeds the stall watcher, which is what makes a freeze self-reporting: the
// SD write still needs arming, but naming the step costs a short copy and so
// is always on.
void xpTrace(const char* stage) {
  stallwatch::stage(stage);
  bootTrace(stage);
}

// M4.2 wake diagnostic: esp_reset_reason() -> short label. Captured at the very
// top of boot() so the About scene can show whether the X3 power-button wake is
// a genuine deep-sleep resume (DEEPSLEEP) or a full power-on reset (POWERON) —
// the two demand different persistence (RTC vs NVS).
static const char* resetReasonLabel(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "WDT";
    default:                return "OTHER";
  }
}

static void boot() {
  const unsigned long tBoot = millis();

  // Capture the wake diagnostic first — before anything else can perturb it.
  gWakeResetReason = resetReasonLabel(esp_reset_reason());

  // Stage 1: serial. The 250ms pre-begin() stall lets the USB Serial/JTAG
  // peripheral finish power-on / host enumeration on cold boot; the 1ms TX
  // timeout keeps logging from stalling when no host is attached
  // (x4-os/src/main.cpp:309-318).
  delay(250);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(1);
  const unsigned long tSerial = millis();

  // Stage 2: device fingerprint, then display. One binary serves X3 and X4
  // (field brick: X4 bin on an X3 drives SSD1677 protocol at UC8253 glass —
  // frozen panel, "dead" buttons). selectXteinkDevice() probes the X3-only
  // I2C parts (gauge/RTC/IMU) and sets BoardConfig::ACTIVE; it must run
  // BEFORE SD + display bring-up (XteinkDetect.h contract).
  gDeviceIsX3 = freeink::selectXteinkDevice();
  // Say WHICH build this is, before anything can hang. A stuck unit cannot
  // reach the About screen, so until now a field report could not name its
  // firmware at all — and the panel fix for newer X3 units is exactly the
  // kind of thing where "which version are you on" IS the whole diagnosis.
  // The web flasher reads this line straight off the USB serial.
  Serial.printf("[xphone-os] flowe %s (%s)\n", XPHONE_VERSION, XPHONE_GIT_REV_STR);
  Serial.printf("[xphone-os] boot: xteink detect -> %s\n", gDeviceIsX3 ? "X3" : "X4");
  stallwatch::begin();
  if (gDeviceIsX3) {
    display.setDisplayX3();
  }
  // Shared SPI bus, initialized ONCE with the SD card's MISO attached
  // (mirrors x4-os/lib/hal/HalGPIO.cpp:195). Display and SD share SCLK=8 /
  // MOSI=10; SD adds MISO=7 + CS=12 (BoardConfig.h XTEINK_X3/X4 profiles).
  // This must run before display.begin(): EpdBus::begin() calls SPI.begin()
  // with miso=-1 (UC8253/SSD1677 use none), and arduino-esp32 SPIClass::begin
  // is first-call-wins — a MISO-less first init would leave the SD card
  // unreadable. SDCardManager itself never remaps pins on X3/X4 (sd.sclk is
  // unassigned), so this is the only place the bus is configured.
  SPI.begin(BoardConfig::ACTIVE.display.sclk, BoardConfig::ACTIVE.sd.miso, BoardConfig::ACTIVE.display.mosi,
            BoardConfig::ACTIVE.display.cs);

  // Arm the breadcrumb tracer before the display — the prime hang suspect on
  // the affected unit is display.begin()'s BUSY waits. SdMan.begin() is
  // idempotent; sd_update below remounts/reuses the same instance.
  if (SdMan.begin()) {
    FsFile probe = SdMan.open("/boot-trace.txt", O_RDONLY);
    if (probe) {
      probe.close();
      gBootTrace = true;
      bootTrace(gDeviceIsX3 ? "boot: spi up, detect=X3" : "boot: spi up, detect=X4");
    }
    // Did the last run freeze? The watcher wrote WHERE into RTC memory,
    // which the reset button does not clear. Put it on the card now, while
    // nothing is stuck, so the file simply EXISTS for an owner who never
    // prepared anything. Also arm the detailed trace for the next run: the
    // device has now seen a freeze once, so the extra writes are earned.
    char stall[96];
    if (stallwatch::takeReport(stall, sizeof(stall))) {
      Serial.printf("[xphone-os] PREVIOUS RUN %s\n", stall);
      FsFile f = SdMan.open("/flowe-diag.txt", O_WRONLY | O_CREAT | O_APPEND);
      if (f) {
        char line[192];
        const int n = snprintf(line, sizeof(line), "flowe %s (%s) on %s: %s\n", XPHONE_VERSION,
                               XPHONE_GIT_REV_STR, gDeviceIsX3 ? "X3" : "X4", stall);
        if (n > 0) f.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(n));
        f.flush();
        f.close();
      }
      gBootTrace = true;
    }
  }

  bootTrace("display.begin: start");
  display.begin();
  const unsigned long tDisplay = millis();
  bootTrace("display.begin: done");

  Serial.printf("[xphone-os] M1 boot on %s (%ux%u, fb %lu bytes)\n", BoardConfig::ACTIVE.name,
                display.getDisplayWidth(), display.getDisplayHeight(),
                static_cast<unsigned long>(display.getBufferSize()));

  // Stage 2.4: graphics + boot splash, straight after the panel is up so the
  // conditioning flash shows the wordmark instead of a bare black blink.
  // gfx.begin() only fails if the SDK returned no framebuffer — but the SD
  // self-update below MUST still run in that case (it is how a bad build gets
  // replaced), so the fatal bail is deferred past it.
  const bool gfxOk = gfx.begin();
  if (gfxOk) drawBootSplash(gfx);
  const unsigned long tSplash = millis();
  bootTrace(gfxOk ? "gfx: splash drawn" : "gfx: NO FRAMEBUFFER");

  // Stage 2.5: SD firmware self-update — MUST stay this early in boot. Mounts
  // the SD card and, if /update.bin exists at the root, flashes it into the
  // inactive OTA slot and restarts (never returns). Any failure logs + draws
  // an X and falls through so the device is never stranded.
  sd_update::checkAndApply(display);
  const unsigned long tSdUpdate = millis();
  bootTrace("sd-update: checked");

  // Stage 3: input. ADC-ladder buttons per BoardConfig (front 4 on GPIO1,
  // side Up/Down on GPIO2, power on GPIO3).
  input.begin();
  // M5 Phase 1: dedicated 5ms sampling task — presses register (and latch)
  // even while the loop is composing or a flush is in flight.
  input.beginTask();
  bootTrace("input: live");

  // Stage 4: launcher — nothing to draw with if gfx failed above, so log and
  // idle (SD update already ran, so the device remains recoverable).
  if (!gfxOk) {
    Serial.println("[xphone-os] FATAL: no framebuffer from EInkDisplay");
    return;
  }
  // M4.2 last-scene restore: return to whatever scene was on glass at sleep
  // (each scene's onEnter re-requests its companion data). The id is persisted
  // in NVS flash (survives the X3's power-on-reset wake); the key is consumed
  // at boot, so a cold boot finds none and falls through to the launcher.
  // M4.3: seed BLOCK_STATUS from the NVS snapshot BEFORE the first render, so a
  // wake into the Block scene shows the last-known locked view instantly ("until
  // 10:30 AM") instead of "Syncing...". A fresh block-status card after BLE
  // reconnect supersedes the seed. No-op on a cold boot with no snapshot.
  Sleep::seedPersistedBlock();

  uint32_t restoreSceneId = 0;
  if (Sleep::consumeRestoreScene(restoreSceneId)) {
    const SceneId id = static_cast<SceneId>(restoreSceneId);
    gWakeRestoreScene = sceneName(id);  // diagnostic: what we restored
    showSceneById(id);
  } else {
    showLauncher();  // gWakeRestoreScene stays "none"
  }
  SCENES.renderIfDirty(gfx);  // first paint (FULL refresh)
  const unsigned long tPaint = millis();
  gBootTotalMs = tPaint - tBoot;

  // Boot report.
  Serial.printf("[xphone-os] stages ms: serial=%lu display=%lu splash=%lu sdupdate=%lu launcher=%lu total=%lu\n",
                tSerial - tBoot, tDisplay - tSerial, tSplash - tDisplay, tSdUpdate - tSplash, tPaint - tSdUpdate,
                gBootTotalMs);
  Serial.printf("[xphone-os] heap: free=%u minFree=%u largestBlock=%u\n", ESP.getFreeHeap(),
                static_cast<unsigned>(esp_get_minimum_free_heap_size()),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

  // Stage 4.5 (M2.1b follow-up): BQ27220 design-capacity check. AFTER the
  // first paint (the check is a single I2C word read when the value is
  // already right, but the one boot that actually reprograms takes a couple
  // of seconds and must not delay boot-to-glass) and BEFORE BLE so the I2C
  // transaction burst never overlaps radio bring-up. X4 builds compile the
  // no-op stub. See BatteryGauge.cpp for the verified BQ27220 sequence.
  BatteryGauge::ensureDesignCapacity();

  // Stage 5 (M2): radios — deliberately AFTER the launcher's first paint so
  // the UI is on glass before the BLE stack spins up ("UI first, radios
  // second"). begin() advertises the same companion GATT service UUID as
  // x4-os (name is per-device: "xphone X3"/"xphone X4", with the ANCS
  // solicitation UUID in the adv packet); the ANCS client arms itself and
  // starts service discovery once an iPhone connects and the link
  // authenticates (that is when iOS shows its pairing prompt).
  // Inflate dict (32 KB zip window) — current design, after two failed
  // experiments: the dict is heap-claimed only while the Reader scene is up
  // (claimed at radio suspend, freed on exit), and blocking inflate work
  // additionally borrows the framebuffer via InflateReader::lendDict()
  // (ReaderScene.cpp), so paging never depends on finding a contiguous
  // 32 KB heap block at all. History that shaped this: a permanent static
  // BSS dict left X3 BLE with 3.8 KB free (min 3652) — links formed but
  // discovery-time allocations failed and iOS hung at "discovering
  // services"; and heap-parking before BLE was moot because the running
  // heap never had a 32 KB contiguous hole on X3-class devices (103 KB
  // free, largest block 29,684 measured).
  // M4.2 restore + radios: when the restore landed in the Reader, the book
  // state is already resident (~60 KB) and the radio must NOT come up beside
  // it — BLE init in that heap wedges boot (observed hard hang on X4:
  // "Starting BLE companion service" then nothing, 53 KB largest block).
  // Honor the reader's turn-taking invariant from boot: leave the radio
  // down; ReaderScene's exit path resumes it (resumeAfterReader -> begin +
  // ANCS rearm), exactly like an in-session reader entry. ReaderScene's
  // onEnter already set _radioSuspended unconditionally, so the exit resume
  // fires even though there was nothing to suspend.
  if (gCurrentSceneId == SceneId::Reader) {
    Serial.println("[xphone-os] radios deferred: Reader scene restored (resume on reader exit)");
  } else {
    COMPANION_BLE.begin();
    COMPANION_ANCS.begin();
    COMPANION_ANCS.requestPairing();
    Serial.printf("[xphone-os] BLE companion + ANCS armed (%lu ms after boot)\n", millis() - tBoot);
  }

  // Stage 6 (M2.1b): drop the CPU to XP_CPU_MHZ. Placed AFTER BLE begin() so
  // the whole radio bring-up runs at the boot clock and the stack never
  // initializes across a frequency change (80 MHz is fully BLE-capable on
  // the C3, this ordering is just belt-and-braces). Skipped entirely at 160
  // so -DXP_CPU_MHZ=160 is a true no-op A/B switch.
#if XP_CPU_MHZ != 160
  const bool cpuOk = setCpuFrequencyMhz(XP_CPU_MHZ);
  Serial.printf("[xphone-os] cpu: setCpuFrequencyMhz(%d) %s, now %lu MHz\n", XP_CPU_MHZ,
                cpuOk ? "ok" : "FAILED", static_cast<unsigned long>(getCpuFrequencyMhz()));
#endif
}

void setup() { boot(); }

// --- Wake resync state (scene-scoped) --------------------------------------
// Lifted to file scope so xphoneSyncActive() (status-bar sync dot) can read the
// retry backstop's remaining count. Touched only from the main loop
// (pumpCompanionEvents), so no synchronization is needed.
static uint8_t gResyncRetriesLeft = 0;

// Revision of the ACTIVE scene's data rail — the number that advances when the
// on-glass scene's store is refilled. Generalized from the old block-only
// blkRev check so the retry backstop waits on whichever card scene is up.
static uint32_t activeSceneRailRevision() {
  switch (gCurrentSceneId) {
    case SceneId::Block:      return BLOCK_STATUS.revision();
    case SceneId::Priorities: return PRIORITIES_STORE.revision();
    case SceneId::Today:      return TODAY_STORE.revision();
    case SceneId::Workout:    return WORKOUT_STORE.revision();
    default:                  return 0;  // no rail to wait on
  }
}
static bool activeSceneHasRail() {
  switch (gCurrentSceneId) {
    case SceneId::Block:
    case SceneId::Priorities:
    case SceneId::Today:
    case SceneId::Workout:    return true;
    default:                  return false;  // Notifications/Launcher/Settings/About
  }
}

// Sync activity for the status-bar dot. PRESENT while any resync work is
// outstanding: a request armed but unsent, retry attempts still budgeted, or
// the ANCS backfill still draining. GONE once everything is idle.
bool xphoneSyncActive() {
  return CompanionSync::inProgress() || gResyncRetriesLeft > 0 || COMPANION_ANCS.getBackfillRemaining() > 0;
}
// "Busy" = something is actually transmitting right now (a request queued to
// send, or an ANCS attribute fetch in flight). Drives the dot's filled(busy)/
// hollow(waiting) look so it visibly changes on the repaints sync already
// triggers — WITHOUT any periodic forced refresh (see StatusBar.h).
bool xphoneSyncBusy() {
  return CompanionSync::inProgress() || COMPANION_ANCS.isBackfillFetchInFlight();
}

// M2 event marshalling: BLE/ANCS state originates on the NimBLE host task;
// nothing over there draws. The loop below is the only place raw payloads
// are parsed (processPending/processQueue) and the only place scenes are
// marked dirty from radio events — and only when the affected scene is the
// one on glass, so notification bursts can't storm the panel.
static void pumpCompanionEvents() {
  COMPANION_BLE.processPending();  // parse queued card JSON off the host task
  COMPANION_ANCS.processQueue();   // parse queued ANCS packets off the host task

  static bool lastConnected = false;
  const bool connected = COMPANION_BLE.isConnected();
  if (connected != lastConnected) {
    lastConnected = connected;
    markLauncherDirtyIfActive();  // status-bar dot actually changed (dot only)
  }

  // Auto-resync: ARM on the reliable isConnected() false->true edge (KEEP this
  // trigger — a real wake proved isEncrypted() is NOT a reliable main-loop edge:
  // it was missed, resync never armed, and the restored card sat on "Syncing..."
  // forever even though the link was up). Scope-reduced: request ONLY the ACTIVE
  // scene's data (the one scene on glass), not all three card apps — the others
  // re-request their own data in onEnter when the user opens them, so pulling
  // them now is wasted work. An early send the phone ignores is simply retried
  // by the backstop below once the link matures.
  static bool lastConnForResync = false;
  constexpr uint8_t kResyncMaxRetries = 3;           // capped from 8: ~6s of maturation coverage
  constexpr uint32_t kResyncRetryIntervalMs = 2000;  // settle time between attempts
  static uint32_t resyncNextAtMs = 0;
  static uint32_t resyncBaselineRailRev = 0;

  // Flush of offline priority toggles. Deliberately NOT tied to the
  // connect edge: the bench proved an edge can be missed entirely (a second
  // phone holding the link means isConnected() never dips, and a send right
  // at a fresh edge can hit a not-yet-mature link and be dropped silently).
  // An edge-triggered flush that misses leaves the toggle stranded forever,
  // which is exactly the durability this fix promises. So instead: while
  // anything is pending AND the link is up, re-send on a slow timer, capped
  // so a phone that never confirms can't be spammed. The store's reconcile
  // clears each pending entry once the phone's snapshot agrees, which is
  // what actually ends the loop. Scene-independent — a toggle syncs even
  // after the user leaves the Priorities scene.
  constexpr uint8_t kPendingPushMaxAttempts = 6;
  constexpr uint32_t kPendingPushIntervalMs = 3000;
  static uint8_t pendingPushAttemptsLeft = 0;
  static uint32_t pendingPushNextAtMs = 0;
  static std::size_t lastPendingCount = 0;

  // New (or newly cleared) pending work re-arms the attempt budget, so each
  // offline toggle gets its own full set of tries.
  const std::size_t pendingNow = PRIORITIES_STORE.pendingCount();
  if (pendingNow != lastPendingCount) {
    lastPendingCount = pendingNow;
    pendingPushAttemptsLeft = pendingNow > 0 ? kPendingPushMaxAttempts : 0;
    pendingPushNextAtMs = millis();
  }
  if (pendingNow > 0 && connected && pendingPushAttemptsLeft > 0 &&
      static_cast<int32_t>(millis() - pendingPushNextAtMs) >= 0) {
    for (std::size_t i = 0; i < pendingNow; i++) {
      char pendId[65];
      bool pendDone = false;
      if (PRIORITIES_STORE.pendingGet(i, pendId, pendDone)) {
        COMPANION_BLE.sendPriorityToggle(pendId, pendDone);
      }
    }
    COMPANION_BLE.sendPrioritiesSyncRequest();  // ask for the confirming snapshot
    pendingPushAttemptsLeft--;
    pendingPushNextAtMs = millis() + kPendingPushIntervalMs;
  }

  if (connected != lastConnForResync) {
    lastConnForResync = connected;
    if (connected) {
      // Fresh link: arm the single active-scene request. Only card scenes have a
      // rail to wait on; for no-rail scenes (Notifications relies on the ANCS
      // backfill; Launcher/Settings/About have no store) skip the retry backstop
      // entirely. requestActive() is a no-op for those, so nothing is sent.
      CompanionSync::requestActive();
      if (activeSceneHasRail()) {
        gResyncRetriesLeft = kResyncMaxRetries;
        resyncNextAtMs = millis() + kResyncRetryIntervalMs;
        resyncBaselineRailRev = activeSceneRailRevision();
      } else {
        gResyncRetriesLeft = 0;
      }
    }
  }

  // Drive the armed request (fires once on the next tick, no blocking).
  CompanionSync::pump();

  // Retry backstop: after the request has gone out but the active scene's rail
  // still hasn't advanced (reply lost, or link not yet mature), RE-ARM, up to
  // kResyncMaxRetries times. Stops once the rail advances, the link drops, or
  // the budget is spent. Generalized from the old block-only blkRev check to
  // whichever card scene is on glass (activeSceneRailRevision()).
  if (gResyncRetriesLeft > 0 && !CompanionSync::inProgress() &&
      static_cast<int32_t>(millis() - resyncNextAtMs) >= 0) {
    if (activeSceneRailRevision() != resyncBaselineRailRev || !COMPANION_BLE.isConnected()) {
      gResyncRetriesLeft = 0;  // data arrived, or link dropped -> stop
    } else {
      CompanionSync::requestActive();  // re-arm the single active-scene request
      gResyncRetriesLeft--;
      resyncNextAtMs = millis() + kResyncRetryIntervalMs;
    }
  }

  static uint32_t lastStoreRevision = 0;
  const uint32_t storeRevision = NOTIFICATION_STORE.revision();
  if (storeRevision != lastStoreRevision) {
    lastStoreRevision = storeRevision;
    markNotificationsDirtyIfActive();  // list changed while it is visible
  }

  // A GetAppAttributes lookup resolved a bundle id to its iOS display name —
  // repaint the list so the row swaps "com.apple.MobileSMS" for "Messages".
  static uint32_t lastAppNameRevision = 0;
  const uint32_t appNameRevision = COMPANION_ANCS.getAppNameRevision();
  if (appNameRevision != lastAppNameRevision) {
    lastAppNameRevision = appNameRevision;
    markNotificationsDirtyIfActive();
  }

  // M3: Block's status card ("block-status", remaining-minutes countdown pushed
  // by the iPhone) repaints ONLY the Block scene, and only while it is on glass.
  // SCOPED to BLOCK_STATUS.revision() (the block-status-only rail) rather than
  // the generic getRevision(): a Priorities or Today card landing bumps
  // getRevision() too, and marking Block dirty on those caused a cross-scene
  // repaint storm (Block repainting for a card it doesn't show). Priorities and
  // Today each have their own store-revision pump below, so they still repaint
  // on their own data.
  static uint32_t lastBlockCardRevision = 0;
  const uint32_t blockCardRevision = BLOCK_STATUS.revision();
  if (blockCardRevision != lastBlockCardRevision) {
    lastBlockCardRevision = blockCardRevision;
    markBlockDirtyIfActive();
  }

  // M3.2: the dedicated priorities store — the service fills it for every
  // snapshot (even ones that land while another scene is on glass), so its
  // own revision is what repaints the list rows.
  static uint32_t lastPrioritiesRevision = 0;
  const uint32_t prioritiesRevision = PRIORITIES_STORE.revision();
  if (prioritiesRevision != lastPrioritiesRevision) {
    lastPrioritiesRevision = prioritiesRevision;
    markPrioritiesDirtyIfActive();
  }

  // M3: the dedicated today store — same discipline as priorities above (the
  // service fills it for every "today.snapshot" card, whichever scene is up).
  static uint32_t lastTodayRevision = 0;
  const uint32_t todayRevision = TODAY_STORE.revision();
  if (todayRevision != lastTodayRevision) {
    lastTodayRevision = todayRevision;
    markTodayDirtyIfActive();
  }

  // Workout — same discipline: the service fills the store for every
  // "workout.snapshot" card; local +/- bumps advance the revision too, and
  // both repaint the scene when it is on glass.
  static uint32_t lastWorkoutRevision = 0;
  const uint32_t workoutRevision = WORKOUT_STORE.revision();
  if (workoutRevision != lastWorkoutRevision) {
    lastWorkoutRevision = workoutRevision;
    markWorkoutDirtyIfActive();
  }

  // R2 Read — phone-initiated requests latched by the BLE service (main-loop
  // flags). Shelf: scan /books and notify the chunked listing (SD access on
  // this task, per the OS-wide rule). Transfer: enter/exit the File Transfer
  // scene; the scene owns the Wi-Fi lifecycle.
  if (COMPANION_BLE.consumeShelfRequest()) {
    COMPANION_BLE.sendReaderShelf();
  }
  if (COMPANION_BLE.consumeProgressRequest()) {
    COMPANION_BLE.sendReaderProgress();
  }
  if (COMPANION_BLE.consumeWifiKnownRequest()) {
    COMPANION_BLE.sendWifiKnown();
  }
  COMPANION_BLE.pumpReaderPlace();  // item 6 step 3: send our place once the link is back
  {
    // Item 6: a place pushed from a phone. Write the matching book's .pos so
    // the next open resumes there. The reader suspends BLE while open, so a
    // push never lands mid-read — a plain .pos write is the whole job.
    CompanionBleService::PlacePush pp;
    if (COMPANION_BLE.consumePlacePush(pp)) {
      char path[160];
      if (reader::FbpBook::findByKey(pp.key, path, sizeof(path))) {
        reader::FbpBook::savePos(path, static_cast<uint16_t>(pp.page),
                                 static_cast<uint16_t>(pp.pageCount));
        Serial.printf("[xphone-os] place: %s -> page %lu/%lu (%s)\n", pp.key,
                      static_cast<unsigned long>(pp.page),
                      static_cast<unsigned long>(pp.pageCount), path);
      } else {
        Serial.printf("[xphone-os] place: no book matches key '%s'\n", pp.key);
      }
    }
  }
  static uint32_t sNeedsWifiEchoAtMs = 0;
  if (sNeedsWifiEchoAtMs && millis() >= sNeedsWifiEchoAtMs) {
    sNeedsWifiEchoAtMs = 0;
    COMPANION_BLE.sendTransferStatus("needs-wifi");
  }
  switch (COMPANION_BLE.consumeTransferRequest()) {
    case CompanionBleService::TransferRequest::Start:
      // No saved network: answer over BLE from WHEREVER we are — launcher,
      // reader, anywhere. The screen is not a participant in this protocol;
      // entering it just to say "needs-wifi" is how a reader ended up
      // parked on a dead-end screen eating every later start request
      // (found live, 2026-08-23).
      if (WifiCreds::count() == 0) {
        Serial.println("[xphone-os] transfer: start requested, no Wi-Fi saved -> needs-wifi (no scene change)");
        COMPANION_BLE.sendTransferStatus("needs-wifi");
        // The first answer races the on-connect flood (shelf, progress,
        // wifi.known chunks share the pipe) and notify() cannot report a
        // drop — the pinned framework returns void. Echo once after the
        // flood drains. The phone ignores a duplicate.
        sNeedsWifiEchoAtMs = millis() + 1500;
        break;
      }
      Serial.println("[xphone-os] transfer: phone requested start");
      if (gCurrentSceneId != SceneId::FileTransfer) {
        showFileTransferAutoStart();
      } else {
        // Same card, scene already up: behave exactly like a fresh entry.
        fileTransferRestartFromCard(/*direct=*/false);
      }
      break;
    case CompanionBleService::TransferRequest::StartDirect:
      Serial.println("[xphone-os] transfer: phone requested DIRECT start");
      if (gCurrentSceneId != SceneId::FileTransfer) {
        showFileTransferAutoStartDirect();
      } else {
        fileTransferRestartFromCard(/*direct=*/true);
      }
      break;
    case CompanionBleService::TransferRequest::Stop:
      stopFileTransferIfActive();  // ack + restart; no-op on other scenes
      break;
    case CompanionBleService::TransferRequest::None:
      break;
  }

  // Status-bar sync dot: repaint the launcher on the sync active<->idle EDGE so
  // the dot appears when a wake resync/backfill starts and clears when it ends.
  // This is ACTIVITY-driven (fires only on the transition, at most twice per
  // wake), NOT a periodic timer — the X3 has composite partial refresh disabled,
  // so a periodic "blink" would flash the whole panel every tick. The dot lives
  // in the battery cluster, which today only the Launcher/About status bars
  // draw; a card scene on glass has no status bar, so nothing to repaint there.
  static bool lastSyncActive = false;
  const bool syncActive = xphoneSyncActive();
  if (syncActive != lastSyncActive) {
    lastSyncActive = syncActive;
    markLauncherDirtyIfActive();
  }
}

// Power button state machine (M4): two gestures, from any scene.
//   * HOLD past ~2.5s -> restart, fired WHILE STILL HELD (the SD self-update
//     only runs at boot, so a software restart path must always exist).
//   * Press-and-RELEASE under the hold threshold -> deep sleep. Sleep fires
//     on the release edge, never on press — otherwise a restart hold would
//     sleep first at +80ms. Sub-80ms blips (pocket brushes, contact bounce
//     beyond the SDK's 5ms debounce) are ignored.
// Debounce + held-time come from the SDK (InputManager::update() debounces
// the GPIO3 pin; getPowerButtonHeldTime() is its press clock — live while
// pressed, and the LAST completed press duration after release,
// InputManager.cpp:307-313 — the same pair x4-os main.cpp uses for
// hold-to-sleep). Never returns on either trigger.
static void checkPowerButton() {
  constexpr unsigned long kRestartHoldMs = 2500;
  constexpr unsigned long kSleepMinPressMs = 80;

  static bool wasDown = false;
  const bool down = input.powerPressed();

  if (down && input.powerHeldMs() >= kRestartHoldMs) {
    Serial.printf("[xphone-os] power held %lums; restarting\n", input.powerHeldMs());
    SCENES.waitFlushIdle();  // panel must be idle before this direct paint
    input.suspendTask();
    gfx.clear();
    gfx.drawTextCentered(kFontRegular, gfx.width() / 2, gfx.height() / 2, "Restarting...");
    gfx.flush(EInkDisplay::FULL_REFRESH);
    esp_restart();
  }

  if (!down && wasDown) {  // release edge, under the restart threshold
    wasDown = false;
    const unsigned long held = input.powerHeldMs();  // duration of the press that just ended
    if (held >= kSleepMinPressMs) {
      Serial.printf("[xphone-os] power pressed %lums; sleeping\n", held);
      Sleep::sleepNow(gfx, input);  // never returns
    }
    Serial.printf("[xphone-os] power blip %lums ignored\n", held);
    return;
  }
  wasDown = down;
}

// M4 device auto-sleep: XP_AUTO_SLEEP_MS (Sleep.h; default 10 min, 0
// disables) with no button input -> the same Sleep::sleepNow() as a short
// power press. Idle clock is rollover-safe unsigned millis subtraction,
// reset by any ladder-button press edge (raw SDK edge, pre-tap-filter) or
// the power button being down.
//
// M4.2 two-timeout model: an ACTIVE Block session used to PIN the device awake
// (main.cpp shoved the deadline forward every tick), so the panel/BLE/CPU ran
// for the whole ~2h session and drained the 650mAh cell — while the iPhone
// (Screen Time) was enforcing the block independently, so the device staying
// awake bought nothing. Instead, an active block now uses the SHORTER
// XP_AUTO_SLEEP_BLOCK_MS idle window (default 2 min); the block keeps running
// on the phone after the device sleeps. Block state comes from the durable
// BLOCK_STATUS store (getCard()'s single slot is clobbered by priorities/today
// pushes; the store is not).
//   * SD flash — needs no check here: both sd_update paths (boot
//     checkAndApply, Settings-scene flashFromPath) run synchronously inside
//     a single loop tick, so this function can never interleave with one.
// Bench-hold USB host detection (see checkAutoSleep). A host issues SOF
// frames ~every 1 ms; the C3's USB Serial/JTAG peripheral counts them even
// when no program has the port open. If the counter moved in the last
// second, a computer is on the line.
static bool gUsbHoldLogged = false;
static bool usbHostConnected() {
  static uint32_t lastFrame = 0;
  static uint32_t lastSampleMs = 0;
  static bool connected = false;
  const uint32_t now = millis();
  if (now - lastSampleMs >= 1000) {
    const uint32_t frame = USB_SERIAL_JTAG.fram_num.sof_frame_index;
    connected = (frame != lastFrame);
    lastFrame = frame;
    lastSampleMs = now;
  }
  return connected;
}

static void checkAutoSleep() {
#if XP_AUTO_SLEEP_MS > 0
  static unsigned long lastInputMs = 0;
  const unsigned long now = millis();
  if (input.wasAnyPressed() || input.powerPressed()) lastInputMs = now;

  // R2: never auto-sleep out of a File Transfer session — a multi-MB book
  // upload has no button presses, and deep-sleeping mid-request tears the
  // TCP connection under the phone. The scene pins the deadline instead of
  // being exempted outright so the normal window resumes the moment it exits.
  if (gCurrentSceneId == SceneId::FileTransfer) lastInputMs = now;

  // Bench hold: while a USB HOST is attached, never auto-sleep. A computer
  // polls the bus every millisecond, which advances the hardware USB frame
  // counter; a wall charger never polls, so battery/charger users keep the
  // normal windows. Sampled once per second; unplugging restarts the idle
  // window from that moment.
  if (usbHostConnected()) {
    lastInputMs = now;
    if (!gUsbHoldLogged) {
      Serial.println("[xphone-os] auto-sleep held: USB host attached");
      gUsbHoldLogged = true;
    }
  } else {
    gUsbHoldLogged = false;
  }

  const bool blockActive = BLOCK_STATUS.active();
  const unsigned long timeout =
      blockActive ? static_cast<unsigned long>(XP_AUTO_SLEEP_BLOCK_MS) : static_cast<unsigned long>(XP_AUTO_SLEEP_MS);
  if (now - lastInputMs < timeout) return;  // rollover-safe unsigned subtraction

  Serial.printf("[xphone-os] idle %lu min (%s window); auto-sleeping\n", (now - lastInputMs) / 60000UL,
                blockActive ? "block" : "normal");
  Sleep::sleepNow(gfx, input);  // never returns
#endif
}

// M2.1d stack/heap/queue audit line, once per 60 s. Baseline evidence before
// any future stack shrink: the only tasks are the framework Arduino loopTask
// (8192) and nimble_host (5120, frozen in the prebuilt core) — nothing to
// resize yet without hardware HWM data, which is exactly what this prints.
// uxTaskGetStackHighWaterMark returns StackType_t units; ESP-IDF defines
// StackType_t as uint8_t, so the value is BYTES on this port.
static void reportRuntimeStats() {
  static uint32_t lastReportMs = 0;
  const uint32_t now = millis();
  if (lastReportMs != 0 && now - lastReportMs < 60000UL) return;
  lastReportMs = now;

  const UBaseType_t loopHwm = uxTaskGetStackHighWaterMark(nullptr);
  const TaskHandle_t bleTask = COMPANION_ANCS.getHostTaskHandle();
  const UBaseType_t bleHwm = bleTask ? uxTaskGetStackHighWaterMark(bleTask) : 0;
  // Fragmentation health: `largest` is the biggest single allocation the heap
  // can satisfy right now; frag% = how much of the free total is unreachable
  // as one block (0 = one contiguous plain). A rising frag% at a stable
  // heapFree is the leak-shrapnel signature that pinned the reader's window.
  const uint32_t heapFree = ESP.getFreeHeap();
  const uint32_t largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  const unsigned fragPct = heapFree ? static_cast<unsigned>(100u - (largest * 100u) / heapFree) : 0;
  const char* mode = gCurrentSceneId == SceneId::Reader        ? "reader"
                     : gCurrentSceneId == SceneId::FileTransfer ? "transfer"
                                                                : "connected";
  Serial.printf("[xphone-os] stats: mode=%s loopHWM=%u B bleHWM=%s%u B heapFree=%u largest=%u frag=%u%% minFree=%u "
                "ancsQpeak=%u drops=%lu\n",
                mode, static_cast<unsigned>(loopHwm), bleTask ? "" : "n/a ", static_cast<unsigned>(bleHwm), heapFree,
                largest, fragPct, static_cast<unsigned>(esp_get_minimum_free_heap_size()),
                COMPANION_ANCS.getQueueHighWater(),
                static_cast<unsigned long>(COMPANION_ANCS.getQueueDropCount()));
}

// Bench dev console: while a USB host is attached, one-line serial commands
// inject synthetic button presses, so the bench drives the UI without hands.
//   btn <name>        — tap (name: up/down/left/right/prev/next/confirm/
//                       open/size/back/books)
//   btn <name> long   — long-press ("hold" also accepted); with taps this
//                       covers the entire input vocabulary, since scenes only
//                       consume the two one-shot edges (long-back = home,
//                       long-confirm = notification actions, ...)
//   reboot            — the power-button 2.5s-hold restart, minus the button
//   sleep             — REFUSED, loudly: deep sleep powers the USB port off
//                       and nothing can wake the device remotely; the bench
//                       must never be able to saw off the branch it sits on
// Unknown input is ignored silently (boot noise, other tools on the port).
static void pumpDevConsole() {
  if (!usbHostConnected()) return;
  static char line[32];
  static uint8_t len = 0;
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    if (c != '\n' && c != '\r') {
      if (len < sizeof(line) - 1) line[len++] = c;
      continue;
    }
    line[len] = 0;
    const uint8_t had = len;
    len = 0;
    if (had == 0) continue;
    // Bench: set the device clock without the phone. The hardware has no
    // RTC and the date arrives over BLE, so every date-dependent screen
    // (streaks, the month strip, "today") was unverifiable on the bench
    // whenever the app was not connected. Usage: clock 20260816 845
    if (!strncmp(line, "clock ", 6)) {
      unsigned long ymd = 0;
      unsigned long minutes = 0;
      if (sscanf(line + 6, "%lu %lu", &ymd, &minutes) == 2 && ymd > 19000000UL &&
          ymd < 30000000UL && minutes < 1440UL) {
        CLOCK_STORE.day = static_cast<uint32_t>(ymd);
        CLOCK_STORE.minutesIntoDay = static_cast<uint16_t>(minutes);
        CLOCK_STORE.firstSyncMs = millis();
        if (CLOCK_STORE.firstConnectMs == 0) CLOCK_STORE.firstConnectMs = millis();
        Serial.printf("[xphone-os] devcon: clock set day=%lu min=%lu\n", ymd, minutes);
        if (SCENES.active()) SCENES.active()->markDirty();
      } else {
        Serial.println("[xphone-os] devcon: clock needs <yyyymmdd> <minutes-into-day>");
      }
      continue;
    }
    if (!strcmp(line, "progress")) {
      // Send reading progress + stats over BLE now, without waiting for the
      // phone to ask. Lets the bench prove the chunking and the payload before
      // either app knows how to request it.
      COMPANION_BLE.sendReaderProgress();
      Serial.println("[xphone-os] devcon: progress sent");
      return;
    }
    if (!strcmp(line, "shelfdump")) {
      readerShelfDump();
      continue;
    }
    if (!strcmp(line, "failnote")) {
      // Bench-only: plant a transfer failure exactly as the radio-down path
      // does, so the carry-across-reboot announcement can be proven without
      // poisoning real credentials. Follow with "reboot", then watch for
      // "Transfer status sent state=failed" on the next encrypted connect.
      Preferences pf;
      if (pf.begin("transfer", /*readOnly=*/false)) {
        pf.putString("lastFail", "Bench-injected failure");
        pf.end();
        Serial.println("[xphone-os] devcon: failure note planted");
      }
      continue;
    }
    if (!strncmp(line, "placepush ", 10)) {
      // Bench: placepush <key> <page> <count> — inject a place through the
      // same latch the reader.place card uses, proving write+resume without
      // the app's sender (step 4).
      char key[64] = {0};
      unsigned page = 0, count = 0;
      if (sscanf(line + 10, "%63s %u %u", key, &page, &count) >= 2) {
        COMPANION_BLE.benchInjectPlace(key, page, count);
        Serial.printf("[xphone-os] devcon: place injected key=%s page=%u count=%u\n",
                      key, page, count);
      } else {
        Serial.println("[xphone-os] devcon: usage: placepush <key> <page> <count>");
      }
      continue;
    }
    if (!strcmp(line, "wificlear")) {
      // Bench-only: forget every saved network, bonds untouched. Exists to
      // reproduce the fresh-device "needs-wifi" path without an NVS erase,
      // which would also take the pairing keys and start THAT saga again.
      WifiCreds::clear();
      Serial.println("[xphone-os] devcon: wifi credentials cleared");
      continue;
    }
    if (!strcmp(line, "where")) {
      // Fact-based remote navigation: what scene is on glass, and where
      // the launcher selection sits (a wrong mental model of the grid
      // once started a real Block session from the bench).
      static constexpr const char* kSceneNames[] = {
          "launcher", "notifications", "settings",  "block",   "priorities",
          "today",    "about",         "reader",    "workout", "transfer"};
      const uint32_t id = static_cast<uint32_t>(gCurrentSceneId);
      char detail[96] = {0};
      if (gCurrentSceneId == SceneId::Reader) readerWhere(detail, sizeof(detail));
      Serial.printf("[xphone-os] devcon: where scene=%s launcherSel=%d%s%s\n",
                    id < sizeof(kSceneNames) / sizeof(kSceneNames[0]) ? kSceneNames[id] : "?",
                    launcherSelection(), detail[0] ? " reader=" : "", detail);
      continue;
    }
    if (!strcmp(line, "fb")) {
      // glass-twin: the exact pixels the firmware believes are on glass.
      // Wait for the flush worker first — mid-flush the framebuffer is a
      // truthful frame, but the panel is not showing it yet, and a camera
      // diff taken against it would report a false mismatch.
      SCENES.waitFlushIdle();
      bench::dumpFrameBuffer(gfx);
      continue;
    }
    if (!strncmp(line, "cal", 3) && (line[3] == 0 || line[3] == ' ')) {
      // Camera calibration target. Drawn straight into the framebuffer,
      // outside the scene manager, so it is not tied to any scene's layout.
      // `redraw` puts the real UI back.
      const char* name = (line[3] == ' ') ? line + 4 : "frame";
      SCENES.waitFlushIdle();
      if (!bench::drawCalPattern(gfx, name)) {
        Serial.printf("[xphone-os] devcon: cal unknown pattern '%s' "
                      "(frame|grid|checker|selftest|proof|bars|white|black)\n",
                      name);
        // The framebuffer is untouched on an unknown name, so nothing to undo.
        continue;
      }
      gfx.flush(EInkDisplay::FULL_REFRESH);
      Serial.printf("[xphone-os] devcon: cal %s (%dx%d) — run 'redraw' to restore the UI\n", name, gfx.width(),
                    gfx.height());
      continue;
    }
    if (!strncmp(line, "text ", 5)) {
      // Bench-only: draw arbitrary UTF-8 in all three UI fonts, straight into
      // the framebuffer (like `cal`), so glass-twin can grab an exact proof of
      // glyph coverage — added for the Cyrillic/Greek extension (flowe-os#3).
      SCENES.waitFlushIdle();
      gfx.clear();
      const int cx = gfx.width() / 2;
      const int cy = gfx.height() / 2;
      gfx.drawTextCentered(kFontBold, cx, cy - 60, line + 5);
      gfx.drawTextCentered(kFontRegular, cx, cy, line + 5);
      gfx.drawTextCentered(kFontSmall, cx, cy + 60, line + 5);
      gfx.flush(EInkDisplay::FULL_REFRESH);
      Serial.println("[xphone-os] devcon: text — run 'redraw' to restore the UI");
      continue;
    }
    if (!strcmp(line, "redraw")) {
      // Force a clean full repaint of the live scene (after `cal`, or to make
      // a capture reproducible without rebooting). The white FULL flush first
      // is what makes it clean: panel RAM holds the calibration pattern, and
      // the scene's own repaint is only a FAST differential against it.
      SCENES.waitFlushIdle();
      gfx.clear();
      gfx.flush(EInkDisplay::FULL_REFRESH);
      if (Scene* s = SCENES.active()) s->markDirty();
      Serial.println("[xphone-os] devcon: redraw");
      continue;
    }
    if (!strcmp(line, "sleep")) {
      Serial.println("[xphone-os] devcon: sleep REFUSED (USB host attached; deep sleep would drop the link for good)");
      continue;
    }
    if (!strcmp(line, "direct")) {
      // Bench trigger for W2 Direct mode (normally BLE "transfer.direct").
      Serial.println("[xphone-os] devcon: direct");
      showFileTransferAutoStartDirect();
      continue;
    }
    if (!strcmp(line, "sta")) {
      // Bench trigger for a normal Wi-Fi session (normally BLE
      // "transfer.start"): joins the saved network, serves HTTP with no
      // session token, so the bench Mac can curl it. (2026-08-18)
      Serial.println("[xphone-os] devcon: sta");
      showFileTransferAutoStart();
      continue;
    }
    if (!strcmp(line, "sdtest")) {
      // The card speed test behind the web console's "Test the card"
      // button. Twenty minutes for six books, and a Read screen that stops
      // answering buttons, both smell like a slow or failing card
      // (oky_doodle, 2026-08-29) — this turns the smell into a number the
      // owner can read us over chat. Reads the largest book for up to 3 s;
      // writes nothing.
      Serial.println("[xphone-os] devcon: sdtest — timing card reads");
      if (!SdMan.ready() && !SdMan.begin()) {
        Serial.println("[xphone-os] devcon: sdtest: no card");
        continue;
      }
      char pick[160] = {0};
      uint64_t pickSize = 0;
      FsFile dir = SdMan.open("/books", O_RDONLY);
      if (dir && dir.isDir()) {
        FsFile f;
        char nm[128];
        while (f.openNext(&dir, O_RDONLY)) {
          if (!f.isDir() && f.fileSize() > pickSize && f.getName(nm, sizeof(nm)) > 0 &&
              nm[0] != '.') {
            pickSize = f.fileSize();
            snprintf(pick, sizeof(pick), "/books/%s", nm);
          }
          f.close();
        }
      }
      if (dir) dir.close();
      if (!pick[0]) {
        Serial.println("[xphone-os] devcon: sdtest: /books has no files to read");
        continue;
      }
      FsFile f = SdMan.open(pick, O_RDONLY);
      if (!f) {
        Serial.printf("[xphone-os] devcon: sdtest: cannot open %s\n", pick);
        continue;
      }
      static uint8_t buf[2048];  // BSS, not this task's tight stack
      const uint32_t t0 = millis();
      uint32_t total = 0;
      // Deliberate work, not a hang: keep the stall watcher fed, or a slow
      // card would file this very test as a freeze.
      while (millis() - t0 < 3000) {
        const int got = f.read(buf, sizeof(buf));
        if (got <= 0) break;
        total += static_cast<uint32_t>(got);
        stallwatch::beat();
      }
      const uint32_t ms = millis() - t0;
      f.close();
      Serial.printf("[xphone-os] devcon: sdtest %s: read %lu KB in %lu ms = %lu KB/s "
                    "(heap %lu, worst stall this run %lu ms)\n",
                    pick, static_cast<unsigned long>(total / 1024),
                    static_cast<unsigned long>(ms),
                    static_cast<unsigned long>(ms ? (total / 1024) * 1000UL / ms : 0),
                    static_cast<unsigned long>(ESP.getFreeHeap()),
                    static_cast<unsigned long>(stallwatch::worstStallMs()));
      continue;
    }
    if (!strncmp(line, "stall ", 6)) {
      // Bench-only: block the loop on purpose, so the stall watcher can be
      // proven to catch a freeze rather than assumed to. Nothing else can
      // produce one on demand — a real freeze needs a failing SD card.
      const unsigned long ms = strtoul(line + 6, nullptr, 10);
      Serial.printf("[xphone-os] devcon: stalling the loop for %lums\n", ms);
      xpTrace("devcon: deliberate stall");
      const uint32_t until = millis() + static_cast<uint32_t>(ms);
      while (static_cast<int32_t>(millis() - until) < 0) {
      }
      Serial.printf("[xphone-os] devcon: stall over, worst this run %lums\n",
                    static_cast<unsigned long>(stallwatch::worstStallMs()));
      continue;
    }
    if (!strcmp(line, "reboot")) {
      Serial.println("[xphone-os] devcon: reboot");
      SCENES.waitFlushIdle();  // same teardown as the power-hold restart
      input.suspendTask();
      gfx.clear();
      gfx.drawTextCentered(kFontRegular, gfx.width() / 2, gfx.height() / 2, "Restarting...");
      gfx.flush(EInkDisplay::FULL_REFRESH);
      esp_restart();
    }
    if (had <= 4 || strncmp(line, "btn ", 4) != 0) continue;
    char* n = line + 4;
    bool lng = false;
    uint32_t holdMs = 0;
    if (char* sp = strchr(n, ' ')) {  // optional modifier after the name
      *sp = 0;
      const char* mod = sp + 1;
      // "long"/"hold" = the one-shot long-press EDGE. "down <ms>" = the button
      // physically held for that long, which is the only way to exercise
      // level-driven behaviour like the chapter list's hold-to-jump.
      if (strcmp(mod, "long") == 0 || strcmp(mod, "hold") == 0) lng = true;
      else if (strncmp(mod, "down", 4) == 0) holdMs = strtoul(mod + 4, nullptr, 10);
      else continue;
      if (holdMs == 0 && !lng) continue;
    }
    Btn b;
    if (!strcmp(n, "up")) b = Btn::Up;
    else if (!strcmp(n, "down")) b = Btn::Down;
    else if (!strcmp(n, "left") || !strcmp(n, "prev")) b = Btn::Left;
    else if (!strcmp(n, "right") || !strcmp(n, "next")) b = Btn::Right;
    else if (!strcmp(n, "confirm") || !strcmp(n, "open") || !strcmp(n, "size")) b = Btn::Confirm;
    else if (!strcmp(n, "back") || !strcmp(n, "books")) b = Btn::Back;
    else continue;
    if (holdMs) input.injectHold(b, holdMs);
    else if (lng) input.injectLong(b);
    else input.injectTap(b);
    if (holdMs) Serial.printf("[xphone-os] devcon: btn %s held %lums\n", n, (unsigned long)holdMs);
    else Serial.printf("[xphone-os] devcon: btn %s%s\n", n, lng ? " long" : "");
  }
}

void loop() {
  stallwatch::beat();       // "the loop is alive"; a stuck loop stops ticking
  pumpDevConsole();         // bench-only: serial "btn X" -> synthetic taps
  input.update();           // debounced button edges (SDK InputManager)
  checkPowerButton();       // press+release -> deep sleep; hold ~2.5s -> restart
  checkAutoSleep();         // M4: idle -> deep sleep (2 min window while a block is active)
  pumpCompanionEvents();         // BLE/ANCS: parse queued payloads, set dirty flags
  COMPANION_BLE.tickAdvPolicy();  // M2.1b: fast->slow advertising demotion
  SCENES.loop(input, gfx);       // handle input; repaint only when a scene is dirty
  reportRuntimeStats();          // M2.1d: 60s stack/heap/ANCS-queue audit line
  delay(10);                     // 10ms poll cadence — no periodic redraws
}
