#pragma once

// xphone-os M2 — companion GATT protocol, copied from x4-os
// src/companion/CompanionProtocol.h so the existing iOS companion app
// connects unchanged (same service/characteristic UUIDs, same card/command
// JSON schema). The camera image transfer state
// (CompanionCameraImageState + MAX_CAMERA_IMAGE_BYTES) is intentionally
// stripped for M2 — camera.image.* writes are acknowledged-and-dropped in
// CompanionBleService.cpp instead of buffered (saves the 22KB image buffer).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace CompanionProtocol {
// Per-build-env BLE identity so an xphone X3 and a real X4 can coexist in
// the same iPhone scan list. The iOS app discovers/reconnects by SERVICE_UUID
// (BluetoothManager.swift:101 scanForPeripherals(withServices:)), not by
// name, so only the human-visible label changes — SERVICE_UUID must never.
#if defined(FREEINK_DEVICE_X4)
constexpr const char* DEVICE_NAME = "xphone X4";
#elif defined(FREEINK_DEVICE_X3)
constexpr const char* DEVICE_NAME = "xphone X3";
#else
constexpr const char* DEVICE_NAME = "xphone";
#endif
constexpr const char* SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char* CARD_WRITE_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char* ACTION_NOTIFY_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

constexpr std::size_t MAX_ACTIONS = 4;
constexpr std::size_t MAX_TODAY_ITEMS = 6;
constexpr std::size_t MAX_MAIL_ITEMS = 8;
constexpr std::size_t MAX_PRIORITY_ITEMS = 10;
constexpr std::size_t MAX_WORKOUT_ITEMS = 8;
constexpr std::size_t MAX_WORKOUT_NAME_CHARS = 48;
constexpr std::size_t MAX_CARD_BYTES = 4096;
constexpr std::size_t MAX_ID_CHARS = 64;
constexpr std::size_t MAX_SOURCE_CHARS = 64;
constexpr std::size_t MAX_TITLE_CHARS = 96;
constexpr std::size_t MAX_BODY_CHARS = 512;
constexpr std::size_t MAX_ACTION_LABEL_CHARS = 32;
constexpr std::size_t MAX_TODAY_FIELD_CHARS = 48;
constexpr std::size_t MAX_MAIL_FIELD_CHARS = 96;
constexpr std::size_t MAX_PRIORITY_FIELD_CHARS = 120;
}  // namespace CompanionProtocol

struct CompanionCardAction {
  std::string id;
  std::string label;
};

struct CompanionTodayItem {
  std::string kind;
  std::string time;
  std::string title;
  std::string subtitle;
  std::string state;
};

// M3 Priorities — one to-do from the iOS "priorities.snapshot" card
// (PrioritiesManager.swift:124-146 sends priorityItems as 4-element arrays
// [id, title, note, done]; struct mirrors x4-os CompanionProtocol.h:54-59).
struct CompanionPriorityItem {
  std::string id;
  std::string title;
  std::string note;
  bool done = false;
};

// Workout — one exercise from the iOS "workout.snapshot" card
// (WorkoutManager.swift sends workoutItems as 4-element arrays
// [id, name, sets, done]). Sets-only model: `done` counts completed sets,
// the exercise is finished when done >= sets. `name` is free text the
// device never parses ("Bench 135lb").
struct CompanionWorkoutItem {
  std::string id;
  std::string name;
  int sets = 0;
  int done = 0;
};

struct CompanionMailItem {
  std::string id;
  std::string from;
  std::string subject;
  std::string preview;
  std::string time;
  std::string state;
};

struct CompanionCardState {
  bool hasCard = false;
  uint32_t revision = 0;
  std::string id;
  std::string kind;
  std::string source;
  std::string title;
  std::string body;
  std::string state;
  std::string preset;
  std::string todayWeather;
  std::string todayHighLow;
  std::string todaySync;
  std::string mailSource;
  std::string mailSync;
  // M4.2 Block: iPhone-formatted block end time ("10:30 AM"); "" on
  // ready/idle/older iOS. Feeds BlockStatusStore + the dormant sleep frame.
  std::string endsAtLabel;
  int durationMinutes = 0;
  int remainingMinutes = 0;
  // Block completion tracking (computed on iOS, additive keys). blocksToday =
  // blocks completed today; blockStreak = consecutive completions (0 after an
  // early stop); blocksTotal = all-time. Older iOS omits them -> 0.
  int blocksToday = 0;
  int blockStreak = 0;
  int blocksTotal = 0;
  std::array<CompanionCardAction, CompanionProtocol::MAX_ACTIONS> actions;
  std::size_t actionCount = 0;
  std::array<CompanionTodayItem, CompanionProtocol::MAX_TODAY_ITEMS> todayItems;
  std::size_t todayItemCount = 0;
  std::array<CompanionMailItem, CompanionProtocol::MAX_MAIL_ITEMS> mailItems;
  std::size_t mailItemCount = 0;
  std::array<CompanionPriorityItem, CompanionProtocol::MAX_PRIORITY_ITEMS> priorityItems;
  std::size_t priorityItemCount = 0;
  // Multi-part snapshot cards: a single GATT write caps a card at ~512 bytes,
  // so iOS splits large priorities snapshots into `parts` cards sharing one
  // card id, each carrying `part` (0-based) and a slice of the items. Older
  // iOS omits both -> a normal single-card snapshot.
  int part = 0;
  int parts = 1;
  std::array<CompanionWorkoutItem, CompanionProtocol::MAX_WORKOUT_ITEMS> workoutItems;
  std::size_t workoutItemCount = 0;
  // Workout card only: the phone's local date ("2026-07-13") the card was
  // built for, so a stale card is labeled honestly after a day rollover.
  std::string workoutDate;
};
