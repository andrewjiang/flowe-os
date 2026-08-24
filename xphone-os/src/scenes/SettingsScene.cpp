#include "SettingsScene.h"

#include <SDCardManager.h>

#include "../DeviceKind.h"
#include <esp_system.h>

#include <cstdio>
#include <cstring>

#include "../Fonts.h"
#include "../IconStyle.h"
#include "../SdUpdate.h"
#include "../art/LauncherIcons.h"
#include "AppScenes.h"

// Visual constants — borrowed from CrossPoint's settings list so the two
// firmwares feel related on the same glass:
//   * header: title left + version right in one bar (x4-os
//     SettingsActivity.cpp:361-362 GUI.drawHeader(title, CROSSPOINT_VERSION)),
//     header band ~45px (BaseTheme.h:128 headerHeight = 45; ours is 46 to
//     match NotificationsScene's existing chrome).
//   * rows: fixed row height with the label at a fixed left padding and the
//     value right-aligned in the same row (BaseTheme.cpp:303-350; padding 20
//     = BaseTheme.h:132 contentSidePadding). CrossPoint's 30px rows carry a
//     10pt font; ours scale to ubuntu_12 (lineHeight + 16).
//   * selector: full-width filled black bar with the row text drawn inverted
//     (BaseTheme.cpp:296-298 fillRect + :321 drawText(..., i != selected)) —
//     not the launcher's rounded outline, so lists read like CrossPoint.
//   * paging: page = sel / perPage, list never scrolls partially
//     (BaseTheme.cpp:302 pageStartIndex = selectedIndex / pageItems * pageItems).
namespace {
constexpr int kMarginX = 20;   // = CrossPoint contentSidePadding (BaseTheme.h:132)
constexpr int kHeaderH = 46;   // ~= CrossPoint headerHeight 45 (BaseTheme.h:128)
constexpr int kRowPad = 16;    // row height = lineHeight(bold) + kRowPad


constexpr const char* kMenuLabels[5] = {"File Transfer", "SD Firmware Update", "Icon style", "Restart", "About"};
constexpr int kMenuCount = 5;

// Fixed-buffer UTF-8-safe truncation with "..." (same helper pattern as
// NotificationsScene.cpp truncateToWidth).
void truncateToWidth(Gfx& gfx, const XpFont& font, const char* src, int maxWidth, char* dst, size_t dstSize) {
  snprintf(dst, dstSize, "%s", src ? src : "");
  if (gfx.textWidth(font, dst) <= maxWidth) return;
  size_t len = strlen(dst);
  while (len > 0) {
    do {
      len--;
    } while (len > 0 && (static_cast<uint8_t>(dst[len]) & 0xC0) == 0x80);
    dst[len] = '\0';
    char probe[96];
    snprintf(probe, sizeof(probe), "%s...", dst);
    if (gfx.textWidth(font, probe) <= maxWidth) {
      snprintf(dst, dstSize, "%s", probe);
      return;
    }
  }
}

// "731.2 KB" / "1.4 MB" — integer math only.
void formatSize(uint32_t bytes, char* dst, size_t dstSize) {
  if (bytes >= 1024UL * 1024UL) {
    const uint32_t tenthsMB = (bytes + 52429UL) / 104858UL;  // bytes / (1MB/10), rounded
    snprintf(dst, dstSize, "%u.%u MB", static_cast<unsigned>(tenthsMB / 10),
             static_cast<unsigned>(tenthsMB % 10));
  } else {
    const uint32_t tenthsKB = (bytes * 10 + 512) / 1024;
    snprintf(dst, dstSize, "%u.%u KB", static_cast<unsigned>(tenthsKB / 10),
             static_cast<unsigned>(tenthsKB % 10));
  }
}

// Picker filter: firmware images on the SD root. Plain *.bin plus
// *.bin.flashed — the boot updater retires a consumed /update.bin to
// /update.bin.flashed, and re-flashing that previous image without a laptop
// is exactly what this picker is for.
bool isFirmwareName(const char* name) {
  const size_t len = strlen(name);
  auto endsWith = [&](const char* suffix) {
    const size_t sl = strlen(suffix);
    return len >= sl && strcmp(name + len - sl, suffix) == 0;
  };
  return endsWith(".bin") || endsWith(".bin.flashed");
}
}  // namespace

void SettingsScene::onEnter() {
  _view = View::Menu;
  _menuSel = 0;
  _status = nullptr;
}

const char* const* SettingsScene::softKeys() const {
  static constexpr const char* kListKeys[4] = {"BACK", "OPEN", SoftKey::Left, SoftKey::Right};
  static constexpr const char* kConfirmKeys[4] = {"NO", "YES", nullptr, nullptr};
  static constexpr const char* kIconKeys[4] = {"BACK", nullptr, SoftKey::Left, SoftKey::Right};
  if (_view == View::ConfirmFlash || _view == View::ConfirmRestart) return kConfirmKeys;
  if (_view == View::IconStyle) return kIconKeys;
  return kListKeys;
}

void SettingsScene::enterIconStyle() {
  _view = View::IconStyle;
  _status = nullptr;
  markDirty();
}

void SettingsScene::cycleIconPack(const int delta) {
  const int n = IconStyle::packCount();
  if (n <= 0) return;
  int next = static_cast<int>(IconStyle::get()) + delta;
  while (next < 0) next += n;
  while (next >= n) next -= n;
  IconStyle::set(static_cast<uint8_t>(next));  // live apply + NVS persist
  markDirty();
}

void SettingsScene::moveSel(int& sel, const int count, const int delta) {
  int next = sel + delta;
  if (next < 0) next = 0;
  if (next > count - 1) next = count - 1;
  if (count <= 0) next = 0;
  if (next != sel) {
    sel = next;
    markDirty();  // selector bar spans the width; whole list region repaints
  }
}

void SettingsScene::enterPicker() {
  _pickSel = 0;
  _fileCount = 0;
  _status = nullptr;
  // Mount on entry (the boot self-update may have mounted long ago, or there
  // was no card then — SDCardManager::begin() re-probes the card each call).
  _sdOk = SdMan.begin();
  if (_sdOk) scanBinFiles();
  _view = View::Picker;
  markDirty();
}

void SettingsScene::scanBinFiles() {
  FsFile root = SdMan.open("/", O_RDONLY);
  if (!root || !root.isDir()) {
    Serial.println("[xphone-os] settings: SD root open failed");
    _sdOk = false;
    return;
  }
  FsFile f;
  while (f.openNext(&root, O_RDONLY)) {
    if (!f.isDir()) {
      char name[MAX_NAME_LEN + 2];  // +1 slack to detect over-long names
      const size_t got = f.getName(name, sizeof(name));
      if (got == 0 || got > MAX_NAME_LEN) {
        if (got > MAX_NAME_LEN) Serial.printf("[xphone-os] settings: skipping over-long name (%u chars)\n",
                                              static_cast<unsigned>(got));
      } else if (isFirmwareName(name)) {
        if (_fileCount >= MAX_BIN_FILES) {
          Serial.printf("[xphone-os] settings: more than %d .bin files on SD root; extra ignored\n", MAX_BIN_FILES);
          f.close();
          break;
        }
        BinFile& e = _files[_fileCount++];
        snprintf(e.name, sizeof(e.name), "%s", name);
        e.size = static_cast<uint32_t>(f.fileSize());
      }
    }
    f.close();
  }
  root.close();
  Serial.printf("[xphone-os] settings: %d firmware image(s) on SD root\n", _fileCount);
}

void SettingsScene::doFlash() {
  if (_gfx == nullptr || _pickSel >= _fileCount) return;  // cannot happen: confirm was rendered via _gfx
  char path[MAX_NAME_LEN + 2];
  snprintf(path, sizeof(path), "/%s", _files[_pickSel].name);
  Serial.printf("[xphone-os] settings: flashing %s\n", path);
  // Validate + progress bar + flash to the inactive OTA slot + otadata switch
  // + esp_restart() — the same proven machinery as the boot-time /update.bin
  // path (SdUpdate). Returns only on failure (error X already drawn).
  sd_update::flashFromPath(_gfx->display(), path);
  _status = "Update failed - image left untouched";
  _view = View::Picker;
  markDirty();  // repaint over the error X (panel RAM diff handles the rest)
}

void SettingsScene::handleInput(Input& in) {
  switch (_view) {
    case View::Menu:
      if (in.wasPressed(Btn::Back)) {
        showLauncher();
        return;
      }
      if (in.wasPressed(Btn::Up) || in.wasPressed(Btn::Left)) moveSel(_menuSel, kMenuCount, -1);
      if (in.wasPressed(Btn::Down) || in.wasPressed(Btn::Right)) moveSel(_menuSel, kMenuCount, +1);
      if (in.wasPressed(Btn::Confirm)) {
        if (_menuSel == 0) {
          showFileTransfer();  // R2: Wi-Fi book sync with the Flowe app
          return;
        } else if (_menuSel == 1) {
          enterPicker();
        } else if (_menuSel == 2) {
          enterIconStyle();
        } else if (_menuSel == 3) {
          _view = View::ConfirmRestart;
          markDirty();
        } else {
          showAbout();
        }
      }
      break;

    case View::IconStyle:
      if (in.wasPressed(Btn::Back)) {
        _view = View::Menu;
        markDirty();
        return;
      }
      if (in.wasPressed(Btn::Left) || in.wasPressed(Btn::Up)) cycleIconPack(-1);
      if (in.wasPressed(Btn::Right) || in.wasPressed(Btn::Down)) cycleIconPack(+1);
      break;

    case View::Picker:
      if (in.wasPressed(Btn::Back)) {
        _view = View::Menu;
        _status = nullptr;
        markDirty();
        return;
      }
      if (in.wasPressed(Btn::Up) || in.wasPressed(Btn::Left)) moveSel(_pickSel, _fileCount, -1);
      if (in.wasPressed(Btn::Down) || in.wasPressed(Btn::Right)) moveSel(_pickSel, _fileCount, +1);
      if (in.wasPressed(Btn::Confirm) && _fileCount > 0) {
        _view = View::ConfirmFlash;
        markDirty();
      }
      break;

    case View::ConfirmFlash:
      if (in.wasPressed(Btn::Back)) {  // NO
        _view = View::Picker;
        markDirty();
      } else if (in.wasPressed(Btn::Confirm)) {  // YES
        doFlash();  // restarts on success; on failure falls back to the picker
      }
      break;

    case View::ConfirmRestart:
      if (in.wasPressed(Btn::Back)) {  // NO
        _view = View::Menu;
        markDirty();
      } else if (in.wasPressed(Btn::Confirm)) {  // YES
        Serial.println("[xphone-os] settings: user restart");
        Serial.flush();
        esp_restart();
      }
      break;
  }
}

// --- rendering ---------------------------------------------------------------

void SettingsScene::drawHeader(Gfx& gfx, const char* title, const char* right) {
  gfx.drawText(kFontBold, kMarginX, 8, title);
  if (right && right[0]) {
    gfx.drawText(kFontRegular, gfx.width() - kMarginX - gfx.textWidth(kFontRegular, right), 8, right);
  }
  gfx.fillRect(0, kHeaderH - 2, gfx.width(), 2, true);
}

// One CrossPoint-style list row: inverted full-width bar when selected, label
// left / value right (BaseTheme.cpp:296-350).
void SettingsScene::drawRow(Gfx& gfx, const int y, const int rowH, const char* label, const char* value,
                            const bool selected) {
  if (selected) gfx.fillRect(0, y, gfx.width(), rowH, true);
  const int textY = y + (rowH - gfx.lineHeight(kFontBold)) / 2;
  int valueW = 0;
  if (value && value[0]) {
    valueW = gfx.textWidth(kFontRegular, value) + 10;
    gfx.drawText(kFontRegular, gfx.width() - kMarginX - (valueW - 10), textY, value, !selected);
  }
  char clipped[96];
  truncateToWidth(gfx, kFontBold, label, gfx.width() - 2 * kMarginX - valueW, clipped, sizeof(clipped));
  gfx.drawText(kFontBold, kMarginX, textY, clipped, !selected);
}

void SettingsScene::renderMenu(Gfx& gfx) {
  drawHeader(gfx, "Settings", nullptr);  // version lives in the footer, once

  const int rowH = gfx.lineHeight(kFontBold) + kRowPad;
  int y = kHeaderH + 8;
  for (int i = 0; i < kMenuCount; i++) {
    const char* value = (i == 2) ? IconStyle::activeName() : nullptr;
    drawRow(gfx, y, rowH, kMenuLabels[i], value, i == _menuSel);
    y += rowH;
  }

  // Footer: firmware version + build env, bottom of the content area
  // (mirrors CrossPoint showing CROSSPOINT_VERSION in the settings header).
  char footer[64];
  snprintf(footer, sizeof(footer), "xphone-os %s (%s)", XPHONE_VERSION, gDeviceIsX3 ? "x3" : "x4");
  const int footY = gfx.height() - Scene::SOFTKEY_BAR_H - gfx.lineHeight(kFontRegular) - 6;
  gfx.drawTextCentered(kFontRegular, gfx.width() / 2, footY, footer);
}

void SettingsScene::renderIconStyle(Gfx& gfx) {
  const uint8_t pack = IconStyle::get();
  const uint8_t n = IconStyle::packCount();
  char right[24];
  snprintf(right, sizeof(right), "%u / %u", static_cast<unsigned>(pack + 1), static_cast<unsigned>(n));
  drawHeader(gfx, "Icon style", right);

  const char* name = IconStyle::activeName();
  gfx.drawTextCentered(kFontBold, gfx.width() / 2, kHeaderH + 10, name ? name : "?");
  gfx.drawTextCentered(kFontRegular, gfx.width() / 2, kHeaderH + 10 + gfx.lineHeight(kFontBold) + 4,
                       "PREV / NEXT to try on glass");

  // 3x2 mini launcher preview using the active pack (live — cycle applies immediately).
  constexpr int kCols = 2;
  constexpr int kRows = 3;
  constexpr int kPreviewIcon = 72;  // scaled-down blit of the 104px masters
  constexpr int kGap = 16;
  constexpr int kLabelH = 18;
  const int cellW = kPreviewIcon + 8;
  const int cellH = kPreviewIcon + kLabelH + 4;
  const int gridW = kCols * cellW + (kCols - 1) * kGap;
  const int gridH = kRows * cellH + (kRows - 1) * kGap;
  const int gridX = (gfx.width() - gridW) / 2;
  const int gridY = kHeaderH + 10 + 2 * gfx.lineHeight(kFontBold) + 20;

  // Launcher order — Today(0) Notifications(1) Priorities(2) Block(3)
  // Read(4) Workout(5). The first version hand-typed a different order and
  // five of six previews wore the wrong name (audit 2026-08-18, A1).
  static constexpr const char* kLabels[6] = {"Today", "Notif", "Priorities", "Block", "Read", "Workout"};

  // Downscale 104 -> 72 by nearest-neighbor sampling into a stack buffer of
  // one destination row, then draw. Keeps the preview cheap (no heap).
  const int srcSize = XPhoneLauncherIconSize;
  for (int i = 0; i < 6; i++) {
    const int col = i % kCols;
    const int row = i / kCols;
    const int cx = gridX + col * (cellW + kGap);
    const int cy = gridY + row * (cellH + kGap);
    const uint8_t* src = IconStyle::iconForApp(i);
    if (src) {
      const int rowBytes = (srcSize + 7) / 8;
      const int ix = cx + (cellW - kPreviewIcon) / 2;
      const int iy = cy;
      for (int dy = 0; dy < kPreviewIcon; dy++) {
        const int sy = dy * srcSize / kPreviewIcon;
        for (int dx = 0; dx < kPreviewIcon; dx++) {
          const int sx = dx * srcSize / kPreviewIcon;
          const uint8_t byte = src[sy * rowBytes + (sx >> 3)];
          if (((byte >> (7 - (sx & 7))) & 1) == 0) gfx.drawPixel(ix + dx, iy + dy, true);
        }
      }
    }
    gfx.drawTextCentered(kFontRegular, cx + cellW / 2, cy + kPreviewIcon + 2, kLabels[i]);
  }
}

void SettingsScene::renderPicker(Gfx& gfx) {
  char right[24];
  snprintf(right, sizeof(right), "%d file%s", _fileCount, _fileCount == 1 ? "" : "s");
  drawHeader(gfx, "SD Firmware Update", _sdOk ? right : nullptr);

  const int midY = gfx.height() / 2;
  if (!_sdOk) {
    gfx.drawTextCentered(kFontBold, gfx.width() / 2, midY - gfx.lineHeight(kFontBold), "No SD card");
    gfx.drawTextCentered(kFontRegular, gfx.width() / 2, midY + 4, "Insert a card and reopen this screen");
    return;
  }
  if (_fileCount == 0) {
    gfx.drawTextCentered(kFontBold, gfx.width() / 2, midY - gfx.lineHeight(kFontBold), "No .bin files found");
    gfx.drawTextCentered(kFontRegular, gfx.width() / 2, midY + 4, "Copy a firmware .bin to the SD root");
    return;
  }

  int y = kHeaderH + 8;
  if (_status) {  // e.g. a failed flash attempt
    gfx.drawText(kFontRegular, kMarginX, y, _status);
    y += gfx.lineHeight(kFontRegular) + 6;
  }

  // CrossPoint-style page-at-a-time list (BaseTheme.cpp:302).
  const int rowH = gfx.lineHeight(kFontBold) + kRowPad;
  const int avail = gfx.height() - y - Scene::SOFTKEY_BAR_H - 4;
  int perPage = avail / rowH;
  if (perPage < 1) perPage = 1;
  const int pageStart = (_pickSel / perPage) * perPage;
  for (int i = pageStart; i < _fileCount && i < pageStart + perPage; i++) {
    char size[24];
    formatSize(_files[i].size, size, sizeof(size));
    drawRow(gfx, y, rowH, _files[i].name, size, i == _pickSel);
    y += rowH;
  }
}

void SettingsScene::renderConfirmFlash(Gfx& gfx) {
  drawHeader(gfx, "SD Firmware Update", nullptr);
  const BinFile& f = _files[_pickSel];

  char line[96];
  char clipped[80];
  truncateToWidth(gfx, kFontBold, f.name, gfx.width() - 2 * kMarginX - gfx.textWidth(kFontBold, "Flash ?"), clipped,
                  sizeof(clipped));
  snprintf(line, sizeof(line), "Flash %s?", clipped);

  int y = gfx.height() / 2 - 2 * gfx.lineHeight(kFontBold);
  gfx.drawTextCentered(kFontBold, gfx.width() / 2, y, line);
  y += gfx.lineHeight(kFontBold) + 6;

  char size[24];
  formatSize(f.size, size, sizeof(size));
  gfx.drawTextCentered(kFontRegular, gfx.width() / 2, y, size);
  y += gfx.lineHeight(kFontRegular) + 14;
  gfx.drawTextCentered(kFontRegular, gfx.width() / 2, y, "Flashes the inactive slot, then restarts.");
}

void SettingsScene::renderConfirmRestart(Gfx& gfx) {
  drawHeader(gfx, "Restart", nullptr);
  gfx.drawTextCentered(kFontBold, gfx.width() / 2, gfx.height() / 2 - gfx.lineHeight(kFontBold), "Restart device?");
}

void SettingsScene::render(Gfx& gfx) {
  _gfx = &gfx;  // captured for doFlash() (see header)
  switch (_view) {
    case View::Menu:
      renderMenu(gfx);
      break;
    case View::Picker:
      renderPicker(gfx);
      break;
    case View::ConfirmFlash:
      renderConfirmFlash(gfx);
      break;
    case View::ConfirmRestart:
      renderConfirmRestart(gfx);
      break;
    case View::IconStyle:
      renderIconStyle(gfx);
      break;
  }
}
