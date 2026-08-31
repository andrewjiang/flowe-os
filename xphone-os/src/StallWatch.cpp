#include "StallWatch.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>

namespace stallwatch {
namespace {

constexpr size_t kStageCap = 56;
// Below this a "stall" is just an e-ink refresh or a book being indexed,
// both of which legitimately hold the loop for a second or two. Five
// seconds is past anything healthy and well short of the point where an
// owner reaches for the reset button.
constexpr uint32_t kReportMs = 5000;
constexpr uint32_t kMagic = 0x5354414C;  // "STAL"

// RTC memory survives a software reset and the reset button; only pulling
// the power clears it. NOINIT means the startup code leaves it alone, which
// is the entire point — the record has to outlive the reset that ends the
// freeze.
struct Record {
  uint32_t magic;
  uint32_t stalledMs;
  char stage[kStageCap];
};
RTC_NOINIT_ATTR Record gRecord;

// Two buffers and an index, so the watcher never reads a half-written name.
// The writer fills the spare one and then flips; a reader that catches the
// flip mid-way sees the previous name, which is still true and still useful.
char gStageBuf[2][kStageCap];
volatile uint8_t gStageIdx = 0;
volatile uint32_t gBeat = 0;
volatile uint32_t gWorstMs = 0;
bool gStarted = false;

StaticTask_t gTcb;
StackType_t gStack[2048];

void watchTask(void*) {
  uint32_t lastBeat = gBeat;
  uint32_t movedAt = millis();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    const uint32_t beat = gBeat;
    const uint32_t now = millis();
    if (beat != lastBeat) {
      lastBeat = beat;
      movedAt = now;
      continue;
    }
    const uint32_t stalled = now - movedAt;
    if (stalled < kReportMs) continue;
    if (stalled <= gWorstMs) continue;  // already recorded, and worse
    gWorstMs = stalled;
    gRecord.magic = kMagic;
    gRecord.stalledMs = stalled;
    snprintf(gRecord.stage, sizeof(gRecord.stage), "%s", gStageBuf[gStageIdx]);
  }
}

}  // namespace

void begin() {
  if (gStarted) return;
  gStarted = true;
  gStageBuf[0][0] = '\0';
  gStageBuf[1][0] = '\0';
  snprintf(gStageBuf[0], kStageCap, "%s", "starting up");
  // Priority 2: above the main loop (1), so it still runs while the loop is
  // stuck. A static stack keeps it out of the heap the reader is competing
  // for, the same reason the flush task uses one.
  xTaskCreateStatic(&watchTask, "xp_stall", sizeof(gStack) / sizeof(StackType_t), nullptr, 2,
                    gStack, &gTcb);
}

void beat() { gBeat++; }

void stage(const char* what) {
  if (!what) return;
  const uint8_t spare = gStageIdx ^ 1;
  snprintf(gStageBuf[spare], kStageCap, "%s", what);
  gStageIdx = spare;
}

uint32_t worstStallMs() { return gWorstMs; }

bool takeReport(char* out, size_t outCap) {
  if (!out || outCap == 0) return false;
  // On a cold power-up this is uninitialised RAM, so check the shape of it,
  // not just the magic: a printable name and a believable duration.
  const bool sane = gRecord.magic == kMagic && gRecord.stalledMs >= kReportMs &&
                    gRecord.stalledMs < 24UL * 60 * 60 * 1000;
  if (!sane) {
    gRecord.magic = 0;
    return false;
  }
  gRecord.stage[kStageCap - 1] = '\0';
  for (size_t i = 0; gRecord.stage[i]; i++) {
    const unsigned char c = static_cast<unsigned char>(gRecord.stage[i]);
    if (c < 0x20 || c > 0x7E) {
      gRecord.magic = 0;
      return false;
    }
  }
  snprintf(out, outCap, "stalled %lu ms in: %s", static_cast<unsigned long>(gRecord.stalledMs),
           gRecord.stage[0] ? gRecord.stage : "(unnamed step)");
  gRecord.magic = 0;
  return true;
}

}  // namespace stallwatch
