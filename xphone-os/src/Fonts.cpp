// The one and only TU that includes the font data headers.
//
// M2.1c flash diet: the headers are ASCII (U+0020..U+007E) subsets generated
// by tools/subset_epd_font.py from the read-only x4-os builtin fonts (see the
// regeneration command in each header). Non-ASCII codepoints have no glyph
// and render as '?' via the Gfx findGlyph fallback. Only the
// Bitmaps/Glyphs/Intervals arrays are emitted; the originals' kern/ligature
// tables were unreferenced (gc-sections discarded them) and are gone.
//
// lineAdvance/ascender come from the original headers' (discarded)
// EpdFontData initializers: both ubuntu_12 styles are {advanceY=29,
// ascender=24, descender=-5}.

#include "Fonts.h"

#include "fonts/ubuntu_10_regular_ascii.h"
#include "fonts/ubuntu_12_bold_ascii.h"
#include "fonts/ubuntu_12_regular_ascii.h"

const XpFont kFontRegular = {
    ubuntu_12_regularBitmaps,
    ubuntu_12_regularGlyphs,
    ubuntu_12_regularIntervals,
    sizeof(ubuntu_12_regularIntervals) / sizeof(ubuntu_12_regularIntervals[0]),
    29,  // advanceY
    24,  // ascender
};

const XpFont kFontBold = {
    ubuntu_12_boldBitmaps,
    ubuntu_12_boldGlyphs,
    ubuntu_12_boldIntervals,
    sizeof(ubuntu_12_boldIntervals) / sizeof(ubuntu_12_boldIntervals[0]),
    29,  // advanceY
    24,  // ascender
};

// M3 soft-key bar labels. Metrics from the original ubuntu_10_regular.h
// EpdFontData initializer (x4-os builtinFonts): advanceY=24, ascender=20.
const XpFont kFontSmall = {
    ubuntu_10_regularBitmaps,
    ubuntu_10_regularGlyphs,
    ubuntu_10_regularIntervals,
    sizeof(ubuntu_10_regularIntervals) / sizeof(ubuntu_10_regularIntervals[0]),
    24,  // advanceY
    20,  // ascender
};
