#pragma once
// Minimal <Logging.h> shim for the vendored x4-os libs (ZipFile.cpp and
// FontDecompressor.cpp include it). This is NOT the x4-os lib/Logging (which
// carries an RTC crash-log ring and a Serial-wrapping MySerialImpl that
// redefines `Serial`); it just maps the LOG_* macros onto xphone-os's plain
// Serial.printf "[xphone-os]" idiom (see src/SdUpdate.cpp, src/main.cpp).
//
// LOG_DBG expands to nothing (same as x4-os builds with LOG_LEVEL < 2) so
// debug format strings don't cost flash.

#include <Arduino.h>

#define LOG_ERR(origin, format, ...) Serial.printf("[xphone-os] %s ERR: " format "\n", origin, ##__VA_ARGS__)
#define LOG_INF(origin, format, ...) Serial.printf("[xphone-os] %s: " format "\n", origin, ##__VA_ARGS__)
#define LOG_DBG(origin, format, ...)
