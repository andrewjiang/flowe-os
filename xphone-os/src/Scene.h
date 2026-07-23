#pragma once

// xphone-os M1 — scene base class + purpose-built scene manager.
//
// One active scene, no stack, no FreeRTOS render task. E-ink discipline:
// scenes render ONLY when dirty (state changed), never periodically. M2.1a
// refresh policy (SceneManager::renderIfDirty): FULL on scene switches,
// PARTIAL window for small dirty rects (scenes report them via
// markDirty(rect)), FAST full-panel otherwise, and a HALF scrub after every
// kScrubAfterRefreshes differential updates to clear ghosting. All scenes are
// static instances — no heap.

#include "Gfx.h"
#include "Input.h"

class Scene {
 public:
  // M2.1: height (logical px) reserved at the BOTTOM of every scene for the
  // persistent soft-key bar drawn by SceneManager. Scenes must keep their
  // content above gfx.height() - SOFTKEY_BAR_H.
  static constexpr int SOFTKEY_BAR_H = 44;

  virtual void onEnter() {}
  virtual void onExit() {}

  // Four soft-key labels, left-to-right aligned with the 4 physical
  // bottom-front buttons (SDK ladder order: Back=0, Confirm=1, Left=2,
  // Right=3 — the same fixed map as Input.h). nullptr = tab hidden. Labels
  // must be static strings; they are read at render time by SceneManager.
  // Default chrome is just "BACK" (Placeholder/About behavior).
  virtual const char* const* softKeys() const;
  // M3: bitmask of soft-key slots (bit i = front button i, Back=0..Right=3)
  // that respond to a LONG PRESS. SceneManager marks those tabs with a small
  // dot near the tab's top edge. Bit 0 is set by default for every scene:
  // long-press BACK always jumps to the launcher (OS-wide convention,
  // enforced in SceneManager::loop — scenes cannot override it).
  virtual uint8_t longPressSlots() const { return 0x01; }
  // Bitmask of soft-key slots rendered as a small GEAR icon in a half-width tab
  // (right-aligned within the slot, so it shrinks toward the neighbouring tab)
  // instead of a text label. Used for the launcher's Settings key. Default: none.
  virtual uint8_t softKeyIconMask() const { return 0; }
  // React to edges from `in`; call markDirty() (or SceneManager::switchTo via
  // the AppScenes helpers) on state change. Never draws. Taps arrive via
  // in.wasPressed(), long-presses via in.wasLongPressed() (see Input.h; a
  // long-press BACK never reaches the scene).
  virtual void handleInput(Input& in) = 0;
  // Compose the full scene into the framebuffer (already cleared to white).
  virtual void render(Gfx& gfx) = 0;

  bool isDirty() const { return _dirty; }
  // Full-panel dirty (unknown/most of the screen changed).
  void markDirty() {
    _dirty = true;
    _dirtyAll = true;
  }
  // M2.1a: dirty with a LOGICAL rect — SceneManager may refresh only that
  // window. Rects union into a single accumulator; a plain markDirty() (or an
  // empty rect) forces full-panel.
  void markDirty(const XpRect& r) {
    _dirty = true;
    if (r.empty()) {
      _dirtyAll = true;
      return;
    }
    if (!_dirtyAll) _dirtyRect.unionWith(r);
  }
  void clearDirty() {
    _dirty = false;
    _dirtyAll = false;
    _dirtyRect = XpRect{};
  }
  bool dirtyAll() const { return _dirtyAll; }
  const XpRect& dirtyRect() const { return _dirtyRect; }

 protected:
  ~Scene() = default;  // scenes are static, never deleted via base pointer

 private:
  bool _dirty = true;
  bool _dirtyAll = true;  // matches _dirty's boot state: first paint is full
  XpRect _dirtyRect;
};

// M2.1a instrumentation: snapshot of the last flush, written by SceneManager
// after every repaint and surfaced on the About scene (the device has no
// serial cable attached in normal use — About IS the readout). Heap-free:
// tier is a static string literal.
struct RefreshStats {
  unsigned long drawMs = 0;     // scene render into the framebuffer
  unsigned long refreshMs = 0;  // display flush (SPI + waveform)
  const char* tier = "-";       // FULL / HALF / FAST / PARTIAL
  uint8_t sinceScrub = 0;       // FAST+PARTIAL refreshes since last FULL/HALF
};
extern RefreshStats gRefreshStats;

// Ghost-scrub cadence: force a full-panel HALF scrub after this many
// differential (FAST/PARTIAL) refreshes. Evidence in Scene.cpp's policy
// comment; surfaced here so About can show it next to the rolling count.
constexpr uint8_t kScrubAfterRefreshes = 10;

class SceneManager {
 public:
  // Exit the old scene, enter the new one; next render uses a FULL refresh.
  void switchTo(Scene& s);

  // One tick: OS-wide long-press BACK -> launcher, else forward input to the
  // active scene; then repaint if dirty. (Defined in Scene.cpp: it needs the
  // AppScenes navigation helpers.)
  void loop(Input& in, Gfx& gfx);

  // Render + flush when the active scene is dirty (also used once at boot).
  void renderIfDirty(Gfx& gfx);

  // M2: lets AppScenes' markXxxDirtyIfActive helpers target only the scene
  // that is actually on glass.
  Scene* active() const { return _active; }

  // M5 responsiveness (Phase 2): true while the flush worker is driving the
  // panel. renderIfDirty() defers composing while set (state keeps advancing;
  // the next compose shows the newest state — natural coalescing).
  bool flushInFlight() const { return _flushInFlight; }
  // Block until the worker is idle. MUST be called before any direct
  // gfx.flush()/panel teardown outside the worker (Sleep::sleepNow, restart).
  void waitFlushIdle() const;

 private:
  Scene* _active = nullptr;
  bool _needFull = false;
  // M2.1a refresh discipline state.
  uint8_t _sinceScrub = 0;   // FAST/PARTIAL refreshes since the last FULL/HALF
  uint8_t _bootFlushes = 0;  // full-panel flushes since boot (panel conditioning gate)

  // M5 Phase 2 — flush worker: the loop task composes the frame (stores are
  // main-loop-only), then hands the BLOCKING flush to this task so the loop
  // keeps pumping input/BLE during the whole e-ink waveform (~0.45–3.2 s).
  enum class FlushReq : uint8_t { Full, Half, Fast, Window, Flash };
  void ensureFlushTask(Gfx& gfx);
  void dispatchFlush(FlushReq req, const XpRect& rect);
  static void flushTrampoline(void* self);
  [[noreturn]] void flushWorkLoop();
  TaskHandle_t _flushTask = nullptr;
  Gfx* _flushGfx = nullptr;
  volatile bool _flushInFlight = false;
  FlushReq _flushReq = FlushReq::Fast;  // written by loop before notify only
  XpRect _flushRect;                    // Window params
  unsigned long _composeMs = 0;         // drawMs for the stats line
};

extern SceneManager SCENES;
