#pragma once

// xphone-os — Settings: SD firmware update, icon-style A/B, restart, About.
//
// Single Scene object; sub-views are internal states (no scene stack). Visual
// design mirrors CrossPoint's settings list: full-width inverted selector bar,
// label left / value right, header with title left / version right.

#include "../Scene.h"

class SettingsScene : public Scene {
 public:
  void onEnter() override;
  void handleInput(Input& in) override;
  void render(Gfx& gfx) override;
  const char* const* softKeys() const override;

  static constexpr int MAX_BIN_FILES = 16;
  static constexpr size_t MAX_NAME_LEN = 64;

 private:
  enum class View : uint8_t { Menu, Picker, ConfirmFlash, ConfirmRestart, IconStyle };

  struct BinFile {
    char name[MAX_NAME_LEN + 1];
    uint32_t size = 0;
  };

  void enterPicker();
  void enterIconStyle();
  void scanBinFiles();
  void doFlash();
  void moveSel(int& sel, int count, int delta);
  void cycleIconPack(int delta);

  void renderMenu(Gfx& gfx);
  void renderPicker(Gfx& gfx);
  void renderConfirmFlash(Gfx& gfx);
  void renderConfirmRestart(Gfx& gfx);
  void renderIconStyle(Gfx& gfx);
  void drawHeader(Gfx& gfx, const char* title, const char* right);
  void drawRow(Gfx& gfx, int y, int rowH, const char* label, const char* value, bool selected);

  View _view = View::Menu;
  int _menuSel = 0;
  int _pickSel = 0;
  int _fileCount = 0;
  bool _sdOk = false;
  const char* _status = nullptr;
  BinFile _files[MAX_BIN_FILES];

  Gfx* _gfx = nullptr;
};
