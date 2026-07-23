// BQ27220 fuel-gauge helpers (X3 only; the X4 ADC build compiles no-op stubs).
//
// Two jobs:
//   1. readWord()/readAvgCurrentMa(): direct standard-command reads the SDK
//      BatteryMonitor doesn't surface (capacities, average current).
//   2. ensureDesignCapacity(): boot-time check-and-reprogram of the gauge's
//      Design Capacity data-memory parameter. The X3 ships with the BQ27220
//      factory default of 3000 mAh while the real pack is ~650 mAh, so SoC
//      saturates around 21-22% (650/3000). Neither the SDK BatteryMonitor
//      (freeink-sdk BatteryMonitor.cpp: reads only 0x08/0x2C) nor x4-os
//      (HalGPIO.cpp/HalPowerManager.cpp: reads only 0x08/0x0C/0x2C) ever
//      configures the gauge. Config lives in battery-backed RAM (OTP holds
//      the 3000 mAh default), so a full battery disconnect reverts it —
//      hence a mismatch-gated check at every boot, not a one-shot tool.
#pragma once

#include <cstdint>

namespace BatteryGauge {

// BQ27220 standard commands (TI TRM SLUUBD4A; same table as the Flipper Zero
// driver's bq27220_reg.h). 16-bit little-endian words.
constexpr uint8_t kCmdVoltage = 0x08;            // mV
constexpr uint8_t kCmdRemainingCapacity = 0x10;  // mAh
constexpr uint8_t kCmdFullChargeCapacity = 0x12; // mAh
constexpr uint8_t kCmdAverageCurrent = 0x14;     // signed mA
constexpr uint8_t kCmdStateOfCharge = 0x2C;      // %
constexpr uint8_t kCmdDesignCapacity = 0x3C;     // mAh (read-only mirror of DM)

// Read one 16-bit standard-command word. Returns false when there is no I2C
// gauge (X4 build / gaugeAddr 0) or on any I2C error.
bool readWord(uint8_t cmd, uint16_t& out);

// AverageCurrent() 0x14 as signed mA (negative = discharging).
bool readAvgCurrentMa(int16_t& outMa);

// Boot-time Design Capacity check; reprograms the gauge data memory when the
// value differs from XP_BATT_DESIGN_MAH (default 650). Quick no-op read on
// every boot after the first successful reprogram. Logs everything it does.
void ensureDesignCapacity();

}  // namespace BatteryGauge
