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
#include "StackProbe.h"
#include <esp_task_wdt.h>
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
#include "BenchFramebufferLoan.h"
#include "TransferSync.h"
#include "BenchSdWrite.h"
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
#include "BackgroundImage.h"
#include "Sleep.h"
#include "ble/CompanionAncsClient.h"
#include "ble/CompanionBleService.h"
#include "reader/FbpBook.h"
#if defined(FLOWE_BENCH_COMPACT)
#include <memory>
#include <new>
#endif
#include "art/FloweLogo.h"
#include "esp_heap_caps.h"
#include "TransferMemoryProbe.h"
#include "esp_system.h"
#if CONFIG_PM_ENABLE
// DFS experiment (custom libs only): the stock prebuilt libraries compile
// power management OUT, so this header and the dev command below exist only
// on the x3sleep package with CONFIG_PM_ENABLE baked in.
#include "esp_pm.h"
#include <driver/gpio.h>
#if XP_LIGHT_SLEEP_LIBS
static void lightSleepPrepare();  // defined with the P4 policy block below
bool gLsActive = false;           // live light-sleep state (P4 policy block below)
#endif
#endif
#include "soc/usb_serial_jtag_struct.h"
// Used in every build (the USB-host hold and the bench link drop), not only
// with the light-sleep libraries; the stock `x3` build of the public tree
// failed on these two (2026-09-06).
static bool usbHostConnected();
static uint32_t gBleDropAtMs = 0;  // bench: scheduled link drop (0 = none)
#include "net/FileTransferServer.h"
#include "scenes/AppScenes.h"
#include "AppsManager.h"
#include "scenes/HomeScene.h"

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

// Faster wake (2026-09-05, Andrew's ask): a wake from deep sleep skips the
// splash. The splash exists so the panel's first conditioning flash after a
// cold power-on shows the wordmark instead of a bare black blink; on a wake
// the glass already holds the sleep poster, so the one FULL refresh that
// paints the restored screen is the only flash the user sees. Measured on
// the X3: the splash stage is 1.79 s, gone. Bench: `wakeboot` leaves a
// one-shot NVS flag and restarts, so a wake can be reproduced on the cable
// without the power button (RTC memory did not survive esp_restart here).
static bool takeBenchQuietWake() {
  Preferences pref;
  if (!pref.begin("bench", /*readOnly=*/false)) return false;
  const bool set = pref.getUChar("quietwake", 0) != 0;
  if (set) pref.remove("quietwake");
  pref.end();
  return set;
}
static void armBenchQuietWake() {
  Preferences pref;
  if (pref.begin("bench", /*readOnly=*/false)) { pref.putUChar("quietwake", 1); pref.end(); }
}

// A scene's way to start over with a clean heap: restart as a wake (no
// splash, glass left as it is) and land back on `sceneId`. First user: the
// reader, when a Wi-Fi session's residue leaves no room for a page buffer
// (2026-09-06, Project Hail Mary on the X3 after a sync).
void quietRestartToScene(uint32_t sceneId) {
  Serial.printf("[xphone-os] quiet restart to scene %lu (free=%u largest=%u)\n",
                static_cast<unsigned long>(sceneId), ESP.getFreeHeap(),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  Sleep::armRestoreScene(sceneId);
  Sleep::persistStoresForRestart();  // the boot re-seeds THESE lists, not the last power-off's
  SCENES.waitFlushIdle();
  input.suspendTask();
  armBenchQuietWake();
  esp_restart();
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
  Serial.setRxBufferSize(4096);  // bench file-put batches several 250-char lines per loop tick (default 256)
  Serial.begin(115200);
  Serial.setTxTimeoutMs(1);
  const unsigned long tSerial = millis();

  // Stage 2: device fingerprint, then display. One binary serves X3 and X4
  // (field brick: X4 bin on an X3 drives SSD1677 protocol at UC8253 glass —
  // frozen panel, "dead" buttons). selectXteinkDevice() probes the X3-only
  // I2C parts (gauge/RTC/IMU) and sets BoardConfig::ACTIVE; it must run
  // BEFORE SD + display bring-up (XteinkDetect.h contract).
  // Reset-reason history (last 8, newest last) in NVS, printed every boot.
  // The bench logger resets the chip whenever the USB port re-enumerates,
  // which hides the reason for any reboot that happened unplugged.
  {
    static const char* const kRstNames[] = {"unknown", "poweron", "ext",   "sw",     "panic",   "int_wdt",
                                            "task_wdt", "wdt",    "deepsleep", "brownout", "sdio", "usb",
                                            "jtag",    "efuse",  "pwr_glitch", "cpu_lockup"};
    const int r = static_cast<int>(esp_reset_reason());
    const char* name = (r >= 0 && r < 16) ? kRstNames[r] : "?";
    Preferences prefs;
    if (prefs.begin("xphone", /*readOnly=*/false)) {
      String hist = prefs.getString("rstlog", "");
      hist += name;
      hist += ",";
      while (hist.length() > 96) hist = hist.substring(hist.indexOf(',') + 1);
      prefs.putString("rstlog", hist);
      prefs.end();
      Serial.printf("[xphone-os] reset history (oldest..newest): %s\n", hist.c_str());
      {
        Preferences pref;  // the USB re-plug marker (usbReplug), consumed here
        if (pref.begin("bench", /*readOnly=*/false)) {
          if (pref.isKey("replugMs")) {
            Serial.printf("[xphone-os] usb: the previous run re-plugged itself at uptime %lu s\n",
                          static_cast<unsigned long>(pref.getUInt("replugMs", 0) / 1000UL));
            pref.remove("replugMs");
          }
          pref.end();
        }
      }
    }
  }
  gDeviceIsX3 = freeink::selectXteinkDevice();
  if (gDeviceIsX3) Sleep::imuSleepAtBoot();  // P1.2: the unused motion sensor sleeps from boot
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
    if (!FileTransferServer::recoverUploads())
      Serial.println("[xphone-os] upload recovery pending; prior bytes retained");
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
  const bool quietWake = esp_reset_reason() == ESP_RST_DEEPSLEEP || takeBenchQuietWake();
  {
    Preferences p;  // bench A/B: `bootfull on` keeps the 3.9 s FULL as the first clear on the X4
    if (p.begin("bench", true)) {
      if (p.getUChar("bootfull", 0)) display.setFirstRefreshFull(true);
      p.end();
    }
  }
  if (gfxOk && !quietWake) drawBootSplash(gfx);
  const unsigned long tSplash = millis();
  bootTrace(!gfxOk ? "gfx: NO FRAMEBUFFER" : quietWake ? "gfx: splash skipped (wake)" : "gfx: splash drawn");

  // Stage 2.5: SD firmware self-update — MUST stay this early in boot. Mounts
  // the SD card and, if /update.bin exists at the root, flashes it into the
  // inactive OTA slot and restarts (never returns). Any failure logs + draws
  // an X and falls through so the device is never stranded.
  sd_update::bootRollbackCheck();  // Phase 4 safety net: revert a build that never painted
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
    if (id == SceneId::Launcher && homeLayout() == HomeLayout::Widget) {
      showHome();
    } else {
      showSceneById(id);
    }
  } else {
    if (homeLayout() == HomeLayout::Widget) {
      showHome();
    } else {
      showLauncher();  // gWakeRestoreScene stays "none"
    }
  }
  SCENES.renderIfDirty(gfx);  // first paint (FULL refresh)
  sd_update::confirmBoot();   // reached the first paint: this build is good
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
    {
      Preferences p;  // a sync ended with a restart: tell the phone it stopped
      if (p.begin("xfer", /*readOnly=*/false)) {
        if (p.getUChar("stopped", 0)) {
          p.remove("stopped");
          COMPANION_BLE.queueTransferStatus("stopped", "restart");
          Serial.println("[xphone-os] transfer: 'stopped' queued for the phone after the restart");
        }
        p.end();
      }
    }
    COMPANION_ANCS.begin();
    COMPANION_ANCS.requestPairing();
    Serial.printf("[xphone-os] BLE companion + ANCS armed (%lu ms after boot)\n", millis() - tBoot);
  }

  // Stage 6 (M2.1b): drop the CPU to XP_CPU_MHZ. Placed AFTER BLE begin() so
  // the whole radio bring-up runs at the boot clock and the stack never
  // initializes across a frequency change (80 MHz is fully BLE-capable on
  // the C3, this ordering is just belt-and-braces). Skipped entirely at 160
  // so -DXP_CPU_MHZ=160 is a true no-op A/B switch.
#if CONFIG_PM_ENABLE
  // P1.7 (efficiency test plan 2026-09-02): dynamic frequency scaling is the
  // default on the PM-enabled packages. Idle at 40 MHz, 80 for radio events
  // (the controller's APB lock), 160 only while a CpuBoost is held. Measured
  // on the bench as `dfs 40 80`: 23.0 -> 17.0 mA reading. `dfs` still
  // re-tunes it at runtime; `dfs off` pins 80/80. No light sleep here — that
  // is the x3ls experiment with its own guards.
  {
    esp_pm_config_t cfg = {};
    cfg.max_freq_mhz = 160;
    cfg.min_freq_mhz = 40;
    cfg.light_sleep_enable = false;
    const esp_err_t rc = esp_pm_configure(&cfg);
    Serial.printf("[xphone-os] cpu: dfs max=160 min=40 rc=%d\n", static_cast<int>(rc));
  }
#if XP_LIGHT_SLEEP_LIBS
  lightSleepPrepare();  // policy ticks in loop(); starts off until the USB check ran
#endif
#elif XP_CPU_MHZ != 160
  const bool cpuOk = setCpuFrequencyMhz(XP_CPU_MHZ);
  Serial.printf("[xphone-os] cpu: setCpuFrequencyMhz(%d) %s, now %lu MHz\n", XP_CPU_MHZ,
                cpuOk ? "ok" : "FAILED", static_cast<unsigned long>(getCpuFrequencyMhz()));
#endif
  // Loop watchdog (2026-09-07): the X3 froze silently at a phone's first
  // request with 20 KB free, on "Syncing..." for ten minutes, USB alive,
  // console dead. With the loop task on the task watchdog a freeze becomes
  // a reset with a backtrace naming the frame. 60 s: longer than any honest
  // single step in the loop; uploads feed it per chunk (FileTransferServer).
  {
    esp_task_wdt_config_t wdt = {};
    wdt.timeout_ms = 60000;
    wdt.idle_core_mask = 0;
    wdt.trigger_panic = true;
    const esp_err_t rc = esp_task_wdt_reconfigure(&wdt);
    enableLoopWDT();
    Serial.printf("[xphone-os] loop watchdog: 60 s, panic on trip (reconfigure rc=%d)\n", static_cast<int>(rc));
  }
}

int8_t gWifiTxPowerQuarterDb = 0;  // bench: max Wi-Fi TX power in 0.25 dBm units, 0 = driver default

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
    markHomeDirtyIfActive();
  }

  // M3.2: the dedicated priorities store — the service fills it for every
  // snapshot (even ones that land while another scene is on glass), so its
  // own revision is what repaints the list rows.
  static uint32_t lastPrioritiesRevision = 0;
  const uint32_t prioritiesRevision = PRIORITIES_STORE.revision();
  if (prioritiesRevision != lastPrioritiesRevision) {
    lastPrioritiesRevision = prioritiesRevision;
    markPrioritiesDirtyIfActive();
    markHomeDirtyIfActive();
  }

  // M3: the dedicated today store — same discipline as priorities above (the
  // service fills it for every "today.snapshot" card, whichever scene is up).
  static uint32_t lastTodayRevision = 0;
  const uint32_t todayRevision = TODAY_STORE.revision();
  if (todayRevision != lastTodayRevision) {
    lastTodayRevision = todayRevision;
    markTodayDirtyIfActive();
    markHomeDirtyIfActive();
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
  // X1 reader.goto consumer — the scene rules live here (proposal v1):
  // Reader scene handles it (live jump or on-glass confirm); the launcher
  // opens the reader INTO the confirm; any other scene drops it with a log
  // (the phone times out on the missing reader.pos echo).
  {
    CompanionBleService::GotoPush gp;
    if (COMPANION_BLE.consumeGotoPush(gp)) {
      if (gCurrentSceneId == SceneId::Reader) {
        readerAcceptGoto(gp.key, gp.cid);
      } else if (gCurrentSceneId == SceneId::Launcher) {
        showReader();
        readerAcceptGoto(gp.key, gp.cid);
      } else {
        Serial.printf("[xphone-os] goto: dropped (scene busy) key=%s\n", gp.key);
      }
    }
  }
  if (apps_mgr::inventoryRequestedAndClear()) {
    COMPANION_BLE.sendAppsInventory();
  }
  if (apps_mgr::infoRequestedAndClear()) {
    COMPANION_BLE.sendDeviceInfo();
  }
  COMPANION_BLE.pumpReaderPlace();  // item 6 step 3: send our place once the link is back
  COMPANION_BLE.pumpReaderPos();    // F1: the live stream (1/sec brake, 5-min heartbeat)
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
        showFileTransferAutoStartInPlace(/*direct=*/false);  // sync in place: no page jump
      } else {
        // Same card, scene already up: behave exactly like a fresh entry.
        fileTransferRestartFromCard(/*direct=*/false);
      }
      break;
    case CompanionBleService::TransferRequest::StartDirect:
      Serial.println("[xphone-os] transfer: phone requested DIRECT start");
      if (gCurrentSceneId != SceneId::FileTransfer) {
        showFileTransferAutoStartInPlace(/*direct=*/true);
      } else {
        fileTransferRestartFromCard(/*direct=*/true);
      }
      break;
    case CompanionBleService::TransferRequest::Stop:
      stopFileTransferIfActive();  // ends the parked screen; no-op on other scenes
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
// ---------------------------------------------------------------------------
// The nap (Andrew, 2026-09-05): the sleep screen is on the glass, the chip
// naps between radio events, Bluetooth stays connected, and any press brings
// the last screen straight back with one FULL refresh: no boot, no logo. It
// replaces "off" as the first idle stage and as the short power press. Off
// (deep sleep) is the second stage, and the long press.
// ---------------------------------------------------------------------------
Input* Input::sInstance = nullptr;

static bool gNapping = false;
static bool gSleepPosterPreview = false;  // bench-only awake preview, never enters either sleep state
static bool gNapOnUsb = false;  // a USB host was attached when the nap began: keep the link, no light sleep
static SceneId gNapScene = SceneId::Launcher;
static uint32_t gNapAfterMsOverride = 0;  // bench: napafter <s>
static uint32_t gOffAfterMsOverride = 0;  // bench: offafter <s>
// Live nap poster: the store revisions seen when the nap began (or last
// redrawn), the debounce clock for a burst of cards, and how many FAST
// updates the poster took (the wake scrubs after several).
static uint32_t gNapRevPriorities = 0, gNapRevWorkout = 0, gNapRevToday = 0;
static unsigned long gNapChangeAtMs = 0;
static uint8_t gNapPosterUpdates = 0;
// X4 nap entry (spec 2026-09-06): the poster lands FAST, then one quiet HALF
// clean while napping, so the entry feels like the wake and the poster
// still sits clean for the hour. 0 = no clean pending. `napclean` lever.
static unsigned long gNapCleanAtMs = 0;
static uint32_t gNapCleanDelayMs = 4000;  // napclean off -> 0
constexpr unsigned long kNapPosterSettleMs = 2000;  // a sync lands as several cards; redraw once after the last
constexpr uint8_t kNapPosterScrubAfter = 4;         // FAST updates before the wake repaint is a HALF scrub

static void snapshotNapRevisions() {
  gNapRevPriorities = PRIORITIES_STORE.revision();
  gNapRevWorkout = WORKOUT_STORE.revision();
  gNapRevToday = TODAY_STORE.revision();
}

// Called every loop tick while napping (the radio and the card parser keep
// running; only the scenes are paused). A changed snapshot starts the settle
// clock; when it runs out the poster is composed again and, if it differs,
// refreshed with one FAST.
static void pumpNapPoster() {
  if (gSleepPosterPreview) return;
  if (PRIORITIES_STORE.revision() != gNapRevPriorities || WORKOUT_STORE.revision() != gNapRevWorkout ||
      TODAY_STORE.revision() != gNapRevToday) {
    snapshotNapRevisions();
    gNapChangeAtMs = millis();
    return;
  }
  if (!gNapChangeAtMs || millis() - gNapChangeAtMs < kNapPosterSettleMs) return;
  gNapChangeAtMs = 0;
  const unsigned long t0 = millis();
  if (Sleep::refreshNapPoster(gfx)) {
    if (gNapPosterUpdates < 255) gNapPosterUpdates++;
    Serial.printf("[xphone-os] nap: poster updated from the phone (%lu ms, update %u)\n", millis() - t0,
                  (unsigned)gNapPosterUpdates);
  } else {
    Serial.println("[xphone-os] nap: snapshot changed, poster the same; glass left alone");
  }
}

static void enterNap(const char* why) {
  if (gNapping) return;
  if (gCurrentSceneId == SceneId::FileTransfer) return;  // a session owns the glass
  SCENES.waitFlushIdle();
  Serial.printf("[xphone-os] nap: %s; sleep screen on, link kept\n", why);
  gNapScene = gCurrentSceneId;
  // Decided while awake, when USB detection is reliable. On the cable a nap
  // must not light-sleep: the X4's USB link drops in light sleep, and the
  // host's reattach resets the chip (five reboots on the bench, 2026-09-06).
  // On a charger without a host nothing changes.
  gNapOnUsb = usbHostConnected();
  Sleep::requestFreshCardsNow();  // a connected phone refreshes the lists before the poster (2026-09-17)
  Sleep::drawSleepScreenNow(gfx, /*napping=*/true);
  SCENES.setPaused(true);  // nothing paints over the sleep screen (the link dot, cards)
  snapshotNapRevisions();
  gNapChangeAtMs = 0;
  gNapPosterUpdates = 0;
  gNapCleanAtMs = (!gDeviceIsX3 && gNapCleanDelayMs) ? millis() + gNapCleanDelayMs : 0;
  gNapping = true;
}

static void exitNap(const char* why) {
  if (!gNapping) return;
  gNapping = false;
  gSleepPosterPreview = false;
  gNapCleanAtMs = 0;  // a wake before the clean: the FAST differential works from the poster as it is
  Serial.printf("[xphone-os] nap: wake (%s)\n", why);
  SCENES.setPaused(false);
  // The poster is on the glass: one repaint back to the scene. After several
  // live updates the poster carries FAST traces, so that repaint is the HALF
  // scrub (X4 1.9 s) instead of the usual FAST (0.6 s).
  if (gNapPosterUpdates >= kNapPosterScrubAfter) SCENES.requestScrubRepaint();
  else SCENES.requestFullRepaint();
}

static void checkPowerButton() {
  // Short press: nap (or wake from a nap). Hold 2.5 s: off (deep sleep).
  // Hold 8 s: restart. A press is taken on its RELEASE with the hold the
  // 5 ms task measured, so a quick tap never falls between idle slices; the
  // minimum is 30 ms, since the SDK debounce already rejects bounces
  // (the 80 ms floor read about half of quick taps as blips on 0.6.6).
  constexpr unsigned long kRestartHoldMs = 8000;
  constexpr unsigned long kOffHoldMs = 2500;
  constexpr unsigned long kMinPressMs = 30;

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

  unsigned long held = 0;
  if (input.powerReleased(held)) {
    if (held >= kOffHoldMs) {
      Serial.printf("[xphone-os] power held %lums; off\n", held);
      Sleep::sleepNow(gfx, input);  // never returns
    }
    if (held >= kMinPressMs) {
      Serial.printf("[xphone-os] power pressed %lums; %s\n", held, gNapping ? "wake" : "nap");
      if (gNapping) exitNap("power");
      else enterNap("power press");
      return;
    }
    Serial.printf("[xphone-os] power blip %lums ignored\n", held);
  }
}

// Lane-9 power bench: read the BQ27220 fuel gauge (X3 only) so battery
// arguments cite a measured mA, not a guess. `pwr` prints one line now.
// `pwr rec <sec> [<period-s>]` arms a RAM recorder that keeps sampling with
// no USB host attached: arm it, unplug the pogo cable, drive the mode under
// test, replug, `pwr dump`. On the bench cable the gauge sees CHARGE current,
// not system draw — the recorder exists because only an unplugged device
// shows what reading actually costs. The ring lives on the heap from rec to
// dump only; checkAutoSleep holds the device awake while armed, because an
// unplugged idle device would otherwise deep-sleep mid-recording.
namespace powerbench {

struct Sample {
  uint16_t sec;   // seconds since rec start
  uint16_t mv;    // Voltage() 0x08
  int16_t ma;     // AverageCurrent() 0x14; negative = discharging
  uint8_t soc;    // StateOfCharge() 0x2C
  uint8_t scene;  // SceneId at sample time
  // bit0 light sleep active, bit1 phone connected, bit2 USB host seen,
  // bit3 advertising. Added 2026-09-03 so a trace explains its plateaus.
  uint8_t flags;
  uint8_t pad;
};

static Sample* ring = nullptr;
static uint16_t cap = 0;
static uint16_t count = 0;
static uint32_t startMs = 0;
static uint32_t durationMs = 0;
static uint32_t periodMs = 0;
static uint32_t nextMs = 0;
static bool recording = false;

static bool holdAwake() { return recording; }

static bool readGauge(uint16_t& mv, int16_t& ma, uint16_t& soc) {
  return BatteryGauge::readWord(BatteryGauge::kCmdVoltage, mv) &&
         BatteryGauge::readAvgCurrentMa(ma) &&
         BatteryGauge::readWord(BatteryGauge::kCmdStateOfCharge, soc);
}

static const char* sceneName(uint8_t id) {
  static constexpr const char* kNames[] = {"launcher", "notifications", "settings",
                                           "block",    "priorities",    "today",
                                           "about",    "reader",        "workout",
                                           "transfer"};
  return id < sizeof(kNames) / sizeof(kNames[0]) ? kNames[id] : "?";
}

static void printOnce() {
  uint16_t mv = 0, soc = 0, rem = 0, fcc = 0;
  int16_t ma = 0;
  if (!readGauge(mv, ma, soc)) {
    Serial.println("[xphone-os] devcon: pwr: no gauge (BQ27220 is X3-only)");
    return;
  }
  BatteryGauge::readWord(BatteryGauge::kCmdRemainingCapacity, rem);
  BatteryGauge::readWord(BatteryGauge::kCmdFullChargeCapacity, fcc);
  Serial.printf("[xphone-os] pwr: v=%umV i=%dmA soc=%u%% rem=%umAh fcc=%umAh scene=%s\n",
                static_cast<unsigned>(mv), static_cast<int>(ma), static_cast<unsigned>(soc),
                static_cast<unsigned>(rem), static_cast<unsigned>(fcc),
                sceneName(static_cast<uint8_t>(gCurrentSceneId)));
}

static void freeRing() {
  free(ring);
  ring = nullptr;
  cap = 0;
  count = 0;
}

static void startRec(uint32_t seconds, uint32_t periodS) {
  uint16_t mv = 0, soc = 0;
  int16_t ma = 0;
  if (!readGauge(mv, ma, soc)) {
    Serial.println("[xphone-os] devcon: pwr rec: no gauge (BQ27220 is X3-only)");
    return;
  }
  if (recording) {
    Serial.println("[xphone-os] devcon: pwr rec: already recording ('pwr stop' first)");
    return;
  }
  if (periodS == 0) periodS = 2;
  uint32_t want = seconds / periodS + 1;
  if (want > 600) want = 600;  // 4.8 KB heap cap; lengthen the period instead
  freeRing();
  ring = static_cast<Sample*>(malloc(want * sizeof(Sample)));
  if (!ring) {
    Serial.printf("[xphone-os] devcon: pwr rec: no heap for %lu samples\n",
                  static_cast<unsigned long>(want));
    return;
  }
  cap = static_cast<uint16_t>(want);
  count = 0;
  startMs = millis();
  durationMs = seconds * 1000UL;
  periodMs = periodS * 1000UL;
  nextMs = startMs;  // first sample immediately
  recording = true;
  Serial.printf("[xphone-os] devcon: pwr rec armed: %lus every %lus (%u samples max); "
                "unplug now, replug and 'pwr dump' when done\n",
                static_cast<unsigned long>(seconds), static_cast<unsigned long>(periodS),
                static_cast<unsigned>(cap));
}

// Reboot-proof: the bench logger resets the chip when the USB port comes
// back, which used to wipe an unplugged recording. A finished recording is
// stashed in NVS; 'pwr dump' reads it back after the reboot and clears it.
static constexpr const char* kRecNs = "xphone";
static constexpr const char* kRecKey = "pwrrec";
static constexpr const char* kRecPeriodKey = "pwrrecP";
static void stashRec() {
  if (!ring || count == 0) return;
  Preferences prefs;
  if (!prefs.begin(kRecNs, /*readOnly=*/false)) return;
  const size_t bytes = static_cast<size_t>(count) * sizeof(Sample);
  const size_t wrote = prefs.putBytes(kRecKey, ring, bytes);
  prefs.putUInt(kRecPeriodKey, periodMs);
  prefs.end();
  Serial.printf("[xphone-os] pwr rec stashed in NVS: %u of %u bytes\n", static_cast<unsigned>(wrote),
                static_cast<unsigned>(bytes));
}
static bool restoreRec() {
  Preferences prefs;
  if (!prefs.begin(kRecNs, /*readOnly=*/false)) return false;
  const size_t bytes = prefs.getBytesLength(kRecKey);
  if (bytes < sizeof(Sample)) {
    prefs.end();
    return false;
  }
  const uint16_t n = static_cast<uint16_t>(bytes / sizeof(Sample));
  ring = static_cast<Sample*>(malloc(n * sizeof(Sample)));
  if (!ring) {
    prefs.end();
    return false;
  }
  prefs.getBytes(kRecKey, ring, n * sizeof(Sample));
  periodMs = prefs.getUInt(kRecPeriodKey, 5000);
  prefs.remove(kRecKey);
  prefs.remove(kRecPeriodKey);
  prefs.end();
  cap = n;
  count = n;
  Serial.printf("[xphone-os] pwr dump: %u samples restored from NVS (recorded before the last reboot)\n",
                static_cast<unsigned>(n));
  return true;
}

static void stopRec(const char* why) {
  if (!recording) return;
  recording = false;
  Serial.printf("[xphone-os] pwr rec %s: %u samples held for 'pwr dump'\n", why,
                static_cast<unsigned>(count));
  stashRec();
}

static void dump() {
  if (recording) stopRec("stopped by dump");
  if ((!ring || count == 0) && !restoreRec()) {
    Serial.println("[xphone-os] devcon: pwr dump: nothing recorded");
    return;
  }
  Serial.printf("[xphone-os] pwr dump: %u samples, period %lus\n", static_cast<unsigned>(count),
                static_cast<unsigned long>(periodMs / 1000UL));
  int32_t sum[10] = {0};
  uint16_t n[10] = {0};
  for (uint16_t i = 0; i < count; ++i) {
    const Sample& s = ring[i];
    Serial.printf("[xphone-os] pwr %us v=%u i=%d soc=%u scene=%s ls=%u conn=%u usb=%u adv=%u\n",
                  static_cast<unsigned>(s.sec), static_cast<unsigned>(s.mv),
                  static_cast<int>(s.ma), static_cast<unsigned>(s.soc), sceneName(s.scene),
                  (s.flags & 1) ? 1u : 0u, (s.flags & 2) ? 1u : 0u, (s.flags & 4) ? 1u : 0u,
                  (s.flags & 8) ? 1u : 0u);
    if (s.scene < 10) {
      sum[s.scene] += s.ma;
      ++n[s.scene];
    }
  }
  for (uint8_t sc = 0; sc < 10; ++sc) {
    if (n[sc] == 0) continue;
    // Average in tenths of a mA so a 2-sample scene still shows a real number.
    const int32_t avg10 = sum[sc] * 10 / n[sc];
    Serial.printf("[xphone-os] pwr avg %s: %ld.%ldmA over %u samples\n", sceneName(sc),
                  static_cast<long>(avg10 / 10), static_cast<long>(labs(avg10) % 10),
                  static_cast<unsigned>(n[sc]));
  }
  freeRing();
}

// One loop-tick pump: cheap no-op unless armed.
static void pump() {
  if (!recording) return;
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - nextMs) >= 0 && count < cap) {
    uint16_t mv = 0, soc = 0;
    int16_t ma = 0;
    if (readGauge(mv, ma, soc)) {
      ring[count].sec = static_cast<uint16_t>((now - startMs) / 1000UL);
      ring[count].mv = mv;
      ring[count].ma = ma;
      ring[count].soc = static_cast<uint8_t>(soc);
      ring[count].scene = static_cast<uint8_t>(gCurrentSceneId);
      uint8_t fl = 0;
#if XP_LIGHT_SLEEP_LIBS
      if (gLsActive) fl |= 1;
#endif
      if (COMPANION_BLE.isConnected()) fl |= 2;
      if (usbHostConnected()) fl |= 4;
      if (COMPANION_BLE.isAdvertising()) fl |= 8;
      ring[count].flags = fl;
      ring[count].pad = 0;
      ++count;
      if (count % 10 == 0) stashRec();  // a crash mid-run keeps everything up to here
    }
    nextMs += periodMs;
  }
  if (now - startMs >= durationMs || count >= cap) stopRec("done");
}

}  // namespace powerbench

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

#if XP_LIGHT_SLEEP_LIBS
// ---- P4: automatic light sleep with the BLE link kept (x3ls package) ----
// Policy: Auto = sleep allowed only while no USB host is on the line (the
// USB Serial/JTAG port is gated in light sleep; a computer would see a dead
// device — CrossPoint's revert). On = always (bench, unplugged). Off = never.
// Pin holds: the package's PM_SLP_DISABLE_GPIO is forced on, and it would
// float every pin at sleep — GPIO13 is the X4 battery latch (and the X3 SD
// rail), so every pin keeps its state (gpio_sleep_sel_dis) and 13 is held.
enum class LsMode : uint8_t { Off, Auto, On };
static LsMode gLsMode = LsMode::Auto;
static uint32_t gLsOnUntilMs = 0;   // bench: 'ls on' reverts to auto after 10 min (USB comes back by itself)
static uint32_t gLsUsbWinOnS = 3;     // USB-handshake window while connected
static uint32_t gLsUsbWinPeriodS = 60; // ...every this many seconds
static uint32_t gLsWinOnS = 5;      // awake window length while the phone is away
static uint32_t gLsWinPeriodS = 30; // ...every this many seconds
static const char* lsModeName(LsMode m) { return m == LsMode::Off ? "off" : m == LsMode::On ? "on" : "auto"; }
static void lightSleepPrepare() {
  for (int pin = 0; pin <= 21; pin++) gpio_sleep_sel_dis(static_cast<gpio_num_t>(pin));
  gpio_hold_en(GPIO_NUM_13);
  Serial.println("[xphone-os] ls: pins keep their state in light sleep; GPIO13 held");
}
static void lightSleepApply(bool on, const char* why) {
  esp_pm_config_t cfg = {};
  cfg.max_freq_mhz = 160;
  cfg.min_freq_mhz = 40;
  cfg.light_sleep_enable = on;
  const esp_err_t rc = esp_pm_configure(&cfg);
  gLsActive = on && rc == ESP_OK;
  Serial.printf("[xphone-os] ls: light sleep %s (%s) rc=%d\n", gLsActive ? "ON" : "off", why, static_cast<int>(rc));
}
// A host that has just seen us attach issues a USB bus reset before it can
// enumerate. The USJ latches that as a raw interrupt bit even if we were
// only awake for a slice. Seeing it = a computer is trying to talk: hold
// the naps off for a minute so the handshake can finish (then the SOF poll
// keeps them off). Works without a VBUS pin or a gauge (X4).
static uint32_t gUsbGraceUntilMs = 0;
static bool usbHostTrying() {
  if (USB_SERIAL_JTAG.int_raw.usb_bus_reset_int_raw) {
    USB_SERIAL_JTAG.int_clr.usb_bus_reset_int_clr = 1;
    return true;
  }
  return false;
}
// Software re-plug (2026-09-06: Andrew's X3 sat dark on the Mac's cable for
// 30 min). Every guard below wakes the chip AFTER the host has tried and
// given up, and a host never retries an abandoned port; only a fresh attach
// does. Dropping the D+ pull-up for 200 ms looks exactly like a cable pulled
// and pushed back in, so the host starts over with a chip that is awake now.
// Runs only when no host is talking (no SOF ticks), so a live port is never
// disturbed. Bench note: the serial logger resets the chip when it reopens
// the port, so on the bench a re-plug reads as a reboot.
static void usbReplug(const char* why) {
  Serial.printf("[xphone-os] usb: re-plug (%s)\n", why);
  Serial.flush();
  {
    // Proof marker: the serial line above is lost (no host yet), and the
    // bench logger resets the chip once the port returns. The next boot
    // prints that a re-plug preceded it, with the uptime it fired at.
    Preferences pref;
    if (pref.begin("bench", /*readOnly=*/false)) {
      pref.putUInt("replugMs", millis());
      pref.end();
    }
  }
  USB_SERIAL_JTAG.conf0.pad_pull_override = 1;
  USB_SERIAL_JTAG.conf0.dp_pullup = 0;
  delay(200);
  USB_SERIAL_JTAG.conf0.dp_pullup = 1;
  USB_SERIAL_JTAG.conf0.pad_pull_override = 0;  // hardware keeps the pull-up from here
  gUsbGraceUntilMs = millis() + 60000UL;
}
static uint32_t gUsbKnockAtMs = 0;  // a knock seen, no ticks yet: re-plug after 3 s
static void lightSleepTick() {
  static uint32_t lastMs = 0;
  const uint32_t now = millis();
  if (usbHostTrying()) {
    gUsbGraceUntilMs = now + 60000UL;
    if (!gUsbKnockAtMs) gUsbKnockAtMs = now;
  }
  if (now - lastMs < 1000) return;
  lastMs = now;
  // Escape hatch: hold Back (top button) for 2 s -> mode Off. The console
  // cannot reach a sleeping device, so this is how the bench gets it back.
  static uint32_t backHeldSinceMs = 0;
  if (input.isPressed(Btn::Back)) {
    if (!backHeldSinceMs) backHeldSinceMs = now;
    else if (now - backHeldSinceMs >= 2000 && gLsMode != LsMode::Off) {
      gLsMode = LsMode::Off;
      Serial.println("[xphone-os] ls: Back held 2 s -> light sleep OFF");
    }
  } else {
    backHeldSinceMs = 0;
  }
  // Field finding 2026-09-03 (Andrew, X3 unplugged): a phone cannot START a
  // connection into a light-sleeping device; an existing link survives.
  // So sleep is allowed only while connected. Disconnected = awake at the
  // DFS floor, advertising, until the phone is back. 'on' ignores USB but
  // still needs the link; 'auto' needs both.
  if (gLsMode == LsMode::On && gLsOnUntilMs && static_cast<int32_t>(now - gLsOnUntilMs) >= 0) {
    gLsMode = LsMode::Auto;
    gLsOnUntilMs = 0;
    Serial.println("[xphone-os] ls: 'on' expired -> auto");
  }
  // Charging guard (2026-09-03 22:50): a napping chip cannot complete USB
  // enumeration, so a computer plugged into a sleeping device is never
  // detected (CrossPoint's freeze, seen on the X3 tonight). The gauge
  // knows before the host does: positive current = charging = a cable is
  // in. No naps while charging (power does not matter then anyway).
  static uint32_t lastGaugeMs = 0;
  static bool charging = false;
  bool chargingAppeared = false;
  if (now - lastGaugeMs >= 5000) {
    lastGaugeMs = now;
    int16_t ma = 0;
    if (gDeviceIsX3 && BatteryGauge::readAvgCurrentMa(ma)) {
      const bool nowCharging = ma > 5;
      chargingAppeared = nowCharging && !charging;
      charging = nowCharging;
    }
  }
  const bool ticks = usbHostConnected();
  // Re-plug triggers, all gated on "no host talking":
  //  - the X3's gauge just saw a cable (charging appeared) while we napped;
  //  - a knock was latched 3 s ago and no ticks followed;
  //  - X4 (no gauge): the start of each awake window while napping.
  if (ticks) gUsbKnockAtMs = 0;
  if (!ticks && chargingAppeared && gLsActive) usbReplug("charging appeared while napping");
  else if (!ticks && gUsbKnockAtMs && now - gUsbKnockAtMs >= 3000) {
    gUsbKnockAtMs = 0;
    usbReplug("host knocked, no ticks");
  }
  const bool usbGrace = static_cast<int32_t>(now - gUsbGraceUntilMs) < 0;
  const bool usb = ticks || charging || usbGrace || (gNapping && gNapOnUsb);
  const bool connected = COMPANION_BLE.isConnected();
  // The bus-reset grace overrides even 'on': a computer is knocking.
  // A Wi-Fi session (BLE down, so "disconnected") must never nap either:
  // a light-sleeping chip drops the TCP stream under the phone (2026-09-04).
  const bool transfer = gCurrentSceneId == SceneId::FileTransfer;
  const bool allowed = !gSleepPosterPreview && !usbGrace && !transfer && (gLsMode == LsMode::On || (gLsMode == LsMode::Auto && !usb));
  // Wake on touch: a press means the user is here; stay fully awake for a
  // minute so the phone can (re)connect and everything feels instant.
  // Only while DISCONNECTED: its job is to let the phone in. While connected
  // a press must not cost a minute awake (the afternoon trace showed every
  // page turn holding 16 mA for 60 s).
  const bool touched = !connected && input.msSinceActivity() < 60000;
  // Disconnected: nap, but open an awake window every gLsWinPeriodS seconds
  // for gLsWinOnS seconds, so a phone that comes back finds the device
  // awake and advertising (a sleeping advertiser misses new connections).
  const uint32_t phase = (now / 1000UL) % gLsWinPeriodS;
  const bool window = phase < gLsWinOnS;
  // USB handshake window (2026-09-03): a napping chip cannot complete USB
  // enumeration, so a computer plugged into a sleeping device is never
  // seen. While connected (no away-windows) stay awake gLsUsbWinOnS
  // seconds every gLsUsbWinPeriodS so an attached host can enumerate; the
  // SOF poll then keeps the naps off. Cheap: 3 s / 60 s = 5% duty.
  const uint32_t uphase = (now / 1000UL) % gLsUsbWinPeriodS;
  const bool usbWindow = connected && uphase < gLsUsbWinOnS;
  static bool prevUsbWindow = false;
  if (usbWindow && !prevUsbWindow && !ticks && gLsActive && !gDeviceIsX3) usbReplug("awake window (X4)");
  prevUsbWindow = usbWindow;
  const bool want = allowed && !touched && !usbWindow && (connected || !window);
  const char* why = !allowed ? (usbGrace ? "usb host knocking" : transfer ? "wi-fi session" : charging ? "charging" : usb ? "usb host" : lsModeName(gLsMode))
                    : touched ? "button pressed"
                    : usbWindow ? "usb handshake window"
                    : (!connected && window) ? "awake window (phone away)"
                    : connected ? "connected" : "phone away, napping";
  if (want != gLsActive) lightSleepApply(want, why);
}
#endif

static void checkAutoSleep() {
  if (gSleepPosterPreview) return;
#if XP_AUTO_SLEEP_MS > 0
  static unsigned long lastInputMs = 0;
  const unsigned long now = millis();
  if (input.wasAnyPressed() || input.powerPressed()) lastInputMs = now;

  // R2: never auto-sleep out of a File Transfer session — a multi-MB book
  // upload has no button presses, and deep-sleeping mid-request tears the
  // TCP connection under the phone. The scene pins the deadline instead of
  // being exempted outright so the normal window resumes the moment it exits.
  if (gCurrentSceneId == SceneId::FileTransfer) lastInputMs = now;

  // Power-bench recording runs unplugged by design; sleeping would end it.
  if (powerbench::holdAwake()) lastInputMs = now;

  // Bench hold: while a USB HOST is attached, never auto-sleep. A computer
  // polls the bus every millisecond, which advances the hardware USB frame
  // counter; a wall charger never polls, so battery/charger users keep the
  // normal windows. Sampled once per second; unplugging restarts the idle
  // window from that moment.
  if (usbHostConnected() && !gNapAfterMsOverride && !gOffAfterMsOverride) {  // bench overrides ignore the host
    lastInputMs = now;
    if (!gUsbHoldLogged) {
      Serial.println("[xphone-os] auto-sleep held: USB host attached");
      gUsbHoldLogged = true;
    }
  } else {
    gUsbHoldLogged = false;
  }

  // Two stages (2026-09-05): idle -> nap (5 min; 2 min while a block runs),
  // then idle -> off (60 min), never while charging. USB host attached holds
  // both, as before: the bench console and a computer's charge both need the
  // device awake.
  // Settings > Sleep (Andrew, 2026-09-06): the windows come from NVS, 0 =
  // never. A running block keeps its short window when that is shorter.
  const bool blockActive = BLOCK_STATUS.active();
  const unsigned long napSetting = static_cast<unsigned long>(Sleep::napAfterMin()) * 60000UL;
  const unsigned long offSetting = static_cast<unsigned long>(Sleep::offAfterMin()) * 60000UL;
  unsigned long napAfter = napSetting;
  if (blockActive && napAfter && napAfter > XP_AUTO_NAP_BLOCK_MS) napAfter = XP_AUTO_NAP_BLOCK_MS;
  if (gNapAfterMsOverride) napAfter = gNapAfterMsOverride;
  const unsigned long offAfter = gOffAfterMsOverride ? gOffAfterMsOverride : offSetting;
  const unsigned long idle = now - lastInputMs;  // rollover-safe unsigned subtraction
  if (!gNapping) {
    if (napAfter) {
      if (idle < napAfter) return;
      Serial.printf("[xphone-os] idle %lu min (%s window); napping\n", idle / 60000UL, blockActive ? "block" : "normal");
      enterNap("idle");
      return;
    }
    // Nap set to Never: idle goes straight to OFF at its own window.
  }
  if (!offAfter) return;  // OFF set to Never: the nap (or the awake screen) holds
  if (idle < offAfter) return;
  int16_t ma = 0;
  const bool charging = gDeviceIsX3 && BatteryGauge::readAvgCurrentMa(ma) && ma > 5;
  static bool chargeHoldLogged = false;
  if (charging) {
    if (!chargeHoldLogged) {
      Serial.println("[xphone-os] auto-off held: charging (napping instead)");
      chargeHoldLogged = true;
    }
    return;
  }
  chargeHoldLogged = false;
  Serial.printf("[xphone-os] idle %lu min; off\n", idle / 60000UL);
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
  const TaskHandle_t flushTask = SCENES.flushTask();
  const UBaseType_t flushHwm = flushTask ? uxTaskGetStackHighWaterMark(flushTask) : 0;
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
  Serial.printf("[xphone-os] stats: mode=%s loopHWM=%u B bleHWM=%s%u B flushHWM=%u B heapFree=%u largest=%u frag=%u%% minFree=%u "
                "ancsQpeak=%u drops=%lu\n",
                mode, static_cast<unsigned>(loopHwm), bleTask ? "" : "n/a ", static_cast<unsigned>(bleHwm),
                static_cast<unsigned>(flushHwm), heapFree,
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
// Serial file-put state (devcon fbegin/fdata/fend), ported from feat/home-apps
// for the 0.7 hardware passes: the Mini cannot reach a device over Wi-Fi
// (macOS Local Network privacy), so test books and the dictionary go over
// the console. ~8 KB/s; the sender paces on the per-chunk ack line.
static FsFile gFputFile;
static uint32_t gFputBytes = 0;

// Minimal base64 decode; returns output length or -1. '=' padding optional.
static int b64Decode(const char* in, uint8_t* out, int outMax) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  int n = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (; *in && *in != '='; ++in) {
    const int v = val(*in);
    if (v < 0) return -1;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= outMax) return -1;
      out[n++] = static_cast<uint8_t>((acc >> bits) & 0xFF);
    }
  }
  return n;
}

#if defined(FLOWE_BENCH_COMPACT)
// Keep diagnostic stack and allocations OUTSIDE pumpDevConsole: that pump
// also runs from HTTP cancellation callbacks with a small remaining stack.
static __attribute__((noinline)) void benchCodecCommand(const char* line) {
  if (transfer_sync::active() || transfer_sync::handlingHttp() || gCurrentSceneId != SceneId::Launcher) {
    Serial.println("[codec] run from launcher outside a transfer"); return;
  }
  unsigned profile = 0, first = 0, count = 0; int consumed = 0;
  if (sscanf(line + 6, "%u %u %u %n", &profile, &first, &count, &consumed) != 3 ||
      !consumed || !count || count > 64 || first > 65535 || strncmp(line + 6 + consumed, "/books/", 7)) {
    Serial.println("[codec] usage: codec <profile0> <page0> <count1..64> /books/<file>"); return;
  }
  SCENES.waitFlushIdle();
  std::unique_ptr<reader::FbpBook> book(new (std::nothrow) reader::FbpBook);
  if (!book) { Serial.println("[codec] no memory for reader"); return; }
  uint16_t width = 0, height = 0;
  const uint32_t openAt = micros();
  if (!book->open(line + 6 + consumed) || !book->benchProfile(profile, &width, &height) || first >= book->pageCount()) {
    Serial.println("[codec] open failed"); return;
  }
  const auto savedOrientation = gfx.orientation();
  gfx.setOrientation(width > height ? Gfx::Orient::Landscape : Gfx::Orient::Portrait);
  if ((width != gfx.width() && !(width + Scene::SOFTKEY_BAR_H == gfx.width() && width > height)) || height != gfx.height()) {
    gfx.setOrientation(savedOrientation); Serial.println("[codec] geometry mismatch"); return;
  }
  Serial.printf("[codec] begin profile=%u pages=%u open_us=%lu heap=%u probe_bytes=%u word_scratch=%u\n", profile, book->pageCount(),
                (unsigned long)(micros() - openAt), ESP.getFreeHeap(), (unsigned)sizeof(reader::FbpBook),
                (unsigned)(256 * sizeof(reader::FbpBook::WordBox)));
  for (uint32_t pg = first; pg < first + count && pg < book->pageCount(); pg++) {
    uint32_t bodyCrc = 0, t = micros();
    bool bodyOk = book->benchBodyCrc((uint16_t)pg, &bodyCrc);
    uint32_t decodeUs = micros() - t;
    gfx.clear(); t = micros();
    bool drawn = book->renderPage(gfx, (uint16_t)pg);
    uint32_t composeUs = micros() - t;
    const uint8_t* fb = display.getFrameBuffer();
    const uint32_t bytes = (uint32_t)display.getDisplayWidthBytes() * display.getDisplayHeight();
    const uint32_t pixels = fb ? bench::crc32(fb, bytes) : 0;
    auto* words = (reader::FbpBook::WordBox*)malloc(256 * sizeof(reader::FbpBook::WordBox));
    if (!words) { Serial.println("[codec] no memory for word scratch"); break; }
    const uint16_t nw = book->pageWords(words, 256); uint32_t wc = UINT32_MAX;
    for (uint16_t w = 0; w < nw; w++) {
      uint8_t fields[7] = {(uint8_t)words[w].x, (uint8_t)(words[w].x >> 8),
        (uint8_t)words[w].w, (uint8_t)(words[w].w >> 8), words[w].line, words[w].flags, words[w].textLen};
      char text[256]; book->wordText(words[w], text, sizeof(text));
      wc = fc_crc_bytes(wc, fields, sizeof(fields));
      wc = fc_crc_bytes(wc, (const uint8_t*)text, words[w].textLen);
    }
    free(words);
    Serial.printf("[codec] page=%lu ok=%u body=%08lx pixels=%08lx words=%u word_crc=%08lx decode_us=%lu compose_us=%lu peak=%lu heap=%u\n",
      (unsigned long)pg, bodyOk && drawn, (unsigned long)bodyCrc, (unsigned long)pixels, nw,
      (unsigned long)~wc, (unsigned long)decodeUs, (unsigned long)composeUs,
      (unsigned long)book->lastPeakBytes(), ESP.getFreeHeap());
    yield();
  }
  // Leave the last page in RAM for `fb` and `flushtier full`. The caller
  // uses `redraw` when finished; no automatic flush enters the timing.
  Serial.println("[codec] end");
}
#endif

static void pumpDevConsole() {
  if (!usbHostConnected()) return;
  static char line[256];  // was 32; the file-put chunks and long Wi-Fi passwords need room
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
    if (transfer_sync::active() || transfer_sync::handlingHttp()) {
      if (!strcmp(line, "reboot") || !strcmp(line, "wakeboot") ||
          !strcmp(line, "sync stop") || !strcmp(line, "btn back") || !strcmp(line, "btn back long")) {
        transfer_sync::requestCancel();
        continue;
      }
      const bool allowed = !strcmp(line, "where") || !strcmp(line, "netmem") || !strcmp(line, "heapdump") ||
                           (!transfer_sync::handlingHttp() && !strcmp(line, "fb"));
      if (!allowed) {
        Serial.println("[syncmem] static sync display; use sync stop or btn back to cancel");
        continue;
      }
    }
#if defined(FLOWE_BENCH_SD_WRITE)
    if (bench::handleSdWriteCommand(line)) continue;
#endif
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
#if defined(FLOWE_BENCH_COMPACT)
    if (!strncmp(line, "codec ", 6)) { benchCodecCommand(line); continue; }
#endif
    if (!strcmp(line, "arenaoff")) {
      // Bench: prove the never-blank render path (glyphs read per use).
      extern bool benchSetNoArena(bool);
      benchSetNoArena(true);
      Serial.println("[xphone-os] devcon: arenaoff — pages render without an arena until reboot");
      continue;
    }
    if (!strcmp(line, "linecids")) {
      readerLineCids();
      continue;
    }
    if (!strncmp(line, "goto ", 5)) {
      // Bench: goto <key> <cid> — inject a phone jump through the same latch
      // reader.goto uses, so X1 is provable without the app's sender.
      char key[64] = {0};
      unsigned long cid = 0;
      if (sscanf(line + 5, "%63s %lu", key, &cid) == 2 && cid > 0) {
        COMPANION_BLE.benchInjectGoto(key, (uint32_t)cid);
        Serial.printf("[xphone-os] devcon: goto injected key=%s cid=%lu\n", key, cid);
      } else {
        Serial.println("[xphone-os] devcon: usage: goto <key> <cid>");
      }
      continue;
    }
    if (!strncmp(line, "hlcard ", 7)) {
      // Bench: feed a home.layout / app.remove / device.apps.request card
      // as JSON, without the phone. Install cards need the real link.
      JsonDocument doc;
      if (deserializeJson(doc, line + 7)) {
        Serial.println("[xphone-os] devcon: hlcard bad json");
        continue;
      }
      const char* t = doc["type"] | (doc["kind"] | "");
      Serial.printf("[xphone-os] devcon: hlcard %s -> %s\n", t,
                    apps_mgr::handleCard(doc.as<JsonObjectConst>(), t) ? "handled" : "ignored");
      continue;
    }
    if (!strcmp(line, "homedump")) {
      homeDebugDump();
      continue;
    }
    if (!strncmp(line, "fget ", 5)) {
      // Bench serial file-read, the sibling of fbegin/fdata/fend. Prints the
      // file as base64 lines ("fget: <b64>") and a final "fget: end <n>".
      const char* path = line + 5;
      if (!SdMan.ready() && !SdMan.begin()) {
        Serial.println("[xphone-os] devcon: fget no SD");
        continue;
      }
      FsFile f = SdMan.open(path, O_RDONLY);
      if (!f) {
        Serial.printf("[xphone-os] devcon: fget missing %s\n", path);
        continue;
      }
      static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      uint8_t buf[120];
      char out[164];
      uint32_t total = 0;
      int n;
      while ((n = f.read(buf, sizeof(buf))) > 0) {
        int o = 0;
        for (int i = 0; i < n; i += 3) {
          const uint32_t b0 = buf[i];
          const uint32_t b1 = i + 1 < n ? buf[i + 1] : 0;
          const uint32_t b2 = i + 2 < n ? buf[i + 2] : 0;
          const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
          out[o++] = kB64[(v >> 18) & 63];
          out[o++] = kB64[(v >> 12) & 63];
          out[o++] = i + 1 < n ? kB64[(v >> 6) & 63] : '=';
          out[o++] = i + 2 < n ? kB64[v & 63] : '=';
        }
        out[o] = 0;
        Serial.printf("fget: %s\n", out);
        total += n;
        delay(8);  // let the host-side reader drain
      }
      f.close();
      Serial.printf("fget: end %lu\n", static_cast<unsigned long>(total));
      continue;
    }
    if (!strncmp(line, "home", 4)) {
      // Bench: switch the home layout. "home widget" / "home tiles" persist
      // the choice and switch now; bare "home" prints the current one.
      const char* arg = line[4] == ' ' ? line + 5 : "";
      if (!strcmp(arg, "widget")) {
        setHomeLayout(HomeLayout::Widget);
        showHome();
      } else if (!strcmp(arg, "tiles")) {
        setHomeLayout(HomeLayout::Tiles);
        showLauncher();
      } else {
        Serial.printf("[xphone-os] devcon: home layout=%s\n",
                      homeLayout() == HomeLayout::Widget ? "widget" : "tiles");
        continue;
      }
      Serial.printf("[xphone-os] devcon: home layout set=%s\n", arg);
      continue;
    }
    if (!strncmp(line, "app ", 4)) {
      // Bench: open a declarative app from SD (/apps/<name>/app.json).
      if (!showApp(line + 4)) Serial.println("[xphone-os] devcon: app load failed");
      continue;
    }
    if (!strcmp(line, "apps")) {
      // Bench: list what /apps holds.
      if (!SdMan.ready() && !SdMan.begin()) {
        Serial.println("[xphone-os] devcon: no SD");
        continue;
      }
      FsFile dir = SdMan.open("/apps", O_RDONLY);
      if (!dir) {
        Serial.println("[xphone-os] devcon: no /apps directory");
        continue;
      }
      FsFile f;
      char nm[48];
      while (f.openNext(&dir, O_RDONLY)) {
        if (f.isDir() && f.getName(nm, sizeof(nm)) > 0)
          Serial.printf("[xphone-os] devcon: app %s\n", nm);
        f.close();
      }
      dir.close();
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
    if (!strcmp(line, "apcreds")) {
      // Bench-only: the hotspot's name and password, so a bench client can
      // try the join without a phone's own dialog in the way.
      char s[33], pw[17];
      WifiCreds::apCredentials(s, sizeof(s), pw, sizeof(pw));
      Serial.printf("[xphone-os] devcon: ap ssid=%s pass=%s\n", s, pw);
      continue;
    }
    if (!strncmp(line, "wifiadd ", 8)) {
      // Bench-only: save a network (ssid<space>pass) so this device can join
      // another device's hotspot as a plain station witness.
      char* sp = strchr(line + 8, ' ');
      if (sp) {
        *sp = '\0';
        const bool ok = WifiCreds::add(line + 8, sp + 1);
        Serial.printf("[xphone-os] devcon: wifiadd %s -> %s\n", line + 8, ok ? "saved" : "FAILED");
      }
      continue;
    }
    if (!strncmp(line, "rm ", 3) && line[3] == '/') {
      // Bench: remove one file from the card with NO tombstone, so the phone
      // still believes in the book and the next sync has something to send.
      // (/delete over HTTP writes a tombstone = "the user deleted it", and
      // the phone then drops the book from its own library too.)
      const bool ok = SdMan.remove(line + 3);
      Serial.printf("[xphone-os] devcon: rm %s -> %s\n", line + 3, ok ? "removed" : "FAILED");
      continue;
    }
    if (!strncmp(line, "isolate", 7)) {
      // Bench: 'isolate on' makes the server accept connections but never
      // answer, like a guest network with client isolation. Walks the
      // device-side reach test (no knock in 20 s -> hotspot).
      const char* a = line[7] == ' ' ? line + 8 : "";
      FileTransferServer::isolate = !strcmp(a, "on");
      Serial.printf("[xphone-os] devcon: isolate %s\n", FileTransferServer::isolate ? "on" : "off");
      continue;
    }
    if (!strncmp(line, "txpwr ", 6)) {
      // Bench-only: max Wi-Fi TX power for the next joins, 0.25 dBm units
      // (8 = 2 dBm, 20 = 5 dBm, 44 = 11 dBm, 78 = default). 0 = default.
      extern int8_t gWifiTxPowerQuarterDb;
      gWifiTxPowerQuarterDb = static_cast<int8_t>(atoi(line + 6));
      Serial.printf("[xphone-os] devcon: txpwr %d\n", gWifiTxPowerQuarterDb);
      continue;
    }
    if (!strcmp(line, "gattchg")) {
      // Bench-only: re-send the GATT Service Changed announcement so a
      // bonded phone's cache refresh can be watched on demand.
      COMPANION_BLE.announceGattTableIfChanged(/*force=*/true);
      Serial.println("[xphone-os] devcon: gattchg sent");
      continue;
    }
    if (!strcmp(line, "wifibad")) {
      // Bench-only: the next Wi-Fi join uses a wrong password, so the
      // failed-join exit (no-restart: linger, NVS breadcrumb, BLE back,
      // "failed" on the phone's first write) can be walked on demand.
      extern bool gTransferBadPassword;
      gTransferBadPassword = true;
      Serial.println("[xphone-os] devcon: next join uses a wrong password");
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
          "today",    "about",         "reader",    "workout", "transfer", "wifi", "home"};
      const uint32_t id = static_cast<uint32_t>(gCurrentSceneId);
      char detail[96] = {0};
      if (gCurrentSceneId == SceneId::Reader) readerWhere(detail, sizeof(detail));
      Serial.printf("[xphone-os] devcon: where scene=%s launcherSel=%d%s%s\n",
                    id < sizeof(kSceneNames) / sizeof(kSceneNames[0]) ? kSceneNames[id] : "?",
                    launcherSelection(), detail[0] ? " reader=" : "", detail);
      continue;
    }
    if (!strncmp(line, "pwr", 3) && (line[3] == 0 || line[3] == ' ')) {
      // Lane-9 power bench (see the powerbench namespace above).
      //   pwr                 — one gauge reading now
      //   pwr rec <s> [<p>]   — record for <s> seconds every <p> (default 2)
      //   pwr stop            — end a recording early (samples kept)
      //   pwr dump            — print samples + per-scene averages, free ring
      const char* a = line[3] ? line + 4 : "";
      if (!*a) {
        powerbench::printOnce();
      } else if (!strncmp(a, "rec", 3) && (a[3] == 0 || a[3] == ' ')) {
        unsigned long sec = 0, per = 2;
        sscanf(a + 3, "%lu %lu", &sec, &per);
        if (sec == 0) {
          Serial.println("[xphone-os] devcon: usage: pwr rec <seconds> [<period-s>]");
        } else {
          powerbench::startRec(sec, per);
        }
      } else if (!strcmp(a, "stop")) {
        powerbench::stopRec("stopped");
      } else if (!strcmp(a, "dump")) {
        powerbench::dump();
      } else {
        Serial.println("[xphone-os] devcon: pwr | pwr rec <s> [<p>] | pwr stop | pwr dump");
      }
      continue;
    }
#if XP_LIGHT_SLEEP_LIBS
    if (!strncmp(line, "ls", 2) && (line[2] == 0 || line[2] == ' ')) {
      // P4 bench lever: ls | ls on | ls off | ls auto
      const char* a = line[2] ? line + 3 : "";
      if (!strcmp(a, "on")) {
        gLsMode = LsMode::On;
        gLsOnUntilMs = millis() + 10UL * 60UL * 1000UL;
      }
      else if (!strcmp(a, "off")) gLsMode = LsMode::Off;
      else if (!strcmp(a, "auto")) gLsMode = LsMode::Auto;
      Serial.printf("[xphone-os] devcon: ls mode=%s active=%d usbhost=%d idle=%lums\n", lsModeName(gLsMode),
                    gLsActive ? 1 : 0, usbHostConnected() ? 1 : 0,
                    static_cast<unsigned long>(Input::idleSampleMs()));
      continue;
    }
    if (!strncmp(line, "connlat ", 8)) {
      // Radio knob: peripheral latency for the low-duty link (default 4 =
      // wake every 900 ms at 180 ms; Apple cap: itvl*(lat+1) <= 2 s -> 9).
      const int lat = atoi(line + 8);
      if (lat >= 0 && lat <= 9) COMPANION_ANCS.setLowDutyLatency(static_cast<uint16_t>(lat));
      Serial.printf("[xphone-os] devcon: connlat %d (re-requesting)\n", lat);
      continue;
    }
    if (!strncmp(line, "advconn", 7)) {
      // Radio knob: keep advertising for a second phone while connected?
      const char* a = line[7] == ' ' ? line + 8 : "";
      if (!strcmp(a, "on")) COMPANION_BLE.setAdvertiseWhileConnected(true);
      else if (!strcmp(a, "off")) COMPANION_BLE.setAdvertiseWhileConnected(false);
      Serial.printf("[xphone-os] devcon: advconn %s\n", COMPANION_BLE.advertiseWhileConnected() ? "on" : "off");
      continue;
    }
    if (!strncmp(line, "advwd", 5)) {
      // Advertising watchdog lever: 'advwd <min>' sets the stage period,
      // 'advwd now' skips the wait for the next stage, 'advwd' prints state.
      const char* a = line[5] == ' ' ? line + 6 : "";
      if (!strcmp(a, "now")) COMPANION_BLE.forceAdvWatchdogStage();
      else if (*a) COMPANION_BLE.setAdvWatchdogPeriodMin(static_cast<uint32_t>(atoi(a)));
      char st[160];
      COMPANION_BLE.advWatchdogStatus(st, sizeof(st));
      Serial.printf("[xphone-os] devcon: advwd %s\n", st);
      continue;
    }
    if (!strncmp(line, "xferkeepbt", 10)) {
      extern bool gTransferKeepBt;
      const char* a = line[10] == ' ' ? line + 11 : "";
      if (!strcmp(a, "on")) gTransferKeepBt = true;
      else if (!strcmp(a, "off")) gTransferKeepBt = false;
      Serial.printf("[xphone-os] devcon: xferkeepbt %s\n", gTransferKeepBt ? "on" : "off");
      continue;
    }
    if (!strncmp(line, "bledrop", 7)) {
      // Bench: drop the phone link now, or 'bledrop in <s>' later (so the
      // console can be dead by then — light sleep kills USB). The phone then
      // reconnects through its normal dropout path; the Moto's dumpsys
      // bluetooth_manager shows when.
      unsigned in = 0;
      if (sscanf(line + 7, " in %u", &in) == 1 && in > 0 && in <= 3600) {
        gBleDropAtMs = millis() + in * 1000UL;
        Serial.printf("[xphone-os] devcon: bledrop in %us\n", in);
      } else {
        COMPANION_BLE.dropLinks();
      }
      continue;
    }
    if (!strncmp(line, "lswin ", 6)) {
      // P4 bench lever: awake window while the phone is away: lswin <on_s> <period_s>
      unsigned on = 0, per = 0;
      if (sscanf(line + 6, "%u %u", &on, &per) == 2 && on >= 1 && per > on && per <= 600) {
        gLsWinOnS = on;
        gLsWinPeriodS = per;
      }
      Serial.printf("[xphone-os] devcon: lswin on=%lus period=%lus\n", static_cast<unsigned long>(gLsWinOnS),
                    static_cast<unsigned long>(gLsWinPeriodS));
      continue;
    }
    if (!strncmp(line, "lsusbwin ", 9)) {
      unsigned on = 0, per = 0;
      if (sscanf(line + 9, "%u %u", &on, &per) == 2 && on >= 1 && per > on && per <= 600) {
        gLsUsbWinOnS = on;
        gLsUsbWinPeriodS = per;
      }
      Serial.printf("[xphone-os] devcon: lsusbwin on=%lus period=%lus\n", static_cast<unsigned long>(gLsUsbWinOnS),
                    static_cast<unsigned long>(gLsUsbWinPeriodS));
      continue;
    }
    if (!strncmp(line, "lsidle ", 7)) {
      // P4 bench lever: idle slice in ms (20..500). Press delay grows with it.
      const int ms = atoi(line + 7);
      if (ms >= 20 && ms <= 500) Input::idleSampleMs() = static_cast<uint32_t>(ms);
      Serial.printf("[xphone-os] devcon: lsidle %lums\n", static_cast<unsigned long>(Input::idleSampleMs()));
      continue;
    }
#endif
    if (!strncmp(line, "napclean", 8)) {
      // Bench: the X4's quiet clean after a FAST nap entry. napclean off | <seconds>
      const char* a = line[8] == ' ' ? line + 9 : "";
      if (!strcmp(a, "off")) gNapCleanDelayMs = 0;
      else if (*a) gNapCleanDelayMs = static_cast<uint32_t>(atoi(a)) * 1000UL;
      Serial.printf("[xphone-os] devcon: napclean %lu ms\n", static_cast<unsigned long>(gNapCleanDelayMs));
      continue;
    }
    if (!strncmp(line, "postertier", 10)) {
      // Bench A/B: the sleep poster's tier. postertier half | full | default
      const char* a = line[10] == ' ' ? line + 11 : "";
      Sleep::gPosterTierOverride = !strcmp(a, "half") ? 1 : !strcmp(a, "full") ? 2 : 0;
      Serial.printf("[xphone-os] devcon: postertier %s\n", Sleep::gPosterTierOverride == 1 ? "half"
                                                           : Sleep::gPosterTierOverride == 2 ? "full" : "default");
      continue;
    }
    if (!strncmp(line, "bootfull", 8)) {
      // Bench A/B: keep the true-temperature FULL as the first clear after a
      // boot (X4). Stored in NVS so it holds across `wakeboot`. bootfull on|off
      const char* a = line[8] == ' ' ? line + 9 : "";
      Preferences p;
      if (p.begin("bench", false)) {
        if (!strcmp(a, "on")) p.putUChar("bootfull", 1);
        else if (!strcmp(a, "off")) p.remove("bootfull");
        Serial.printf("[xphone-os] devcon: bootfull %s\n", p.getUChar("bootfull", 0) ? "on" : "off");
        p.end();
      }
      continue;
    }
    if (!strncmp(line, "x4temp", 6)) {
      // Bench lever: the temperature the X4 panel believes for a HALF refresh.
      //   x4temp <celsius> | x4temp default | (no arg: print)
      const char* a = line[6] == ' ' ? line + 7 : "";
      if (!strcmp(a, "default")) display.setHalfTemp(0x7F);
      else if (*a) display.setHalfTemp((int8_t)atoi(a));
      const int8_t t = display.halfTemp();
      if (t == 0x7F) Serial.println("[xphone-os] devcon: x4temp default");
      else Serial.printf("[xphone-os] devcon: x4temp %d C\n", (int)t);
      continue;
    }
    if (!strncmp(line, "flushtier", 9)) {
      // Bench lever: flush the framebuffer as it is, with a chosen tier,
      // through the flush task. Times land in the usual draw/refresh line.
      //   flushtier full | half | fast
      const char* a = line[9] == ' ' ? line + 10 : "";
      SceneManager::NowTier tier = SceneManager::NowTier::Fast;
      if (!strcmp(a, "full")) tier = SceneManager::NowTier::Full;
      else if (!strcmp(a, "half")) tier = SceneManager::NowTier::Half;
      else if (strcmp(a, "fast")) { Serial.println("[xphone-os] devcon: flushtier full|half|fast"); continue; }
      const unsigned long t0 = millis();
      SCENES.flushFramebufferNow(gfx, tier);
      Serial.printf("[xphone-os] devcon: flushtier %s %lums\n", a, millis() - t0);
      continue;
    }
    if (!strncmp(line, "fbegin ", 7)) {
      // Bench serial file-put: fbegin <path> / fdata <base64> ... / fend.
      const char* path = line + 7;
      if (!SdMan.ready() && !SdMan.begin()) {
        Serial.println("[xphone-os] devcon: fbegin no SD");
        continue;
      }
      char dir[80];
      snprintf(dir, sizeof(dir), "%s", path);
      if (char* slash = strrchr(dir, '/')) {
        if (slash != dir) {
          *slash = 0;
          if (!SdMan.exists(dir) && !SdMan.mkdir(dir))
            Serial.printf("[xphone-os] devcon: fbegin mkdir %s failed\n", dir);
        }
      }
      if (gFputFile) gFputFile.close();
      gFputFile = SdMan.open(path, O_WRONLY | O_CREAT | O_TRUNC);
      gFputBytes = 0;
      Serial.printf("[xphone-os] devcon: fbegin %s %s\n", path, gFputFile ? "ok" : "FAILED");
      continue;
    }
    if (!strncmp(line, "fdata ", 6)) {
      if (!gFputFile) {
        Serial.println("[xphone-os] devcon: fdata without fbegin");
        continue;
      }
      uint8_t out[180];
      const int n = b64Decode(line + 6, out, sizeof(out));
      if (n < 0) {
        Serial.println("[xphone-os] devcon: fdata bad base64");
        continue;
      }
      gFputFile.write(out, n);
      gFputBytes += n;
      // Ack every chunk: the sender paces on this line (the USB-CDC RX buffer is small).
      Serial.printf("[xphone-os] devcon: fdata ok %lu\n", static_cast<unsigned long>(gFputBytes));
      continue;
    }
    if (!strcmp(line, "fend")) {
      if (gFputFile) {
        gFputFile.close();
        Serial.printf("[xphone-os] devcon: fend wrote %lu bytes\n", static_cast<unsigned long>(gFputBytes));
      } else {
        Serial.println("[xphone-os] devcon: fend without fbegin");
      }
      continue;
    }
    if (!strncmp(line, "paneloff", 8)) {
      // P2 bench lever: power the panel booster down after every fast refresh.
      //   paneloff on | off | (no arg: print)
      const char* a = line[8] == ' ' ? line + 9 : "";
      if (!strcmp(a, "on")) display.setIdlePowerOff(true);
      else if (!strcmp(a, "off")) display.setIdlePowerOff(false);
      Serial.printf("[xphone-os] devcon: paneloff %s\n", display.idlePowerOff() ? "on" : "off");
      continue;
    }
#if CONFIG_PM_ENABLE
    if (!strncmp(line, "dfs", 3) && (line[3] == 0 || line[3] == ' ')) {
      // Lane-9 DFS experiment: let the CPU downclock between loop ticks.
      //   dfs               — print the live configuration
      //   dfs <min> <max>   — enable scaling (no light sleep, ever: it would
      //                       kill USB and the BLE sleep clock)
      //   dfs off           — pin 80/80, today's stock behavior
      // Bench-only A/B lever; boot leaves scaling OFF so the experiment
      // never changes behavior until asked.
      const char* a = line[3] ? line + 4 : "";
      esp_pm_config_t cfg = {};
      if (!*a) {
        esp_pm_get_configuration(&cfg);
        Serial.printf("[xphone-os] devcon: dfs max=%d min=%d lightsleep=%d\n", cfg.max_freq_mhz,
                      cfg.min_freq_mhz, cfg.light_sleep_enable ? 1 : 0);
      } else if (!strcmp(a, "off")) {
        cfg.max_freq_mhz = 80;
        cfg.min_freq_mhz = 80;
        cfg.light_sleep_enable = false;
        Serial.printf("[xphone-os] devcon: dfs off rc=%d\n",
                      static_cast<int>(esp_pm_configure(&cfg)));
      } else {
        int mn = 0, mx = 0;
        if (sscanf(a, "%d %d", &mn, &mx) == 2 && mn > 0 && mx >= mn) {
          cfg.max_freq_mhz = mx;
          cfg.min_freq_mhz = mn;
          cfg.light_sleep_enable = false;
          Serial.printf("[xphone-os] devcon: dfs min=%d max=%d rc=%d\n", mn, mx,
                        static_cast<int>(esp_pm_configure(&cfg)));
        } else {
          Serial.println("[xphone-os] devcon: usage: dfs | dfs <min> <max> | dfs off");
        }
      }
      continue;
    }
#endif
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
    if (!strncmp(line, "sta ", 4) && line[4]) {
      // Bench: the route as the phone would ask it. 'sta <ssid>' = "I am on
      // <ssid>": the device joins that network only, or becomes the hotspot.
      // 'sta <ssid> <pass>' = the phone-hotspot rung (session-only creds).
      char ssid[64] = {0}, pass[64] = {0};
      const char* sp = strchr(line + 4, ' ');
      if (sp) {
        snprintf(ssid, sizeof(ssid), "%.*s", (int)(sp - (line + 4)), line + 4);
        snprintf(pass, sizeof(pass), "%s", sp + 1);
      } else {
        snprintf(ssid, sizeof(ssid), "%s", line + 4);
      }
      COMPANION_BLE.setTransferTarget(ssid, pass);
      Serial.printf("[xphone-os] devcon: sta target \"%s\"%s\n", ssid, pass[0] ? " (with a session password)" : "");
      showFileTransferAutoStart();
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
    if (!strcmp(line, "stafb")) {
      // Bench: a session with NO target but the hotspot fallback allowed —
      // the shape a phone sends when its OS withholds the network name.
      // With 'isolate on' this walks the rescue that used to be missing:
      // served, nobody knocked, become the hotspot anyway. (2026-09-10)
      COMPANION_BLE.setTransferHotspotFallback(true);
      Serial.println("[xphone-os] devcon: stafb (no target, hotspot fallback allowed)");
      showFileTransferAutoStart();
      continue;
    }
    if (!strcmp(line, "stainplace")) {
      // Bench: the phone-started shape of a session (in place, the sync bar
      // over the current scene, restart back to it at the end) without a
      // phone. 2026-09-07, for the sync-bar and restart proof.
      Serial.println("[xphone-os] devcon: stainplace");
      showFileTransferAutoStartInPlace(/*direct=*/false);
      continue;
    }
#if defined(FLOWE_TRANSFER_MEMORY_PROBE)
    if (!strcmp(line, "netguest")) {
      // Bench-only: exercise a failed guest download without ending STA.
      // Production session tokens still come from the paired phone over BLE.
      extern void transferSetSessionToken(const char* token, bool ownedReadiness);
      if (gTransferMemoryLocal) transferSetSessionToken("bench-guest-cleanup", true);
      continue;
    }
    if (!strcmp(line, "alloctest")) {
      allocationProbeSelfTest();
      continue;
    }
    if (!strcmp(line, "netmem")) {
      // TCP/IP may not exist before the first Wi-Fi session of this boot.
      if (gTransferMemoryLocal) transferNetworkMemoryProbe();
      else Serial.println("[netmem] start a transfer server before this probe");
      continue;
    }
#endif
    if (!strcmp(line, "heapdump")) {
      // Bench: walk every heap block into a buffer, then print. Printing
      // inside the walk is impossible: the heap lock is held there and the
      // USB CDC write path silently drops. Used blocks >= 128 B and every
      // free block are listed (addr size); smaller used ones are counted.
      // Two snapshots diffed by size name the residue a Wi-Fi session
      // leaves behind (no-restart transfer exit, 2026-09-04).
      struct Blk { uint32_t addr, size; };
      struct W { Blk* v; uint32_t n, cap, small, smallBytes, freeN, freeBytes; };
      W w{};
      w.cap = 700;
      w.v = static_cast<Blk*>(malloc(w.cap * sizeof(Blk)));
      Serial.printf("[xphone-os] devcon: heapdump free=%u largest=%u\n", ESP.getFreeHeap(),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
      if (w.v) {
        heap_caps_walk(MALLOC_CAP_8BIT, [](walker_heap_into_t, walker_block_info_t b, void* ud) -> bool {
          W* w = static_cast<W*>(ud);
          if (!b.used) { w->freeN++; w->freeBytes += b.size; }
          else if (b.size < 128) { w->small++; w->smallBytes += b.size; return true; }
          if (w->n < w->cap) w->v[w->n++] = Blk{reinterpret_cast<uint32_t>(b.ptr), static_cast<uint32_t>(b.size) | (b.used ? 0x80000000u : 0)};
          return true;
        }, &w);
        for (uint32_t i = 0; i < w.n; i++) {
          Serial.printf("%c 0x%08lx %lu\n", (w.v[i].size & 0x80000000u) ? 'U' : 'F', static_cast<unsigned long>(w.v[i].addr),
                        static_cast<unsigned long>(w.v[i].size & 0x7fffffffu));
          if ((i & 7) == 7) Serial.flush();
        }
        free(w.v);
      }
      Serial.printf("[xphone-os] devcon: heapdump end listed=%u small=%u (%u B) free=%u (%u B)\n", (unsigned)w.n, (unsigned)w.small,
                    (unsigned)w.smallBytes, (unsigned)w.freeN, (unsigned)w.freeBytes);
      continue;
    }
    if (!strcmp(line, "tasks")) {
      // Bench: FreeRTOS task table (name, state, prio, stack HWM, number).
      // ~45 B per task; 1024 cut the table at 9 tasks.
      char* table = static_cast<char*>(malloc(1536));
      if (!table) continue;
      vTaskList(table);
      Serial.printf("[xphone-os] devcon: tasks\n%s[xphone-os] devcon: tasks end\n", table);
      free(table);
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
      // Heap for the bench probe (was 2 KB of permanent BSS for a command
      // nobody runs in the field — efficiency audit 2026-09-02).
      uint8_t* buf = static_cast<uint8_t*>(malloc(2048));
      if (!buf) {
        Serial.println("[xphone-os] devcon: sdtest: no 2 KB buffer");
        continue;
      }
      struct BufFree { uint8_t* p; ~BufFree() { free(p); } } bufFree{buf};
      const uint32_t t0 = millis();
      uint32_t total = 0;
      // Deliberate work, not a hang: keep the stall watcher fed, or a slow
      // card would file this very test as a freeze.
      while (millis() - t0 < 3000) {
        const int got = f.read(buf, 2048);
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
    if (!strncmp(line, "prioseed ", 9)) {
      // Bench: fill the priorities store with n long dummy items (max 10) to
      // check the sleep poster's layout at the full list.
      int n = atoi(line + 9);
      if (n < 0) n = 0;
      if (n > static_cast<int>(CompanionProtocol::MAX_PRIORITY_ITEMS)) n = CompanionProtocol::MAX_PRIORITY_ITEMS;
      CompanionCardState c;
      c.id = "bench-seed";
      c.part = 0;
      c.parts = 1;
      c.priorityItemCount = static_cast<std::size_t>(n);
      for (int i = 0; i < n; i++) {
        char id[16], title[96];
        snprintf(id, sizeof(id), "seed-%d", i + 1);
        snprintf(title, sizeof(title), "Priority %d: a long title to stretch the row all the way across", i + 1);
        c.priorityItems[i].id = id;
        c.priorityItems[i].title = title;
        c.priorityItems[i].done = (i % 3 == 2);
      }
      PRIORITIES_STORE.updateFromCard(c);
      Serial.printf("[xphone-os] devcon: prioseed %d -> store holds %u\n", n, (unsigned)PRIORITIES_STORE.count());
      continue;
    }
    if (!strcmp(line, "nap")) {
      Serial.println("[xphone-os] devcon: nap");
      enterNap("devcon");
      continue;
    }
    if (!strcmp(line, "poster off")) {
      // Bench: hold the sleep poster while fully awake for camera and
      // framebuffer checks. Use wakeboot to leave the preview.
      Serial.println("[xphone-os] devcon: poster off (held, no sleep)");
      if (!gNapping && gCurrentSceneId != SceneId::FileTransfer) {
        SCENES.waitFlushIdle();
        gNapScene = gCurrentSceneId;
        gSleepPosterPreview = true;
        gNapOnUsb = usbHostConnected();
        snapshotNapRevisions();
        gNapChangeAtMs = 0;
        gNapCleanAtMs = 0;
        gNapPosterUpdates = 0;
        Sleep::drawSleepScreenNow(gfx, /*napping=*/false);
        SCENES.setPaused(true);
        gNapping = true;
      }
      continue;
    }
    if (!strncmp(line, "napafter ", 9) || !strncmp(line, "offafter ", 9)) {
      // Bench: short idle timers, in seconds (0 = back to the build default).
      const uint32_t s = strtoul(line + 9, nullptr, 10) * 1000UL;
      if (line[0] == 'n') gNapAfterMsOverride = s; else gOffAfterMsOverride = s;
      Serial.printf("[xphone-os] devcon: %s %lu ms\n", line[0] == 'n' ? "napafter" : "offafter", (unsigned long)s);
      continue;
    }
    if (!strcmp(line, "quietrestart")) {
      // Bench: the restart that ends a sync, on the current scene.
      Serial.println("[xphone-os] devcon: quietrestart");
      quietRestartToScene(static_cast<uint32_t>(gCurrentSceneId));
    }
    if (!strcmp(line, "wakeboot")) {
      // Bench: restart as if waking from deep sleep (no splash), leaving the
      // glass as it is, the way the sleep poster would be left.
      Serial.println("[xphone-os] devcon: wakeboot (quiet wake, no splash)");
      SCENES.waitFlushIdle();
      input.suspendTask();
      armBenchQuietWake();
      esp_restart();
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
    if (!strcmp(n, "power")) {  // synthetic power press: tap = 100 ms, or "btn power down <ms>"
      input.injectPower(holdMs ? holdMs : 100);
      Serial.printf("[xphone-os] devcon: btn power %lums\n", (unsigned long)(holdMs ? holdMs : 100));
      continue;
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

// Poll only control input during blocking HTTP work. Never call a scene or
// tear a server down under its own callback. Back is latched by the input
// task, so even a short press between network chunks is kept.
bool transfer_sync::pollControls() {
  static uint32_t lastPoll = 0;
  if (millis() - lastPoll >= 20) {
    lastPoll = millis();
    pumpDevConsole();
    input.update();
    if (input.wasPressed(Btn::Back) || input.wasLongPressed(Btn::Back)) requestCancel();
  }
  return cancelRequested();
}

// Loop task stack: 10 KB, not the core's 8 KB (2026-09-07). The probe above
// showed the task at 1756 B free in normal running (BLE start, the connect
// handling, and the reader.progress send each step it down), the transfer
// path 1 KB deeper before the BLE shutdown, and WiFi.mode(STA) taking the
// last 500 B to 48 B. Two "Stack protection fault" panics in one night
// were interrupts landing on those 48 B. Costs 2 KB of heap for ever.
SET_LOOP_TASK_STACK_SIZE(10 * 1024)

void loop() {
  stackProbe("loop");  // a new low anywhere else gets this name; the transfer path names its own steps
  stallwatch::beat();       // "the loop is alive"; a stuck loop stops ticking
  pumpDevConsole();         // bench-only: serial "btn X" -> synthetic taps
  input.update();           // debounced button edges (SDK InputManager)
  if (transfer_sync::active()) {
    // Static sync owns the glass. USB and physical cancel remain active;
    // no power/sleep paint or queued BLE scene switch may borrow its RAM.
    SCENES.loop(input, gfx);
    reportRuntimeStats();
    delay(10);
    return;
  }
  checkPowerButton();       // press+release -> deep sleep; hold ~2.5s -> restart
  checkAutoSleep();         // M4: idle -> deep sleep (2 min window while a block is active)
  pumpCompanionEvents();         // BLE/ANCS: parse queued payloads, set dirty flags
  COMPANION_BLE.tickAdvPolicy();  // M2.1b: fast->slow advertising demotion
  COMPANION_BLE.tickAdvWatchdog();  // 0.7: a silent radio heals itself (restart adv -> controller -> reboot)
  bool sceneLoop = true;
  if (gNapping) {
    if (gCurrentSceneId != gNapScene) {
      gNapping = false;  // a phone-started session switched scenes; it paints itself
      gSleepPosterPreview = false;
      SCENES.setPaused(false);
    } else {
      // Only the power button wakes a nap (Andrew, 2026-09-05): one rule for
      // both rest states, no bag wakes from the face buttons, and no
      // swallowed press. Face buttons do nothing here — EXCEPT this pair,
      // which stays fully asleep (no gNapping/sceneLoop change, no wake):
      // Right shows the user's /background.bmp full-screen in place of the
      // normal sleep face; Left reverts. The choice persists (Sleep::
      // setNapBackgroundOverride), so it's also what the NEXT sleep (nap or
      // OFF) starts on. waitFlushIdle first: the SD card and the panel share
      // the SPI bus, and the redraw reads the card (refreshNapPoster waits
      // for the same reason).
      sceneLoop = false;  // sleep screen on the glass: no input, no repaints; the radio and the naps go on
      // Tap OR long press: a tap only counts if the button is RELEASED under
      // 550 ms (Input::kLongPressMs), and someone poking a button that looks
      // dead holds it longer than that — which would report only the
      // long-press edge and do nothing at all.
      const bool rightPress = input.wasPressed(Btn::Right) || input.wasLongPressed(Btn::Right);
      const bool leftPress = input.wasPressed(Btn::Left) || input.wasLongPressed(Btn::Left);
      if (rightPress) {
        SCENES.waitFlushIdle();
        if (Sleep::setNapBackgroundOverride(true)) {
          // First show of a given image reads and dithers it whole off the
          // card — seconds of a frozen-looking glass without a frame saying
          // so. A cache hit is just a blit, so skip the extra refresh there.
          if (!BackgroundImage::cached(gfx.width(), gfx.height())) Sleep::drawNapBackgroundLoading(gfx);
          Sleep::drawSleepScreenNow(gfx, /*napping=*/true);
        }
      } else if (leftPress && Sleep::napShowingBackground()) {
        SCENES.waitFlushIdle();
        Sleep::setNapBackgroundOverride(false);
        Sleep::drawSleepScreenNow(gfx, /*napping=*/true);
      }
      pumpNapPoster();    // a fresh phone snapshot redraws the poster with one quiet FAST
      if (gNapCleanAtMs && static_cast<long>(millis() - gNapCleanAtMs) >= 0) {
        // The X4's quiet clean: the framebuffer still holds the poster (or a
        // newer one from a live update); rewrite it once with the HALF.
        gNapCleanAtMs = 0;
        const unsigned long t0 = millis();
        SCENES.flushFramebufferNow(gfx, SceneManager::NowTier::Half);
        Serial.printf("[xphone-os] nap: quiet clean (%lu ms)\n", millis() - t0);
      }
    }
  }
  if (sceneLoop) SCENES.loop(input, gfx);  // handle input; repaint only when a scene is dirty
  if (transfer_sync::active()) { reportRuntimeStats(); delay(10); return; }
  reportRuntimeStats();          // M2.1d: 60s stack/heap/ANCS-queue audit line
  powerbench::pump();            // lane 9: gauge sampler, no-op unless armed
  if (gBleDropAtMs && static_cast<int32_t>(millis() - gBleDropAtMs) >= 0) {
    gBleDropAtMs = 0;
    COMPANION_BLE.dropLinks();
  }
  // Advertising watchdog stage 3: a reboot, but never under a reader or a
  // transfer session, and never mid-flush.
  if (COMPANION_BLE.advWatchdogWantsReboot() && gCurrentSceneId != SceneId::Reader &&
      gCurrentSceneId != SceneId::FileTransfer && !SCENES.flushInFlight()) {
    COMPANION_BLE.advWatchdogRebootNow();
  }
#if XP_LIGHT_SLEEP_LIBS
  lightSleepTick();
  if (input.powerPressed()) input.noteActivity();
  // P4: 50 ms loop slices once idle, so the idle task gets room to sleep.
  delay((input.msSinceActivity() > 1000 && !SCENES.flushInFlight()) ? Input::idleSampleMs() : 10);
#else
  delay(10);                     // 10ms poll cadence — no periodic redraws
#endif
}
