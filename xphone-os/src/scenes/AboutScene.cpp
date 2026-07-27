#include "AboutScene.h"

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <BoardConfig.h>

#include <cstdio>

#include "../BatteryGauge.h"
#include "../ClockStore.h"
#include "../Fonts.h"
#include "../NotificationStore.h"
#include "../Sleep.h"
#include "../ble/CompanionAncsClient.h"
#include "../ble/CompanionBleService.h"
#include "AppScenes.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

void AboutScene::handleInput(Input& in) {
  if (in.wasPressed(Btn::Back)) showLauncher();
}

void AboutScene::render(Gfx& gfx) {
  // Values are sampled here, once per entry — the scene renders only when
  // dirty, so this is a snapshot, not a live monitor (e-ink discipline).
  char line[96];
  const int x = 24;
  int y = 16;

  gfx.drawText(kFontBold, x, y, "About xphone-os");
  y += gfx.lineHeight(kFontBold) + 10;
  gfx.fillRect(x, y - 6, gfx.width() - 2 * x, 2, true);

  snprintf(line, sizeof(line), "version: %s (built %s %s)", XPHONE_VERSION, __DATE__, __TIME__);
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 4;

  snprintf(line, sizeof(line), "panel: %s  %dx%d", BoardConfig::ACTIVE.name, gfx.width(), gfx.height());
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 4;

  snprintf(line, sizeof(line), "boot to first paint: %lu ms", gBootTotalMs);
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 4;

  // M4.2 wake diagnostic: reset reason + last-scene restore outcome, captured
  // once in boot(). Tells us whether the power-button wake is DEEPSLEEP (RTC
  // would survive) or POWERON (needs NVS), and whether a scene was restored.
  snprintf(line, sizeof(line), "wake: %s  restore: %s", gWakeResetReason, gWakeRestoreScene);
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 4;

  // Phase 0 morning-meditation spec: wake→BLE-connect and wake→date latency
  // (millis() starts ~0 at boot, so the stamps ARE the delays). This readout
  // is the experiment — no serial cable needed.
  if (CLOCK_STORE.firstSyncMs != 0) {
    snprintf(line, sizeof(line), "time.sync: ble %lu ms  date %lu ms  %02u:%02u",
             static_cast<unsigned long>(CLOCK_STORE.firstConnectMs),
             static_cast<unsigned long>(CLOCK_STORE.firstSyncMs),
             static_cast<unsigned>(CLOCK_STORE.minutesIntoDay / 60),
             static_cast<unsigned>(CLOCK_STORE.minutesIntoDay % 60));
  } else if (CLOCK_STORE.firstConnectMs != 0) {
    snprintf(line, sizeof(line), "time.sync: ble %lu ms  date pending",
             static_cast<unsigned long>(CLOCK_STORE.firstConnectMs));
  } else {
    snprintf(line, sizeof(line), "time.sync: no connect since boot");
  }
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 4;

  snprintf(line, sizeof(line), "heap free: %u B", static_cast<unsigned>(ESP.getFreeHeap()));
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 4;

  snprintf(line, sizeof(line), "heap min free: %u B",
           static_cast<unsigned>(esp_get_minimum_free_heap_size()));
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 4;

  snprintf(line, sizeof(line), "heap largest block: %u B",
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 10;

  // --- M2.1a: refresh instrumentation — the LAST interaction before entering
  // About (entering About is itself a FULL scene-switch refresh that
  // overwrites gRefreshStats after this render, so what is on glass here is
  // the interaction just before the switch). No serial cable needed: this IS
  // the readout. ------------------------------------------------------------
  gfx.fillRect(x, y - 6, gfx.width() - 2 * x, 2, true);

  snprintf(line, sizeof(line), "last draw: %lu ms", gRefreshStats.drawMs);
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 4;

  snprintf(line, sizeof(line), "last refresh: %lu ms (%s)", gRefreshStats.refreshMs, gRefreshStats.tier);
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 4;

  snprintf(line, sizeof(line), "partials since scrub: %u  (HALF every %u)",
           static_cast<unsigned>(gRefreshStats.sinceScrub), static_cast<unsigned>(kScrubAfterRefreshes));
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 10;

  // --- M2.1b: power instrumentation — About IS the meter readout for the
  // power levers (uptime + gauge average current = drain per configuration;
  // adv mode + conn interval = what the radio is actually doing). -----------
  gfx.fillRect(x, y - 6, gfx.width() - 2 * x, 2, true);

  snprintf(line, sizeof(line), "cpu: %lu MHz  uptime: %lu min",
           static_cast<unsigned long>(getCpuFrequencyMhz()), static_cast<unsigned long>(millis() / 60000UL));
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 4;

  {
    // BoardConfig-driven backend: X3 = BQ27220 I2C gauge, X4 = ADC divider.
    static BatteryMonitor battery;
    const BatteryMonitor::Status batt = battery.readStatus();
    int16_t avgMa = 0;
    const bool hasAvg = BatteryGauge::readAvgCurrentMa(avgMa);
    if (batt.supported && batt.percentageKnown && batt.millivoltsKnown) {
      if (hasAvg) {
        snprintf(line, sizeof(line), "battery: %u%%  %u mV  avg %d mA", batt.percentage, batt.millivolts, avgMa);
      } else {
        snprintf(line, sizeof(line), "battery: %u%%  %u mV", batt.percentage, batt.millivolts);
      }
    } else {
      snprintf(line, sizeof(line), "battery: unavailable");
    }
  }
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 4;

  {
    // M2.1b follow-up: gauge capacity readout — remaining / full-charge /
    // design, all mAh (BQ27220 std commands 0x10 / 0x12 / 0x3C). This is the
    // on-glass check that the boot-time design-capacity reprogram took (see
    // BatteryGauge::ensureDesignCapacity): design must read 650, not the
    // factory 3000. X3 only — on X4 (ADC path) readWord() returns false and
    // the line is skipped, leaving the existing battery lines unchanged.
    uint16_t remainMah = 0, fccMah = 0, designMah = 0;
    if (BatteryGauge::readWord(BatteryGauge::kCmdRemainingCapacity, remainMah) &&
        BatteryGauge::readWord(BatteryGauge::kCmdFullChargeCapacity, fccMah) &&
        BatteryGauge::readWord(BatteryGauge::kCmdDesignCapacity, designMah)) {
      snprintf(line, sizeof(line), "gauge: %u/%u mAh  design %u mAh", remainMah, fccMah, designMah);
      gfx.drawText(kFontRegular, x, y, line);
      y += gfx.lineHeight(kFontRegular) + 4;
    }
  }

  if (!COMPANION_BLE.isStarted()) {
    snprintf(line, sizeof(line), "adv: off");
  } else if (COMPANION_BLE.isConnected()) {
    snprintf(line, sizeof(line), "adv: stopped (connected)");
  } else if (COMPANION_BLE.getAdvMode() == CompanionBleService::AdvMode::Fast) {
    snprintf(line, sizeof(line), "adv: fast (30-60 ms window)");
  } else {
    snprintf(line, sizeof(line), "adv: slow (400-500 ms)");
  }
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 4;

  {
    uint16_t itvl125 = 0, latency = 0, timeout10ms = 0;
    if (COMPANION_BLE.getConnParams(itvl125, latency, timeout10ms)) {
      const unsigned itvlUs = static_cast<unsigned>(itvl125) * 1250u;  // 1.25 ms units
      snprintf(line, sizeof(line), "conn: %u.%02u ms  lat=%u  timeout=%u ms", itvlUs / 1000u,
               (itvlUs % 1000u) / 10u, latency, static_cast<unsigned>(timeout10ms) * 10u);
    } else {
      snprintf(line, sizeof(line), "conn: none");
    }
  }
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 10;

  // --- M2: companion BLE / ANCS snapshot (same e-ink discipline: sampled
  // once per render, no live polling) --------------------------------------
  gfx.fillRect(x, y - 6, gfx.width() - 2 * x, 2, true);

  if (!COMPANION_BLE.isStarted()) {
    snprintf(line, sizeof(line), "ble: off");
  } else if (COMPANION_BLE.isConnected()) {
    snprintf(line, sizeof(line), "ble: connected");
  } else {
    snprintf(line, sizeof(line), "ble: advertising as %s", CompanionProtocol::DEVICE_NAME);
  }
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 4;

  char peer[18];
  if (COMPANION_BLE.getPeerAddress(peer, sizeof(peer))) {
    snprintf(line, sizeof(line), "peer: %s%s", peer, COMPANION_BLE.isConnected() ? "" : " (last)");
  } else {
    snprintf(line, sizeof(line), "peer: none since boot");
  }
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 4;

  // ANCS status snapshot (fixed-buffer copy under the client's state mutex).
  char ancsStatus[48];
  COMPANION_ANCS.getStatusMessage(ancsStatus, sizeof(ancsStatus));
  snprintf(line, sizeof(line), "ancs: %s", ancsStatus);
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 4;

  snprintf(line, sizeof(line), "notifications stored: %u",
           static_cast<unsigned>(NOTIFICATION_STORE.count()));
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 4;

  // M2.1d: stack + ANCS-queue audit on glass, so hardware validation needs
  // no serial cable. HWM values are bytes (ESP-IDF StackType_t is uint8_t).
  {
    const UBaseType_t loopHwm = uxTaskGetStackHighWaterMark(nullptr);
    const TaskHandle_t bleTask = COMPANION_ANCS.getHostTaskHandle();
    if (bleTask) {
      snprintf(line, sizeof(line), "stack HWM: loop %u B  ble %u B  ancs q peak %u/6 drops %lu",
               static_cast<unsigned>(loopHwm), static_cast<unsigned>(uxTaskGetStackHighWaterMark(bleTask)),
               COMPANION_ANCS.getQueueHighWater(),
               static_cast<unsigned long>(COMPANION_ANCS.getQueueDropCount()));
    } else {
      snprintf(line, sizeof(line), "stack HWM: loop %u B  ble n/a  ancs q peak %u/6 drops %lu",
               static_cast<unsigned>(loopHwm), COMPANION_ANCS.getQueueHighWater(),
               static_cast<unsigned long>(COMPANION_ANCS.getQueueDropCount()));
    }
  }
  gfx.drawText(kFontRegular, x, y, line);
  y += gfx.lineHeight(kFontRegular) + 4;

#if XP_AUTO_SLEEP_MS > 0
  snprintf(line, sizeof(line), "Press power to sleep (auto %lu min), hold 3s to restart",
           static_cast<unsigned long>(XP_AUTO_SLEEP_MS / 60000UL));
#else
  snprintf(line, sizeof(line), "Press power to sleep, hold 3s to restart");
#endif
  gfx.drawText(kFontRegular, x, y, line);
  // M2.1: footer hint replaced by the SceneManager soft-key bar (default BACK tab).
}
