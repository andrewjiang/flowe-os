#include "XteinkDetect.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <string.h>

namespace freeink {

namespace {

// X3-only peripherals on the secondary I2C bus (SDA=20, SCL=0).
constexpr int X3_I2C_SDA = 20;
constexpr int X3_I2C_SCL = 0;
constexpr uint32_t X3_I2C_FREQ = 400000;

constexpr uint8_t ADDR_BQ27220 = 0x55;  // fuel gauge
constexpr uint8_t ADDR_DS3231 = 0x68;   // RTC
constexpr uint8_t ADDR_QMI8658 = 0x6B;  // IMU
constexpr uint8_t ADDR_QMI8658_ALT = 0x6A;

constexpr uint8_t BQ27220_SOC_REG = 0x2C;
constexpr uint8_t BQ27220_VOLT_REG = 0x08;
constexpr uint8_t DS3231_SEC_REG = 0x00;
constexpr uint8_t QMI8658_WHO_AM_I_REG = 0x00;
constexpr uint8_t QMI8658_WHO_AM_I_VALUE = 0x05;

bool readReg8(uint8_t addr, uint8_t reg, uint8_t* out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) return false;
  *out = Wire.read();
  return true;
}

bool readReg16LE(uint8_t addr, uint8_t reg, uint16_t* out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) Wire.read();
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *out = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

// Each probe checks not just for an ACK but for a plausible value, so a stray
// pull-up or floating bus can't masquerade as a present chip.
bool probeBq27220() {
  uint16_t soc = 0;
  uint16_t mv = 0;
  if (!readReg16LE(ADDR_BQ27220, BQ27220_SOC_REG, &soc) || soc > 100) return false;
  if (!readReg16LE(ADDR_BQ27220, BQ27220_VOLT_REG, &mv)) return false;
  return mv >= 2500 && mv <= 5000;
}

bool probeDs3231() {
  uint8_t sec = 0;
  if (!readReg8(ADDR_DS3231, DS3231_SEC_REG, &sec)) return false;
  const uint8_t tens = (sec >> 4) & 0x07;
  const uint8_t ones = sec & 0x0F;
  return tens <= 5 && ones <= 9;  // valid BCD seconds
}

bool probeQmi8658() {
  uint8_t who = 0;
  if (readReg8(ADDR_QMI8658, QMI8658_WHO_AM_I_REG, &who) && who == QMI8658_WHO_AM_I_VALUE) return true;
  if (readReg8(ADDR_QMI8658_ALT, QMI8658_WHO_AM_I_REG, &who) && who == QMI8658_WHO_AM_I_VALUE) return true;
  return false;
}

// Nine-pulse bus clear (NXP I2C spec 3.1.16). A slave left mid-transaction
// by an unclean reset can hold SDA low indefinitely; every probe below would
// then fail and an X3 would fingerprint as an X4 — wrong panel protocol,
// blind device. Toggling SCL up to nine times lets the stuck slave clock out
// its remaining bits and release SDA; the manual STOP afterwards resets every
// slave's bus state machine. No-op (one read) when the bus is already idle —
// including on every X4, where nothing drives these pins.
void clearStuckBus() {
  pinMode(X3_I2C_SDA, INPUT_PULLUP);
  pinMode(X3_I2C_SCL, INPUT_PULLUP);
  delayMicroseconds(10);
  if (digitalRead(X3_I2C_SDA) == HIGH) return;

  pinMode(X3_I2C_SCL, OUTPUT_OPEN_DRAIN);
  digitalWrite(X3_I2C_SCL, HIGH);
  for (int i = 0; i < 9 && digitalRead(X3_I2C_SDA) == LOW; ++i) {
    digitalWrite(X3_I2C_SCL, LOW);
    delayMicroseconds(10);  // ~50 kHz half-periods: slow enough for any slave
    digitalWrite(X3_I2C_SCL, HIGH);
    delayMicroseconds(10);
  }
  // STOP condition — SDA rising while SCL is high.
  pinMode(X3_I2C_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(X3_I2C_SDA, LOW);
  delayMicroseconds(10);
  digitalWrite(X3_I2C_SDA, HIGH);
  delayMicroseconds(10);
  pinMode(X3_I2C_SDA, INPUT);
  pinMode(X3_I2C_SCL, INPUT);
}

uint8_t runProbePass() {
  Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
  Wire.setTimeOut(6);
  const uint8_t score =
      static_cast<uint8_t>(probeBq27220()) + static_cast<uint8_t>(probeDs3231()) + static_cast<uint8_t>(probeQmi8658());
  Wire.end();
  pinMode(X3_I2C_SDA, INPUT);
  pinMode(X3_I2C_SCL, INPUT);
  return score;
}


// --- UC81xx display-controller probe ----------------------------------------
// Newer X3 units ship a UC8279d where older ones have a UC8253. The two need
// different drivers, and driving UC8253 sequences at a UC8279d leaves the panel
// dark with BUSY stuck asserted, so the controller must be identified before
// the display comes up. UC81xx parts answer register 0x70 (VER) and 0x71 (FLG);
// the UC8253 and SSD-family do not, so their half-duplex line floats to a
// uniform level. Bit-banged because this runs before EpdBus::begin().
constexpr uint8_t UC81XX_CMD_VER = 0x70;
constexpr uint8_t UC81XX_CMD_FLG = 0x71;

struct EpdProbePins {
  int8_t sclk, mosi, cs, dc, rst, busy;
};

inline void epdClockDelay() { delayMicroseconds(1); }  // ~500 kHz, timing-safe

void epdWriteByte(const EpdProbePins& p, uint8_t b) {
  for (uint8_t i = 0; i < 8; i++) {
    digitalWrite(p.mosi, (b & 0x80) ? HIGH : LOW);
    epdClockDelay();
    digitalWrite(p.sclk, HIGH);
    epdClockDelay();
    digitalWrite(p.sclk, LOW);
    b <<= 1;
  }
}

uint8_t epdReadByte(const EpdProbePins& p) {
  uint8_t b = 0;
  for (uint8_t i = 0; i < 8; i++) {
    // The controller shifts the next bit out on the SCL falling edge; sample
    // while the clock is low, then pulse.
    epdClockDelay();
    b = static_cast<uint8_t>((b << 1) | (digitalRead(p.mosi) == HIGH ? 1 : 0));
    digitalWrite(p.sclk, HIGH);
    epdClockDelay();
    digitalWrite(p.sclk, LOW);
  }
  return b;
}

// Command with DC low, then release SDA (our MOSI) to input with DC high while
// the controller drives the read bytes.
void epdCmdRead(const EpdProbePins& p, uint8_t cmd, uint8_t* out, uint8_t len) {
  pinMode(p.mosi, OUTPUT);
  digitalWrite(p.dc, LOW);
  digitalWrite(p.cs, LOW);
  epdClockDelay();
  epdWriteByte(p, cmd);
  digitalWrite(p.dc, HIGH);
  pinMode(p.mosi, INPUT_PULLUP);
  epdClockDelay();
  for (uint8_t i = 0; i < len; i++) out[i] = epdReadByte(p);
  digitalWrite(p.cs, HIGH);
  pinMode(p.mosi, OUTPUT);
}

// Five identical bytes means nobody drove the line — a floating read, not a UC81xx.
bool verIsFloating(const uint8_t ver[5]) {
  for (int i = 1; i < 5; i++)
    if (ver[i] != ver[0]) return false;
  return true;
}

// No specific CHIP_VER is required: shipping parts have been seen reporting 0x00.
// What matters is that FLG is a real driven status with BUSY_N (bit 0) idle and
// that VER is a structured, non-uniform response.
bool matchUc81xx(const uint8_t ver[5], uint8_t flg) {
  if (flg == 0x00 || flg == 0xFF) return false;
  if ((flg & 0x01) != 0x01) return false;
  return !verIsFloating(ver);
}

bool runDisplayProbePass(const EpdProbePins& p, uint8_t ver[5], uint8_t* flg, uint8_t rstLowMs) {
  pinMode(p.cs, OUTPUT);
  digitalWrite(p.cs, HIGH);
  pinMode(p.sclk, OUTPUT);
  digitalWrite(p.sclk, LOW);
  pinMode(p.dc, OUTPUT);
  digitalWrite(p.dc, LOW);
  pinMode(p.mosi, OUTPUT);
  if (p.busy >= 0) pinMode(p.busy, INPUT);

  // We cannot gate on BUSY here — which controller (and so which idle level) is
  // present is exactly what we are identifying — so use a flat settle. The panel
  // driver's begin() resets again afterwards, so this leaves no state behind.
  if (p.rst >= 0) {
    // A deep-sleep pin hold survives the wake reset; release it or every write
    // below silently bounces off the latch and the probe picks the wrong driver.
    gpio_hold_dis(static_cast<gpio_num_t>(p.rst));
    pinMode(p.rst, OUTPUT);
    digitalWrite(p.rst, HIGH);
    delay(2);
    digitalWrite(p.rst, LOW);
    delay(rstLowMs);
    digitalWrite(p.rst, HIGH);
  }
  delay(30);

  uint8_t flgByte = 0;
  epdCmdRead(p, UC81XX_CMD_FLG, &flgByte, 1);
  epdCmdRead(p, UC81XX_CMD_VER, ver, 5);
  if (flg) *flg = flgByte;
  return matchUc81xx(ver, flgByte);
}

void releaseDisplayPins(const EpdProbePins& p) {
  // RST_N has an internal pull-up, so INPUT keeps the controller out of reset.
  pinMode(p.sclk, INPUT);
  pinMode(p.mosi, INPUT);
  pinMode(p.cs, INPUT_PULLUP);  // don't leave the panel selected
  pinMode(p.dc, INPUT);
  if (p.rst >= 0) pinMode(p.rst, INPUT);
}

}  // namespace

bool detectXteinkX3IsUc8279() {
  // X3 display pinout (BoardConfig XTEINK_X3.display): SCLK8 MOSI10 CS21 DC4
  // RST5 BUSY6. Identical on both X3 siblings — only the controller differs.
  const EpdProbePins p{8, 10, 21, 4, 5, 6};

  uint8_t ver1[5] = {0}, ver2[5] = {0};
  uint8_t flg1 = 0;
  // Pass 1 screens with a cheap 1 ms reset; a UC8253 never answers, so it does
  // not pay the vendor identification timing. Retry once at doc timing (RST low
  // 50 ms) before concluding there is no UC81xx part.
  bool pass1 = runDisplayProbePass(p, ver1, &flg1, /*rstLowMs=*/1);
  if (!pass1) {
    delay(2);
    pass1 = runDisplayProbePass(p, ver1, &flg1, /*rstLowMs=*/50);
  }
  delay(2);
  const bool pass2 = runDisplayProbePass(p, ver2, nullptr, /*rstLowMs=*/pass1 ? 50 : 1);
  releaseDisplayPins(p);

  // Confirmed only when both passes match AND agree byte for byte: a floating
  // bus cannot produce the same stable non-trivial pattern twice.
  const bool confirmed = pass1 && pass2 && memcmp(ver1, ver2, 5) == 0;
  if (Serial) {
    Serial.printf("[freeink] epd probe: VER=%02X %02X %02X %02X %02X FLG=%02X -> %s\n", ver1[0], ver1[1], ver1[2],
                  ver1[3], ver1[4], flg1, confirmed ? "UC8279" : "UC8253");
  }
  return confirmed;
}

bool detectXteinkIsX3() {
  clearStuckBus();
  const uint8_t pass1 = runProbePass();
  delay(2);
  const uint8_t pass2 = runProbePass();
  // X3 confirmed only when both passes see at least two of the three chips; the
  // X4 sees zero, so a single stray ACK never flips the result.
  return pass1 >= 2 && pass2 >= 2;
}

bool selectXteinkDevice() {
  const bool isX3 = detectXteinkIsX3();
  if (!isX3) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX4);
    return false;
  }
  // Same board, two panel controllers. Ask the controller which one it is.
  const bool uc8279 = detectXteinkX3IsUc8279();
  BoardConfig::selectDevice(uc8279 ? BoardConfig::Board::XteinkX3Uc8279 : BoardConfig::Board::XteinkX3);
  return true;
}

}  // namespace freeink
