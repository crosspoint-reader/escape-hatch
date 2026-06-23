#pragma once

// Tiny logging shim so the ported flash code (FirmwareFlasher / OtaBootSwitch)
// keeps its LOG_* calls without dragging in CrossPoint's full Logging library.
// Everything routes to Serial; compiles away to nothing if Serial is closed.

#include <Arduino.h>

#define LOG_INF(tag, fmt, ...)                                       \
  do {                                                               \
    if (Serial) Serial.printf("[%lu][" tag "] " fmt "\n", millis(), ##__VA_ARGS__); \
  } while (0)
#define LOG_DBG(tag, fmt, ...) LOG_INF(tag, fmt, ##__VA_ARGS__)
#define LOG_ERR(tag, fmt, ...) LOG_INF(tag, fmt, ##__VA_ARGS__)
