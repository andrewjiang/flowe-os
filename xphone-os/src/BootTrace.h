#pragma once

// Breadcrumbs for a device that hangs where nobody can attach a serial
// cable. Armed ONLY when /boot-trace.txt already exists on the card: the
// owner creates an empty file with that name, reproduces the hang, pulls
// the card, and the last line names the step that never finished.
//
// It began as a boot-only tracer for the X3 that never painted (issue #33).
// The reader needs it for the same reason: "I go into Read and it freezes,
// no button works, I have to press reset" (oky_doodle, 2026-08-29) is a
// report we cannot act on, and the shelf does a great deal of synchronous
// SD work — scanning the card, reading each package header, extracting
// cover art — any step of which can stall for a long time on a slow or
// failing card.
//
// Cost when disarmed: one boolean test. Cost when armed: one small append
// per call, which is itself slow on the card being diagnosed. That is the
// trade, and it is why this is never on by default.
void xpTrace(const char* stage);
