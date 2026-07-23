#pragma once

// Header transfer glyph shared by the Block/Workout/Priorities headers.
//
// The companion status line used to print the raw BLE service message
// ("Command sent", "Card received", ...) in the header's top-right corner.
// Transfer transients are now drawn as the classic paired up/down transfer
// arrows instead (the phone-status-bar style glyph): the ACTIVE direction
// flashes as a solid bold arrow for ~1 s, then the pair settles back to the
// thin idle outlines — otherwise a lingering bold arrow reads as a transfer
// that never finishes. Non-transfer messages (connect hints, errors) still
// render as text — the caller falls back when draw() returns false.
//
// The flash is driven by the scenes' existing 10 ms input tick: call tick()
// there and repaint the header when it returns true. State is a single
// shared singleton (one scene is on glass at a time, and the BLE status
// rail is global).

#include <cstring>
#include <string>

#include "Gfx.h"

namespace SyncIndicator {

enum class Direction { None, Up, Down };

// "... sent" transients (Command/Block command/Action/Priority update/
// Workout update) are uploads; "Card received" is the download ack.
inline Direction classify(const char* msg) {
  if (!msg || !msg[0]) return Direction::None;
  const size_t n = strlen(msg);
  constexpr const char* kSent = " sent";
  constexpr const char* kReceived = " received";
  const size_t sentLen = strlen(kSent);
  const size_t recvLen = strlen(kReceived);
  if (n >= sentLen && strcmp(msg + n - sentLen, kSent) == 0) return Direction::Up;
  if (n >= recvLen && strcmp(msg + n - recvLen, kReceived) == 0) return Direction::Down;
  if (strcmp(msg, "Sent") == 0) return Direction::Up;
  if (strcmp(msg, "Received") == 0) return Direction::Down;
  return Direction::None;
}

// How long the active direction stays bold after a transient lands.
constexpr uint32_t kBoldMs = 1000;

struct State {
  uint32_t seenRevision = 0;  // BLE status revision last classified
  uint32_t boldUntilMs = 0;
  bool boldActive = false;
  Direction dir = Direction::None;
};
inline State& state() {
  static State s;
  return s;
}

// Scene tick (10 ms loop): latch NEW transfer transients off the service's
// status revision and expire the bold flash kBoldMs later. `getMsg` is only
// invoked on a revision change, so the steady-state tick does no heap copy.
// Returns true when the caller should repaint its header (bold appeared or
// expired).
template <typename MsgFn>
inline bool tick(const uint32_t revision, const uint32_t nowMs, MsgFn getMsg) {
  State& s = state();
  bool repaint = false;
  if (revision != s.seenRevision) {
    s.seenRevision = revision;
    const std::string msg = getMsg();
    const Direction d = classify(msg.c_str());
    if (d != Direction::None) {
      s.dir = d;
      s.boldUntilMs = nowMs + kBoldMs;
      s.boldActive = true;
      repaint = true;
    }
  }
  if (s.boldActive && static_cast<int32_t>(nowMs - s.boldUntilMs) >= 0) {
    s.boldActive = false;
    repaint = true;
  }
  return repaint;
}

// One vertical arrow: `cx` is the stem's center column, the arrow spans
// y..y+h-1. Active = 3px strokes, inactive = 1px (thin ghost of the pair).
inline void drawArrow(Gfx& gfx, const int cx, const int y, const int h, const bool up, const bool active) {
  const int t = active ? 3 : 1;
  const int head = active ? 5 : 4;  // head half-width AND head depth
  const int tipY = up ? y : y + h - 1;
  const int baseY = up ? y + h - 1 : y;
  const int headY = up ? y + head : y + h - 1 - head;
  gfx.drawLine(cx, tipY, cx, baseY, t, true);
  gfx.drawLine(cx, tipY, cx - head, headY, t, true);
  gfx.drawLine(cx, tipY, cx + head, headY, t, true);
}

// Draws the paired transfer glyph right-aligned so it ends at `rightX`,
// vertically centered on the text line starting at `y` with height `lineH`.
// The active direction is bold only while the tick()-managed flash window is
// open. Returns false when `msg` is not a transfer transient (caller draws
// the message as text).
inline bool draw(Gfx& gfx, const int rightX, const int y, const int lineH, const char* msg) {
  const Direction dir = classify(msg);
  if (dir == Direction::None) return false;
  const State& s = state();
  const bool bold = s.boldActive;
  constexpr int kArrowH = 18;
  constexpr int kPitch = 14;  // column spacing between the two stems
  const int top = y + (lineH - kArrowH) / 2;
  const int downCx = rightX - 6;         // down arrow on the right
  const int upCx = downCx - kPitch;      // up arrow on the left
  drawArrow(gfx, upCx, top, kArrowH, /*up=*/true, bold && dir == Direction::Up);
  drawArrow(gfx, downCx, top, kArrowH, /*up=*/false, bold && dir == Direction::Down);
  return true;
}

}  // namespace SyncIndicator
