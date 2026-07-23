#pragma once

// UC8253 panel driver — Xteink X3 (792x528 B/W + grayscale). Ported from the
// community-sdk `main` lineage (the production X3 implementation CrossPoint
// ships). Selected at runtime via FreeInkDisplay::setDisplayX3() so one binary
// drives both X3 and X4.
//
// Three-tier differential refresh, all single-phase 1-bpp:
//   FAST -> `_fast` turbo LUTs (DTM1 holds prev frame, diffs against it)
//   HALF -> `_half` scrub LUTs (WW==BW, WB==BB: drive to target ignoring DTM1)
//   FULL -> `_full` OEM bank from a white DTM1 baseline + post-full settle pass
// Grayscale: `_gc` 4-level (community AA/cover) or `_full` (factory absolute),
// reverted via the `_half` scrub bank. DTM1/DTM2 are the controller's old/new
// RAM planes; CDI (cmd 0x50) selects differential (0x29) vs absolute (0xA9).
//
// X3TwoPhase BUSY; SPI clock board-overridable (default 16 MHz).

#include "PanelDriver.h"

namespace freeink {

// One UC8253 waveform bank: VCOM + the four transition LUTs.
struct Uc8253LutBank {
  const uint8_t* vcom;
  const uint8_t* ww;
  const uint8_t* bw;
  const uint8_t* wb;
  const uint8_t* bb;
};

struct Uc8253X3Config {
  Uc8253LutBank normal;  // condition-pass / settle (CDI 0xA9)
  Uc8253LutBank half;    // scrub (CDI 0xA9)
  Uc8253LutBank fast;    // turbo differential (CDI 0x29)
  Uc8253LutBank full;    // OEM full / factory (CDI 0x29)
  Uc8253LutBank gc;        // community 4-level grayscale (CDI 0x29)
  Uc8253LutBank preBwMid;  // OEM grayscale preconditioning settle (CDI 0xA9)
  Uc8253LutBank flash;     // ~8-frame press-feedback flash (CDI 0x29, windowed only)
  uint8_t lutLen;          // bytes per LUT sent to the controller (42)
};

const Uc8253X3Config& uc8253X3DefaultConfig();

class Uc8253X3Driver : public PanelDriver {
 public:
  explicit Uc8253X3Driver(const Uc8253X3Config& cfg = uc8253X3DefaultConfig());

  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::X3TwoPhase; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;

  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;

  // Windowed FAST differential via PTL (0x91/0x90/0x92) with the full-voltage
  // `_fast` bank and the standard DTM contract (DTM2=new inside the window,
  // fired, then DTM1 window resync). Unlike the retired composite-partial
  // experiment this never touches the half-conditioning preBwMid bank, so the
  // window is left in the same firmly-driven state a full-frame FAST leaves.
  // Falls back to full-frame FAST whenever the controller RAM can't support a
  // clean differential (grayscale planes loaded, boot conditioning pending,
  // resync forced, or panel powered down).
  void displayWindow(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, uint16_t x, uint16_t y, uint16_t w,
                     uint16_t h, bool turnOff) override;
  // Same PTL window mechanics but fired with the ~8-frame `flash` bank —
  // sub-200 ms press feedback for tiny regions (soft-key tab invert).
  void displayWindowFlash(EpdBus& bus, const uint8_t* fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h) override;

  bool supportsStripGrayscale() const override { return true; }
  void displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) override;
  void preconditionGrayscale(EpdBus& bus, uint16_t x, uint16_t y, uint16_t w, uint16_t h) override;
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  void writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                uint16_t numRows) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;
  void cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) override;
  void grayscaleRevert(EpdBus& bus, const uint8_t* fb) override;

  void requestResync(uint8_t settlePasses) override;
  void skipInitialResync() override;

 private:
  void initController(EpdBus& bus);
  void loadBank(EpdBus& bus, const Uc8253LutBank& bank);
  void loadBankCdi(EpdBus& bus, uint8_t cdi0, uint8_t cdi1, const Uc8253LutBank& bank);
  void triggerRefresh(EpdBus& bus, bool turnOff);
  // Shared PTL windowed-refresh body for displayWindow / displayWindowFlash:
  // window write to DTM2, fire `bank` (CDI 0x29), DTM1 window resync.
  void displayWindowWithBank(EpdBus& bus, const uint8_t* fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             const Uc8253LutBank& bank, bool turnOff);

  const Uc8253X3Config& _cfg;

  uint16_t _w;
  uint16_t _h;
  uint16_t _wb;
  uint32_t _bufferSize;

  bool _isScreenOn = false;
  bool _redRamSynced = false;
  bool _inGrayscaleMode = false;
  uint8_t _initialFullSyncsRemaining = 0;
  bool _forceFullSyncNext = false;
  uint8_t _forcedConditionPassesNext = 0;
  struct GrayState {
    bool lastBaseWasPartial = false;
    bool lsbValid = false;
  } _grayState;
};

PanelDriver& uc8253X3Driver();

}  // namespace freeink
