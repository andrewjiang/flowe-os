#pragma once

// xphone-os M3 — Today: the day at a glance (agenda + reminders + weather),
// synced from the iPhone as a companion BLE "today.snapshot" card.
//
// Port of the x4-os TodayActivity card contract (x4-os
// src/activities/companion/TodayActivity.cpp:13-15 isTodayCard) onto the
// xphone scene framework. The wire format is the generic companion card JSON
// already parsed byte-identically in CompanionBleService::applyCardPayload:
// flat "weather"/"highLow"/"sync" strings plus a unified "items" array of
// [kind, time, title, subtitle, state] rows (kind "reminder" selects the
// Reminders section here; x4-os renders one unified list — the split is this
// scene's X3 layout, the data is untouched). There is NO iOS today producer
// yet; the scene renders whatever snapshot lands and its SYNC key sends the
// forward-compatible "today.sync.request" command (unknown types fall
// through the iOS dispatch harmlessly).
//
// Controls, same mapping as Notifications/Priorities: front Left/Right and
// the top-edge Up/Down pair scroll when the content overflows, CONFIRM
// (SYNC) re-requests the snapshot, BACK returns to the launcher. Renders
// only when dirty; main.cpp marks the scene dirty on TODAY_STORE revision
// changes (fresh snapshots) and companion card/status revision changes (the
// header's status line) while this scene is on glass.
//
// All card state lives in TODAY_STORE (fixed buffers, filled by the
// companion service for every snapshot — even ones that arrive while another
// scene is on glass); the scene keeps only scroll/status-message state.

#include "../Scene.h"

class TodayScene : public Scene {
 public:
  void onEnter() override;
  void handleInput(Input& in) override;
  void render(Gfx& gfx) override;
  const char* const* softKeys() const override;  // BACK / SYNC / UP / DOWN

 private:
  // Flattened render rows (day dividers, the reminders header, items) so the
  // scroll offset has one honest index space. Worst case is 6 events each in a
  // distinct day bucket (6 dividers + 6 items) or a mix with the reminders
  // header — bounded by 6 items + <=6 dividers + 1 header.
  static constexpr int MAX_ROWS = 14;
  // DayDivider carries an event's day-bucket label (TONIGHT/TOMORROW/...) via
  // its item index (the divider reads item.subtitle). SectionReminders is the
  // single "Reminders" header. Item is an event or reminder row.
  enum class RowType : uint8_t { DayDivider, SectionReminders, Item };
  struct Row {
    RowType type;
    int8_t item;  // TODAY_STORE index for Item/DayDivider, else -1
  };

  int buildRows(Row (&rows)[MAX_ROWS]) const;
  void requestSync();
  XpRect contentRect() const;

  int _scroll = 0;               // first visible flattened row
  int _maxScrollCache = 0;       // from the last render (content overflow)
  const char* _localMsg = "Sync today from iPhone.";  // static literals only
  uint32_t _seenStoreRevision = 0;  // TODAY_STORE.revision() last consumed by render()
  int16_t _wCache = 0, _hCache = 0;  // panel dims cached by render()
};
