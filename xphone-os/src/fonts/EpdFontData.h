#pragma once
// Shim: this header used to be a symlink to the x4-os copy of EpdFontData.h.
// The vendored lib/EpdFontCore ships the SAME file (byte-identical), and any
// TU that mixes the UI text stack (Gfx.h -> this header) with the reader
// engine (EpdFontFamily.h -> lib/EpdFontCore/EpdFontData.h) would otherwise
// see two distinct-by-path definitions of EpdGlyph/EpdFontData/fp4 — a
// redefinition error (#pragma once keys on the file, not its contents).
// Route everyone to the single vendored copy instead.
#include <EpdFontData.h>
