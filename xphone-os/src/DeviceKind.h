#pragma once

// Which Xteink this binary woke up on. Set ONCE at the top of boot() from
// freeink::selectXteinkDevice() (I2C fingerprint of the X3-only gauge/RTC/IMU)
// before any scene, service, or SD/display bring-up runs. false = X4, the
// SDK's conservative pre-detection default. One universal update.bin serves
// both devices — a compile-time panel choice bricked testers who SD-flashed
// the other device's image (both zips ship a file named update.bin).
extern bool gDeviceIsX3;
