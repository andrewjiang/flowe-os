#pragma once

// Phase 0 of the morning-meditation spec (docs/plans/morning-meditation-spec.md):
// the phone is the clock — this hardware has no RTC and the X3 power-button
// wake wipes software time. The iOS app pushes a "time.sync" card right after
// the BLE card writer is ready; this store stamps when the first connect and
// first date arrived (millis(), which starts ~0 at boot) so the About scene
// can show wake→date latency without a serial cable.
//
// Write paths: firstConnectMs from the NimBLE host task (markConnected),
// sync fields from the main-loop card parse. All fields are single 32/16-bit
// aligned scalars — atomic on the C3, diagnostic-grade, no mutex.

#include <cstdint>

struct ClockStore {
  uint32_t firstConnectMs = 0;  // millis() at first BLE connect since boot (0 = none yet)
  uint32_t firstSyncMs = 0;     // millis() when the first time.sync parsed (0 = none yet)
  uint32_t day = 0;             // local yyyymmdd from the latest time.sync
  uint16_t minutesIntoDay = 0;  // local minutes since midnight at receipt
};

extern ClockStore CLOCK_STORE;
