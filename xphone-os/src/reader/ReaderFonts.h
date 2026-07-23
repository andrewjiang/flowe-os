#pragma once
#include <EpdFontFamily.h>

namespace reader {

// One serif family, three sizes (the R1 typography surface).
// fontId doubles as the section.bin cache key: 0=12pt, 1=14pt, 2=16pt.
constexpr int kReaderFontCount = 3;

// Returns the EpdFontFamily for a reader fontId (clamped to a valid id).
// Families are flash-resident statics defined in ReaderFonts.cpp — the ONLY
// reader translation unit that includes the builtin notoserif data headers.
const EpdFontFamily& readerFontFamily(int fontId);

}  // namespace reader
