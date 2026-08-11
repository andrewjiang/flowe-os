#pragma once
#include <Arduino.h>

// M2.1b (power lever 3): CPU clock after boot, MHz. ESP32-C3 BLE requires
// >= 80 MHz (the ESP-IDF BT port refuses lower; the BLE modem clock itself is
// sourced independently of the CPU PLL), so 80 is the floor AND the target —
// ~30-40% lower active/idle core power vs the prebuilt core's 160 MHz
// default. Override without editing code: build_flags -DXP_CPU_MHZ=160.
#ifndef XP_CPU_MHZ
#define XP_CPU_MHZ 80
#endif

// Race-to-idle scope guard: hold 160 MHz exactly while CPU-bound reader work
// runs (chapter indexing, page compose, cover decode), restore the XP_CPU_MHZ
// park on every exit path — including early returns and failWith() paths.
//
// Boost-per-work beats boost-per-scene: the reader's wall-clock is dominated
// by WAITING (panel refresh ~400 ms, button idle, hours-long reading
// sessions), and a waiting core at 160 MHz burns roughly double for zero
// speedup. Scoping the boost to the work sites keeps the battery ledger
// strictly positive: same joules per job finished sooner, park the rest.
//
// Depth-counted so nested guards (a compose inside a work unit) restore only
// at the outermost exit. Main loop is single-threaded; no locking needed.
class CpuBoost {
 public:
  CpuBoost() {
    if (depth()++ == 0 && getCpuFrequencyMhz() < 160) setCpuFrequencyMhz(160);
  }
  ~CpuBoost() {
    if (--depth() == 0 && getCpuFrequencyMhz() != XP_CPU_MHZ) setCpuFrequencyMhz(XP_CPU_MHZ);
  }
  CpuBoost(const CpuBoost&) = delete;
  CpuBoost& operator=(const CpuBoost&) = delete;

 private:
  static int& depth() {
    static int d = 0;
    return d;
  }
};
