#pragma once

// xphone-os — launcher: status bar + 3x2 app grid (Notifications/Read/Today/
// Priorities/Block/Workout). Settings opens from the BACK soft-key; About
// lives in Settings. Icon bitmaps come from IconStyle (multi-pack).

#include "../Scene.h"

class LauncherScene : public Scene {
 public:
  void handleInput(Input& in) override;
  void render(Gfx& gfx) override;
  const char* const* softKeys() const override;  // [gear] / OPEN / PREV / NEXT
  uint8_t softKeyIconMask() const override { return 0x01; }  // Settings = gear tab

  static constexpr int COLS = 2;
  static constexpr int APP_COUNT = 6;

  // Bench dev console ("where"): expose the grid selection so remote
  // navigation is fact-based, never dead reckoning.
  int selection() const { return _sel; }

 private:
  void moveSelection(int dCol, int dRow);
  // Logical rect of grid cell i (tile + label, small slop), from the layout
  // cached by render(). Empty rect until the first render.
  XpRect cellRect(int i) const;

  int _sel = 0;  // persists across scene switches (static instance)

  // M2.1a: grid layout cached by render() so moveSelection() can dirty only
  // the two affected cells (soft-key labels are static on the launcher).
  int16_t _gridX = 0, _gridY = 0, _side = 0, _cellH = 0;
};
