#pragma once

// xphone-os M3 — Block: trigger iOS Screen Time shields on the paired iPhone
// through the companion BLE protocol. First real app of M3.
//
// Faithful port of CrossPoint's BlockActivity (x4-os
// src/activities/companion/BlockActivity.{h,cpp}) onto the xphone scene
// framework, with the wire protocol kept byte-identical (block.start /
// block.break / block.stop / block.status commands, "block-status" card) so
// the existing iOS companion app works unchanged.
//
// Controls are redesigned for the X3's physical layout (no side buttons):
//   MAIN  — top-LEFT (Btn::Up) = duration −10 min, top-RIGHT (Btn::Down) =
//           +10 min (visualised as −/+ tabs on the LEFT/RIGHT screen edges,
//           styled like the soft-key tabs); soft keys BACK / START / MODE
//           when idle, BACK / − / BREAK when a block is active (BREAK sits
//           on the same physical button as MODE; the break chooser's "Stop
//           now" is the only stop path, as in CrossPoint).
//   MODES — front-LEFT (Btn::Left) moves the selection UP, front-RIGHT
//           (Btn::Right) moves DOWN (top-edge pair mirrors); CONFIRM selects
//           the preset and returns to MAIN. Soft keys BACK/SELECT/UP/DOWN.
//   BREAK — CrossPoint's "Take a break?" chooser (Keep blocking / 5 min
//           break / Stop now), same navigation as MODES.
//
// One view state machine inside a single static scene (no scene stack), same
// pattern as SettingsScene. Renders only when dirty; main.cpp marks the scene
// dirty when the companion card revision changes and this scene is on glass.
// Bounded state only: mode/break lists are fixed constexpr arrays (CrossPoint
// caps: 4 presets, 3 break choices), messages are static string literals.

#include "../Scene.h"

struct CompanionCardState;

class BlockScene : public Scene {
 public:
  void onEnter() override;
  void handleInput(Input& in) override;
  void render(Gfx& gfx) override;
  const char* const* softKeys() const override;

  // Quick-action entry point: force the Deep Work preset at its default
  // duration and fire block.start immediately — same effect as opening the
  // app and pressing START. Called by the launcher's top-right long-press
  // (see showBlockDeepWork() in AppScenes). Must run AFTER switchTo/onEnter.
  void startDeepWork();

  static constexpr int MODE_COUNT = 4;   // CrossPoint blockModes cap
  static constexpr int BREAK_COUNT = 3;  // CrossPoint breakChoices cap

 private:
  enum class View : uint8_t { Main, Modes, Break };
  // In-flight command whose outcome the phone hasn't confirmed yet. The iOS
  // app pushes a fresh block-status card after every block.* command
  // (BluetoothManager.swift:800 -> sendBlockResult:131), and render()
  // resolves the transient from that card's actual content. If the push is
  // delayed/lost, a ~4s timeout in handleInput() falls through to the
  // optimistic state so the UI is never stuck on "Starting...".
  enum class Pending : uint8_t { None, Start, Stop, Break };

  void adjustDuration(int delta);
  void startBlock();
  void confirmBreakChoice();
  void setPending(Pending p);
  void latchEnd(int minutes);               // local countdown anchor
  int remainingNowMin(uint32_t now) const;  // rollover-safe signed diff
  void tickTransients(uint32_t now);        // timeout + minute tick + zero-confirm

  void renderHeader(Gfx& gfx) const;
  void renderReady(Gfx& gfx, const CompanionCardState& card) const;
  void renderActive(Gfx& gfx, const CompanionCardState& card);
  void renderModes(Gfx& gfx) const;
  void renderBreak(Gfx& gfx) const;
  void drawLock(Gfx& gfx, int cx, int topY, bool locked) const;

  View _view = View::Main;
  int _modeSel = 0;
  int _breakSel = 0;
  int _durationMin = 30;
  bool _durationCustomized = false;
  // DISPLAY state captured by render() so handleInput()/softKeys() never
  // copy the (heap-string) card on the 10 ms input tick. Fresh by
  // construction: every card revision change re-renders this scene while it
  // is on glass (main.cpp pumpCompanionEvents). True when the ACTIVE view is
  // shown: card-active or optimistic-active, overridden by optimistic-ready.
  bool _activeCache = false;
  const char* _localMsg = "Start a phone block.";  // static literals only

  // Transient/optimistic machine (fix for stuck "Starting..."):
  Pending _pending = Pending::None;
  uint32_t _pendingAtMs = 0;
  uint32_t _seenCardRevision = 0;  // card.revision last consumed by render()
  bool _optimisticActive = false;  // show active before the phone confirms
  bool _optimisticReady = false;   // show ready before the phone confirms

  // Local countdown, zero radio cost: _endMs latched from the card's
  // remainingMinutes (phone stays authoritative — corrected on every fresh
  // card) or from the optimistic start. A once-per-minute dirty mark in
  // handleInput() repaints only while this scene is on glass.
  uint32_t _endMs = 0;
  bool _endValid = false;
  int _shownRemaining = -1;       // minutes on glass; repaint when it changes
  bool _zeroConfirmSent = false;  // block.status sent once at local zero
  uint32_t _zeroConfirmAtMs = 0;
  int16_t _wCache = 0;  // panel width cached by render() (header dirty rect)
};
