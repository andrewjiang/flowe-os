#pragma once

// xphone-os M2 — tiny local stand-in for the x4-os couplings the ported
// companion BLE/ANCS sources expect (x4-os <Logging.h> LOG_* macros; the
// CrossPointState/CrossPointSettings singletons are simply not referenced by
// the two companion translation units, so no state shim is needed).
//
// xphone-os logs straight to Serial (see src/main.cpp boot()); map the x4-os
// macros onto Serial.printf with the same "tag: message" shape.

#include <Arduino.h>

#define XP_LOG(level, tag, fmt, ...) Serial.printf("[" level "][" tag "] " fmt "\n", ##__VA_ARGS__)

#define LOG_INF(tag, fmt, ...) XP_LOG("I", tag, fmt, ##__VA_ARGS__)
#define LOG_ERR(tag, fmt, ...) XP_LOG("E", tag, fmt, ##__VA_ARGS__)
#define LOG_DBG(tag, fmt, ...) XP_LOG("D", tag, fmt, ##__VA_ARGS__)
