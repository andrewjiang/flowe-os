#pragma once
#include <Arduino.h>

// xphone-os logs with Serial.printf("[module] ...") — see src/main.cpp.
// These macros keep the CrossPoint LOG_* call sites intact in the ported
// engine while routing to that idiom with a [reader:TAG] prefix.
// (x4-os Logging.h is not vendored; xphone-os has no LOG_* macros of its own.)
#ifndef LOG_ERR
#define LOG_ERR(tag, fmt, ...) Serial.printf("[reader:" tag "] ERR " fmt "\n", ##__VA_ARGS__)
#endif
#ifndef LOG_DBG
#define LOG_DBG(tag, fmt, ...) Serial.printf("[reader:" tag "] " fmt "\n", ##__VA_ARGS__)
#endif
#ifndef LOG_INF
#define LOG_INF(tag, fmt, ...) Serial.printf("[reader:" tag "] " fmt "\n", ##__VA_ARGS__)
#endif
