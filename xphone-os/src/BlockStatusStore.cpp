#include "BlockStatusStore.h"

#include <cstdio>
#include <string>

#include "ble/CompanionProtocol.h"

BlockStatusStore BLOCK_STATUS;

// The formatted end-time buffer is small on purpose (see the header); keep the
// size assertion here so a future widening is a deliberate edit.
static_assert(sizeof(BlockStatusStore::Status::endsAtLabel) == 16, "endsAtLabel field size drifted");

namespace {
// Digit scrape of the card title's "<N> min" prefix — the pre-latch fallback
// when the phone omits remainingMinutes (mirrors BlockScene.cpp cardRemaining).
int remainingFromTitle(const std::string& title) {
  const auto minPos = title.find(" min");
  if (minPos == std::string::npos) return 0;
  int value = 0;
  for (std::size_t i = 0; i < minPos; ++i) {
    if (title[i] >= '0' && title[i] <= '9') value = value * 10 + (title[i] - '0');
  }
  return value;
}
}  // namespace

// Predicate mirrors BlockScene.cpp isActiveBlockCard/isBreakCard so the store
// and the Block scene interpret the same iOS card identically.
void BlockStatusStore::updateFromCard(const CompanionCardState& card) {
  bool active = false;
  if (card.state == "active" || card.state == "break") {
    active = true;
  } else if (card.title.find("min left") != std::string::npos || card.title.find("break") != std::string::npos ||
             card.body.find("min left") != std::string::npos || card.body.find("paused") != std::string::npos) {
    active = true;
  }
  const bool onBreak = (card.state == "break") || card.body.find("paused") != std::string::npos;

  int remaining = card.remainingMinutes;
  if (remaining <= 0) remaining = remainingFromTitle(card.title);

  _status.active = active;
  _status.onBreak = onBreak;
  _status.fromCard = true;
  _status.ready = (card.state == "ready");
  _status.remainingMinutes = remaining;
  _status.durationMinutes = card.durationMinutes;
  // snprintf clips + always null-terminates (strncpy does not).
  snprintf(_status.preset, sizeof(_status.preset), "%s", card.preset.c_str());
  snprintf(_status.endsAtLabel, sizeof(_status.endsAtLabel), "%s", card.endsAtLabel.c_str());
  _status.blocksToday = card.blocksToday;
  _status.streak = card.blockStreak;
  _status.total = card.blocksTotal;
  _status.revision++;
}

void BlockStatusStore::seedCounts(int blocksToday, int streak, int total) {
  _status.blocksToday = blocksToday;
  _status.streak = streak;
  _status.total = total;
  _status.revision++;
}

// Seed from the NVS snapshot at boot (Sleep::seedPersistedBlock). Same field
// shape as updateFromCard but sourced from persisted primitives, not a card.
void BlockStatusStore::seedFromPersisted(bool active, bool onBreak, int remainingMinutes, int durationMinutes,
                                         const char* preset, const char* endsAtLabel) {
  _status.active = active;
  _status.onBreak = onBreak;
  _status.fromCard = false;  // a seed is not a card: no countdown, loses to real cards
  _status.ready = false;
  _status.remainingMinutes = remainingMinutes;
  _status.durationMinutes = durationMinutes;
  snprintf(_status.preset, sizeof(_status.preset), "%s", preset ? preset : "");
  snprintf(_status.endsAtLabel, sizeof(_status.endsAtLabel), "%s", endsAtLabel ? endsAtLabel : "");
  _status.revision++;
}
