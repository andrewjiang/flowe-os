#pragma once

// xphone-os M4.2 — durable block-status store.
//
// NotificationStore/PrioritiesStore/TodayStore mold applied to the Block
// status card: a tiny fixed struct capturing the latest "block-status" card
// the iPhone pushes. It exists because the companion service keeps exactly ONE
// card slot (CompanionBleService::card), and priorities.snapshot / today.snapshot
// pushes clobber it — so main.cpp checkAutoSleep() and the dormant sleep frame
// cannot rely on getCard() still holding the block card when they need it. The
// service fills this store UNCONDITIONALLY for every block-status card
// (CompanionBleService.cpp applyCardPayload), so the "is a block running?"
// answer survives any number of intervening card pushes.
//
// applyCardPayload runs ONLY on the Arduino main loop (handleCardWrite on the
// NimBLE host task just stashes raw bytes; processPending() parses them from
// loop()/Sleep.cpp), and every reader (checkAutoSleep, Sleep::sleepNow,
// PrioritiesScene::renderDormant) runs on the main loop too, so every accessor
// here is main-loop-only by construction: no mutex needed.

#include <cstddef>
#include <cstdint>

struct CompanionCardState;

class BlockStatusStore {
 public:
  // Fixed snapshot of the latest block-status card. endsAtLabel holds the
  // iPhone-formatted end time ("10:30 AM") from the card's "endsAtLabel" field;
  // 16 bytes fits "12:59 PM" plus slack and keeps the struct trivially copyable.
  struct Status {
    bool active = false;            // block running (state active/break or heuristics)
    bool onBreak = false;          // block paused on a break
    // Provenance: true = a live block-status card from the phone; false = the
    // boot/wake NVS seed. BlockScene needs the distinction — a seed has no
    // elapsed-time info (no RTC), so it must render the absolute end label,
    // never a countdown, and must lose to any real card.
    bool fromCard = false;
    bool ready = false;            // card state == "ready" (preset view); seeds never set it
    int remainingMinutes = 0;      // countdown minutes the phone reported
    int durationMinutes = 0;       // total block minutes the phone reported
    char preset[24] = {0};         // preset title ("Deep Work"), "" when omitted
    char endsAtLabel[16] = {0};    // formatted end time, "" when the phone omits it
    // Completion tracking (from the block-status card; persisted across sleep so
    // the dormant frame can show today's count regardless of active state).
    int blocksToday = 0;           // blocks completed today
    int streak = 0;                // consecutive completions (0 after an early stop)
    int total = 0;                 // all-time completed blocks
    uint32_t revision = 0;         // bumped on every updateFromCard()/seedFromPersisted()
  };

  // Copy the block-status card's derived fields into the fixed snapshot and
  // bump the revision. Call ONLY for a block-status card — the service's
  // predicate (id == "block-status") decides.
  void updateFromCard(const CompanionCardState& card);

  // M4.3 wake-from-active-block: seed the store from the NVS snapshot persisted
  // at the last sleep, BEFORE BLE reconnects. Lets BlockScene render the locked
  // ("active") view instantly on wake using the ABSOLUTE end-time label (no RTC
  // on X3/X4, so a persisted minute countdown would be wrong). A fresh
  // block-status card after reconnect supersedes this via updateFromCard().
  void seedFromPersisted(bool active, bool onBreak, int remainingMinutes, int durationMinutes, const char* preset,
                         const char* endsAtLabel);

  // Seed the completion counters from NVS at boot (independent of active state,
  // so today's block count shows on the dormant frame even with no live block).
  // A fresh block-status card supersedes these via updateFromCard().
  void seedCounts(int blocksToday, int streak, int total);

  // Copy-out accessor (whole struct, including the revision counter).
  Status get() const { return _status; }
  // Convenience predicate for the hot paths that only need "is a block on?".
  bool active() const { return _status.active; }
  // Polled like the other stores' revisions if a consumer wants change events.
  uint32_t revision() const { return _status.revision; }

 private:
  Status _status;
};

extern BlockStatusStore BLOCK_STATUS;
