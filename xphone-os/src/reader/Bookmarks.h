#pragma once

// Reader bookmarks (0.7 workstream I): up to 16 saved places per book,
// in a "<book path>.bmk" sidecar next to the book — the same convention
// FbpBook uses for ".pos", so a deleted book takes its marks with it and
// the phone-side sync never has to know about them.
//
// A mark is stored as {kind, value}: FBP books mark a PAGE (stable —
// pages are pre-paginated per profile, and the compiler's content IDs
// make the page reproducible), epubs mark {spine, page}. Marks are
// deliberately dumb: no titles, no notes. The list shows where they
// land, and the reader jumps there.

#include <cstdint>

namespace reader {

class Bookmarks {
 public:
  static constexpr int kMax = 16;

  struct Mark {
    uint16_t spine;  // epub chapter index; 0 for FBP
    uint16_t page;   // FBP page, or page within the epub chapter
  };

  // Load/save are whole-file: 16 marks is 64 bytes.
  static int load(const char* bookPath, Mark* out, int cap);
  static bool save(const char* bookPath, const Mark* marks, int count);

  // Add unless an identical mark exists (idempotent bookmarking); drops
  // the oldest when full. Returns the new count, or -1 on write failure.
  static int add(const char* bookPath, Mark m);
  // Remove by index; returns the new count or -1.
  static int remove(const char* bookPath, int index);
};

}  // namespace reader
