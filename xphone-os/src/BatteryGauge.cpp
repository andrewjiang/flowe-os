// BQ27220 gauge access + Design Capacity reprogramming (see BatteryGauge.h).
//
// The CFG UPDATE / data-memory write sequence below is verified against the
// Flipper Zero production driver (flipperzero-firmware/lib/drivers/bq27220.c,
// bq27220_reg.h, bq27220_data_memory.h — the Flipper's battery is a BQ27220
// programmed with exactly this code) and matches TI TRM SLUUBD4A:
//   * Unseal: Control() subcommands 0x0414 then 0x3672 (little-endian to
//     reg 0x00), ~5 ms apart; only needed when OperationStatus SEC bits
//     [2:1] read 0b11 (sealed). Unsealed = 0b10, full access = 0b01.
//   * ENTER_CFG_UPDATE (0x0090) written little-endian to the MAC address
//     register 0x3E (the Flipper writes this subcommand via 0x3E; 0x3E
//     mirrors Control() for subcommands), then poll OperationStatus 0x3A
//     bit 10 (CFGUPDATE, 0x0400) until set.
//   * DM write, one transaction to 0x3E ff. (auto-increment): DM address
//     little-endian (0x929F -> 0x9F 0x92), then the parameter bytes
//     MSB-first (Flipper: buffer[1+size-i] = value >> (i*8)). 0x929F is
//     BQ27220DMAddressGasGaugingCEDVProfile1DesignCapacity ("Gas Gauging >
//     CEDV Profile 1 > Design Capacity mAh").
//   * Checksum to 0x60 = 0xFF - (8-bit sum of the 2 address bytes + data
//     bytes); length to 0x61 = 2 (address) + size (data) + 1 + 1 = 6 for a
//     16-bit parameter. Short settle delays between MAC writes (the Flipper
//     documents failures below ~120 us / ~500 us; we use generous ones).
//   * Verify: rewrite the DM address to 0x3E, read the bytes back from 0x40.
//   * EXIT_CFG_UPDATE_REINIT (0x0091) to Control() 0x00, then poll CFGUPDATE
//     cleared. REINIT re-runs gauge init against the new data memory, which
//     rescales SoC immediately. The gauge is deliberately left unsealed.
// Datasheet note: SLUUBD4's own DM-update chapter is known-wrong; the
// working procedure is the one above (TI E2E thread 719878, referenced in
// the Flipper source).

#include "BatteryGauge.h"

#include <Arduino.h>
#include <BoardConfig.h>
#if FREEINK_BATTERY_I2C_GAUGE
#include <Wire.h>
#endif

// Real X3 pack is ~650 mAh. Override per-build: -DXP_BATT_DESIGN_MAH=1000.
#ifndef XP_BATT_DESIGN_MAH
#define XP_BATT_DESIGN_MAH 650
#endif

namespace BatteryGauge {

#if FREEINK_BATTERY_I2C_GAUGE

namespace {

constexpr uint16_t kDesignCapacityMah = XP_BATT_DESIGN_MAH;
static_assert(XP_BATT_DESIGN_MAH > 0 && XP_BATT_DESIGN_MAH <= 0xFFFF,
              "XP_BATT_DESIGN_MAH must fit a 16-bit mAh value");

// Registers (TI SLUUBD4A / Flipper bq27220_reg.h).
constexpr uint8_t kRegControl = 0x00;          // Control() subcommand, LE
constexpr uint8_t kRegOperationStatus = 0x3A;  // status word
constexpr uint8_t kRegSelectSubclass = 0x3E;   // MAC: DM address / subcommand
constexpr uint8_t kRegMACData = 0x40;          // MAC data window
constexpr uint8_t kRegMACDataSum = 0x60;       // checksum, then length at 0x61

// Control()/MAC subcommands.
constexpr uint16_t kSubUnsealKey1 = 0x0414;
constexpr uint16_t kSubUnsealKey2 = 0x3672;
constexpr uint16_t kSubEnterCfgUpdate = 0x0090;
constexpr uint16_t kSubExitCfgUpdateReinit = 0x0091;

// Data-memory address: Gas Gauging > CEDV Profile 1 > Design Capacity (mAh).
constexpr uint16_t kDmDesignCapacity = 0x929F;

// OperationStatus 0x3A: CFGUPDATE = bit 10; SEC = bits [2:1].
constexpr uint16_t kOpCfgUpdateMask = 0x0400;
constexpr uint16_t kSecSealed = 0x3;  // 0b11 sealed, 0b10 unsealed, 0b01 full

// Returns the gauge I2C address (0 = none) with the bus brought up.
// TwoWire::begin() is idempotent, so this is safe to call per operation.
uint8_t busAddr() {
  const auto& g = BoardConfig::ACTIVE.batteryGauge;
  if (g.gaugeAddr == 0) return 0;
  Wire.begin(g.i2cSda, g.i2cScl, g.i2cHz);
  return g.gaugeAddr;
}

bool writeBytes(uint8_t addr, uint8_t reg, const uint8_t* data, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.write(data, n) != n) return false;
  return Wire.endTransmission() == 0;
}

bool readBytes(uint8_t addr, uint8_t reg, uint8_t* data, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;  // repeated start
  if (Wire.requestFrom(addr, n, static_cast<uint8_t>(1)) < n) return false;
  for (uint8_t i = 0; i < n; ++i) data[i] = Wire.read();
  return true;
}

bool readWordReg(uint8_t addr, uint8_t reg, uint16_t& out) {
  uint8_t b[2];
  if (!readBytes(addr, reg, b, 2)) return false;
  out = static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
  return true;
}

// Subcommands go out little-endian regardless of target register (0x00/0x3E).
bool writeSubcommand(uint8_t addr, uint8_t reg, uint16_t sub) {
  const uint8_t b[2] = {static_cast<uint8_t>(sub & 0xFF), static_cast<uint8_t>(sub >> 8)};
  return writeBytes(addr, reg, b, 2);
}

// Poll OperationStatus until CFGUPDATE matches `wantSet` (10 ms cadence).
bool waitCfgUpdate(uint8_t addr, bool wantSet, uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  do {
    uint16_t op = 0;
    if (readWordReg(addr, kRegOperationStatus, op) && ((op & kOpCfgUpdateMask) != 0) == wantSet) {
      return true;
    }
    delay(10);
  } while (static_cast<int32_t>(deadline - millis()) > 0);
  return false;
}

// One DM parameter write + readback verify, inside CFG UPDATE mode.
bool writeDesignCapacityDm(uint8_t addr, uint16_t mah) {
  // DM address little-endian, then value MSB-first (Flipper byte order).
  const uint8_t block[4] = {
      static_cast<uint8_t>(kDmDesignCapacity & 0xFF),
      static_cast<uint8_t>(kDmDesignCapacity >> 8),
      static_cast<uint8_t>(mah >> 8),
      static_cast<uint8_t>(mah & 0xFF),
  };
  if (!writeBytes(addr, kRegSelectSubclass, block, sizeof(block))) {
    Serial.println("[xphone-os] gauge: DM block write failed");
    return false;
  }
  delayMicroseconds(500);  // MAC write settle (Flipper: fails below ~120 us)

  uint8_t sum = 0;
  for (uint8_t i = 0; i < sizeof(block); ++i) sum += block[i];
  // checksum = 0xFF - sum(address + data); length = 2 + 2 + 1 + 1.
  const uint8_t tail[2] = {static_cast<uint8_t>(0xFF - sum), 6};
  if (!writeBytes(addr, kRegMACDataSum, tail, 2)) {
    Serial.println("[xphone-os] gauge: DM checksum write failed");
    return false;
  }
  delay(15);  // config write settle (Flipper waits 10 ms, bqstudio ~15 ms)

  // Verify: re-select the DM address, read the value back from the MAC window.
  if (!writeBytes(addr, kRegSelectSubclass, block, 2)) {
    Serial.println("[xphone-os] gauge: DM readback select failed");
    return false;
  }
  delay(2);  // MAC load settle (Flipper: fails below ~500 us)
  uint8_t rb[2] = {0, 0};
  if (!readBytes(addr, kRegMACData, rb, 2)) {
    Serial.println("[xphone-os] gauge: DM readback failed");
    return false;
  }
  if (rb[0] != block[2] || rb[1] != block[3]) {
    Serial.printf("[xphone-os] gauge: DM verify mismatch (%02x%02x != %02x%02x)\n", rb[0], rb[1],
                  block[2], block[3]);
    return false;
  }
  return true;
}

}  // namespace

bool readWord(uint8_t cmd, uint16_t& out) {
  const uint8_t addr = busAddr();
  if (addr == 0) return false;
  return readWordReg(addr, cmd, out);
}

bool readAvgCurrentMa(int16_t& outMa) {
  uint16_t raw = 0;
  if (!readWord(kCmdAverageCurrent, raw)) return false;
  outMa = static_cast<int16_t>(raw);
  return true;
}

void ensureDesignCapacity() {
  const uint8_t addr = busAddr();
  if (addr == 0) return;

  uint16_t current = 0;
  if (!readWordReg(addr, kCmdDesignCapacity, current)) {
    Serial.println("[xphone-os] gauge: design capacity read failed; skipping check");
    return;
  }
  if (current == kDesignCapacityMah) {
    Serial.printf("[xphone-os] gauge: design capacity OK (%u mAh)\n",
                  static_cast<unsigned>(current));
    return;
  }

  Serial.printf("[xphone-os] gauge: design capacity %u mAh != %u mAh; reprogramming\n",
                static_cast<unsigned>(current), static_cast<unsigned>(kDesignCapacityMah));

  // Unseal only when sealed (the Flipper driver checks SEC the same way).
  uint16_t op = 0;
  if (!readWordReg(addr, kRegOperationStatus, op)) {
    Serial.println("[xphone-os] gauge: operation status read failed; aborting");
    return;
  }
  if (((op >> 1) & 0x3) == kSecSealed) {
    bool sent = writeSubcommand(addr, kRegControl, kSubUnsealKey1);
    delay(5);
    sent = writeSubcommand(addr, kRegControl, kSubUnsealKey2) && sent;
    delay(5);
    if (!sent || !readWordReg(addr, kRegOperationStatus, op) || ((op >> 1) & 0x3) == kSecSealed) {
      Serial.println("[xphone-os] gauge: unseal failed; aborting");
      return;
    }
  }

  // Enter CFG UPDATE (subcommand via the MAC register, as the Flipper does).
  if (!writeSubcommand(addr, kRegSelectSubclass, kSubEnterCfgUpdate)) {
    Serial.println("[xphone-os] gauge: ENTER_CFG_UPDATE write failed; aborting");
    return;
  }
  bool ok = false;
  if (!waitCfgUpdate(addr, true, 1000)) {
    Serial.println("[xphone-os] gauge: CFGUPDATE never set");
  } else {
    ok = writeDesignCapacityDm(addr, kDesignCapacityMah);
  }

  // ALWAYS attempt the exit, success or not — never leave the gauge parked in
  // CFG UPDATE mode. REINIT re-inits gauging against the new design capacity.
  if (!writeSubcommand(addr, kRegControl, kSubExitCfgUpdateReinit)) {
    Serial.println("[xphone-os] gauge: EXIT_CFG_UPDATE_REINIT write failed");
  }
  if (!waitCfgUpdate(addr, false, 2500)) {
    Serial.println("[xphone-os] gauge: CFGUPDATE never cleared after exit");
    ok = false;
  }

  // Confirm through the standard-command mirror once REINIT settles.
  uint16_t after = 0;
  for (uint8_t i = 0; ok && i < 30; ++i) {
    if (readWordReg(addr, kCmdDesignCapacity, after) && after == kDesignCapacityMah) break;
    delay(50);
  }
  if (ok && after == kDesignCapacityMah) {
    Serial.printf("[xphone-os] gauge: design capacity %u -> %u mAh reprogrammed\n",
                  static_cast<unsigned>(current), static_cast<unsigned>(after));
  } else {
    Serial.printf("[xphone-os] gauge: reprogram FAILED (still %u mAh); will retry next boot\n",
                  static_cast<unsigned>(ok ? after : current));
  }
  // Deliberately not re-sealed: nothing secret in there, and staying unsealed
  // keeps the next reprogram (after a battery disconnect) a two-step shorter.
}

#else  // !FREEINK_BATTERY_I2C_GAUGE — X4 ADC build: no gauge on the bus.

bool readWord(uint8_t, uint16_t&) { return false; }
bool readAvgCurrentMa(int16_t&) { return false; }
void ensureDesignCapacity() {}

#endif  // FREEINK_BATTERY_I2C_GAUGE

}  // namespace BatteryGauge
