#pragma once

#include <cstddef>
#include <cstdint>

// The device notices its own freeze, so nobody has to prepare for one.
//
// The breadcrumb file that came before this had to be armed BEFORE the
// problem: the owner created /boot-trace.txt, reproduced the freeze, pulled
// the card. That is a lot to ask of someone whose device just stopped
// answering ("I go into Read and it freezes, I had to press the reset
// button" — oky_doodle, 2026-08-29), and it only works if they still have
// the freeze in front of them.
//
// This watches instead. The main loop ticks a counter; a small task checks
// it twice a second. When the counter stops moving, the loop is stuck in
// something — an SD read on a failing card, a panel wait that never
// finishes — and the watcher writes WHERE into RTC memory, which is plain
// RAM the reset button does not clear. The next boot finds it and says so.
//
// It only ever writes RTC RAM, never flash and never the card, so it cannot
// make a struggling device worse. The report reaches the card on the next
// boot, at a moment when nothing is stuck.
namespace stallwatch {

// Start the watcher. Safe to call once, after Serial is up.
void begin();

// Called from loop(). One increment; that is the whole cost.
void beat();

// Name what is happening now. Costs a short copy. The pointer need not
// outlive the call. Anything longer than the buffer is truncated.
void stage(const char* what);

// The longest stall the PREVIOUS run suffered, or false if it ran clean.
// Clears the record, so it reports once. Call early in boot.
bool takeReport(char* out, size_t outCap);

// The longest stall in THIS run so far, in ms (0 when nothing has stalled).
// Lets a live device answer "is this card slow?" without a reboot.
uint32_t worstStallMs();

}  // namespace stallwatch
