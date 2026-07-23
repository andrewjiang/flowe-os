#include "TodayStore.h"

#include <cstdio>
#include <cstring>

#include "ble/CompanionProtocol.h"

TodayStore TODAY_STORE;

// Keep the fixed fields sized to what the wire protocol can deliver (see the
// header comment) — a protocol bump must grow the store with it.
static_assert(TodayStore::CAPACITY == CompanionProtocol::MAX_TODAY_ITEMS, "store capacity != protocol");
static_assert(sizeof(TodayStore::Item::kind) == CompanionProtocol::MAX_ID_CHARS + 1, "kind field != protocol");
static_assert(sizeof(TodayStore::Item::time) == CompanionProtocol::MAX_TODAY_FIELD_CHARS + 1,
              "time field != protocol");
static_assert(sizeof(TodayStore::Item::title) == CompanionProtocol::MAX_TITLE_CHARS + 1, "title field != protocol");
static_assert(sizeof(TodayStore::Item::subtitle) == CompanionProtocol::MAX_TODAY_FIELD_CHARS + 1,
              "subtitle field != protocol");

void TodayStore::updateFromCard(const CompanionCardState& card) {
  std::size_t n = card.todayItemCount;
  if (n > CAPACITY) n = CAPACITY;
  for (std::size_t i = 0; i < n; i++) {
    const CompanionTodayItem& src = card.todayItems[i];
    // snprintf clips + always null-terminates (strncpy does not).
    snprintf(_items[i].kind, sizeof(_items[i].kind), "%s", src.kind.c_str());
    snprintf(_items[i].time, sizeof(_items[i].time), "%s", src.time.c_str());
    snprintf(_items[i].title, sizeof(_items[i].title), "%s", src.title.c_str());
    snprintf(_items[i].subtitle, sizeof(_items[i].subtitle), "%s", src.subtitle.c_str());
  }
  _count = n;
  snprintf(_weather, sizeof(_weather), "%s", card.todayWeather.c_str());
  snprintf(_highLow, sizeof(_highLow), "%s", card.todayHighLow.c_str());
  snprintf(_syncLine, sizeof(_syncLine), "%s", card.todaySync.c_str());
  _hasSnapshot = true;
  _revision++;
}

bool TodayStore::get(std::size_t index, Item& out) const {
  if (index >= _count) return false;
  out = _items[index];
  return true;
}

void TodayStore::tally(int& agenda, int& reminders) const {
  agenda = 0;
  reminders = 0;
  for (std::size_t i = 0; i < _count; i++) {
    if (strcmp(_items[i].kind, "reminder") == 0) {
      reminders++;
    } else {
      agenda++;
    }
  }
}
