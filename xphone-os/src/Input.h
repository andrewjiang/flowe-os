#pragma once

// xphone-os M1 — logical button input.
//
// Thin wrapper over the FreeInk SDK InputManager (XteinkAdcLadder style on
// X3/X4: the 4 bottom-front buttons are a resistor ladder on ADC GPIO1, the
// pair on the TOP edge (held portrait) a second ladder on ADC GPIO2 —
// InputManager.cpp ADC_RANGES_1/2; the SDK calls that pair BTN_UP/BTN_DOWN).
// Fixed default mapping, no settings system: logical == the SDK's physical
// indices, same as x4-os CrossPointSettings defaults (FRONT_HW_BACK=0,
// CONFIRM=1, LEFT=2, RIGHT=3; the top pair Up=4 / Down=5 is always fixed —
// in x4-os default Portrait MappedInputManager does not remap any of them).
//
// M3 tap vs long-press: a per-button state machine on top of the SDK's
// debounced level state (isPressed). Timestamp latched on the press edge;
// holding past kLongPressMs fires wasLongPressed() ONCE (no repeat) while the
// button is still down; releasing before the threshold fires wasPressed() —
// i.e. a TAP is reported on RELEASE, not on the press edge, which is what
// lets a long-press consume the hold. The POWER button is deliberately
// excluded: it is owned by the hold-to-restart handler in main.cpp
// (powerPressed()/powerHeldMs()).
//
// M5 responsiveness (docs/x3-responsiveness-plan.md Phase 1): sampling runs
// on a DEDICATED FreeRTOS task every 5 ms (beginTask()), so presses register
// even while the loop task is busy (e-ink flushes used to eat any tap that
// started AND ended inside their 0.45–3.2 s window — SDK events are one-shot
// and were simply never seen). Events LATCH into pending bitmasks and are
// drained by update() on the main loop, so nothing is lost between ticks.
// Verified safe on the single-core C3: the display driver's busy-wait yields
// every 1 ms (EpdBus::waitBusy delay(1)) and holds no locks, and analogRead
// goes through the IDF oneshot driver's own mutex. The InputManager state is
// task-owned once the task starts — the main loop only reads the published
// snapshots below.
//
// Call update() once per loop() tick, then query wasPressed()/
// wasLongPressed() edges (same API as always).

#include <Arduino.h>
#include <InputManager.h>

#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

enum class Btn : uint8_t { Up = 0, Down, Left, Right, Confirm, Back, COUNT };

class Input {
 public:
  // Hold time that turns a press into a long-press. ~550ms: longer than any
  // deliberate tap, shorter than the SDK's own 650ms confirm-back hold.
  static constexpr unsigned long kLongPressMs = 550;

  void begin() { _mgr.begin(); }  // pinMode + ADC attenuation per BoardConfig

  // Phase 1: start the dedicated 5 ms sampling task. After this, the SDK
  // manager is owned by that task and update() only drains latched events.
  void beginTask() {
    if (_task) return;
    xTaskCreate(&Input::taskTrampoline, "xp_input", 2560, this, 2, &_task);
  }

  // Stop sampling before deep sleep / restart so the task isn't mid-ADC-read
  // during teardown. (Wake from X3 sleep is a full power-on reset, so there
  // is no resume path — suspend is enough.)
  void suspendTask() {
    if (_task) vTaskSuspend(_task);
  }

  // Main-loop tick: drain events latched by the sampling task into this
  // tick's one-shot flags. Without the task (beginTask() not called) it
  // samples inline first — identical semantics, single code path.
  void update() {
    if (!_task) sampleOnce();
    uint8_t tap, lng;
    bool any;
    portENTER_CRITICAL(&_mux);
    tap = _pendingTap;
    lng = _pendingLong;
    any = _pendingAny;
    _pendingTap = 0;
    _pendingLong = 0;
    _pendingAny = false;
    portEXIT_CRITICAL(&_mux);
    for (uint8_t i = 0; i < kBtnCount; i++) {
      _tap[i] = tap & (1u << i);
      _long[i] = lng & (1u << i);
    }
    _anyThisTick = any;
  }

  // TAP: released before kLongPressMs (one-tick event, latched so a tap
  // during a flush fires the moment the loop next drains).
  bool wasPressed(Btn b) const { return _tap[static_cast<uint8_t>(b)]; }
  // LONG-PRESS: fired once at kLongPressMs while still held (one-tick event).
  bool wasLongPressed(Btn b) const { return _long[static_cast<uint8_t>(b)]; }

  // Debounced level, from the sampling task's published snapshot.
  bool isPressed(Btn b) const { return _levelMask & (1u << static_cast<uint8_t>(b)); }
  // Any ladder press edge since the last update() drain (idle-timer reset).
  bool wasAnyPressed() const { return _anyThisTick; }

  // Power button (digital pin per BoardConfig input.power — GPIO3 on X3/X4,
  // active-low; the SDK debounces it alongside the ladder buttons). Published
  // by the sampling task; single-word volatile reads are atomic on the C3.
  bool powerPressed() const { return _powerDown; }
  unsigned long powerHeldMs() const { return _powerHeldMs; }

 private:
  static constexpr uint8_t kBtnCount = static_cast<uint8_t>(Btn::COUNT);

  // Fixed logical -> SDK physical index map.
  static constexpr uint8_t kMap[kBtnCount] = {
      InputManager::BTN_UP,    InputManager::BTN_DOWN,    InputManager::BTN_LEFT,
      InputManager::BTN_RIGHT, InputManager::BTN_CONFIRM, InputManager::BTN_BACK,
  };

  static void taskTrampoline(void* self) { static_cast<Input*>(self)->taskLoop(); }

  [[noreturn]] void taskLoop() {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
      vTaskDelayUntil(&last, pdMS_TO_TICKS(5));
      sampleOnce();
    }
  }

  // One SDK sample + tap/long-press machine pass; results latch into the
  // pending masks (OR-accumulated until the main loop drains them).
  void sampleOnce() {
    _mgr.update();
    const unsigned long now = millis();
    uint8_t tapBits = 0, longBits = 0, levels = 0;
    for (uint8_t i = 0; i < kBtnCount; i++) {
      const bool down = _mgr.isPressed(kMap[i]);
      if (down) levels |= (1u << i);
      if (down) {
        if (!_held[i]) {  // press edge: start the hold clock
          _held[i] = true;
          _longFired[i] = false;
          _pressStartMs[i] = now;
        } else if (!_longFired[i] && now - _pressStartMs[i] >= kLongPressMs) {
          _longFired[i] = true;  // fire-once while held, no repeat
          longBits |= (1u << i);
        }
      } else if (_held[i]) {  // release edge
        _held[i] = false;
        if (!_longFired[i]) tapBits |= (1u << i);  // short hold -> tap
        // else: the long-press already consumed this hold — no tap.
      }
    }
    const bool any = _mgr.wasAnyPressed();
    portENTER_CRITICAL(&_mux);
    _pendingTap |= tapBits;
    _pendingLong |= longBits;
    if (any) _pendingAny = true;
    portEXIT_CRITICAL(&_mux);
    _levelMask = levels;
    _powerDown = _mgr.isPressed(InputManager::BTN_POWER);
    _powerHeldMs = _mgr.getPowerButtonHeldTime();
  }

  InputManager _mgr;  // task-owned once beginTask() runs
  TaskHandle_t _task = nullptr;
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

  // Tap/long-press machine state (sampling context only).
  unsigned long _pressStartMs[kBtnCount] = {};
  bool _held[kBtnCount] = {};
  bool _longFired[kBtnCount] = {};

  // Latches: sampling context -> main loop (guarded by _mux).
  uint8_t _pendingTap = 0;
  uint8_t _pendingLong = 0;
  bool _pendingAny = false;

  // Published snapshots (volatile single-word, lock-free reads).
  volatile uint8_t _levelMask = 0;
  volatile bool _powerDown = false;
  volatile unsigned long _powerHeldMs = 0;

  // This tick's drained one-shot events (main loop only).
  bool _tap[kBtnCount] = {};
  bool _long[kBtnCount] = {};
  bool _anyThisTick = false;
};
