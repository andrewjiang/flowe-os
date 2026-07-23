// Mirrors x4-os src/main.cpp:40-115 font setup for the notoserif family at
// 12/14/16 pt (4 styles each). Include paths match the vendored lib/EpdFontCore
// builtinFonts layout (same names as x4-os lib/EpdFont/builtinFonts/).
#include "ReaderFonts.h"

#include <EpdFont.h>
#include <builtinFonts/notoserif_12_bold.h>
#include <builtinFonts/notoserif_12_bolditalic.h>
#include <builtinFonts/notoserif_12_italic.h>
#include <builtinFonts/notoserif_12_regular.h>
#include <builtinFonts/notoserif_14_bold.h>
#include <builtinFonts/notoserif_14_bolditalic.h>
#include <builtinFonts/notoserif_14_italic.h>
#include <builtinFonts/notoserif_14_regular.h>
#include <builtinFonts/notoserif_16_bold.h>
#include <builtinFonts/notoserif_16_bolditalic.h>
#include <builtinFonts/notoserif_16_italic.h>
#include <builtinFonts/notoserif_16_regular.h>

namespace reader {
namespace {

const EpdFont kSerif12Regular(&notoserif_12_regular);
const EpdFont kSerif12Bold(&notoserif_12_bold);
const EpdFont kSerif12Italic(&notoserif_12_italic);
const EpdFont kSerif12BoldItalic(&notoserif_12_bolditalic);
const EpdFontFamily kSerif12(&kSerif12Regular, &kSerif12Bold, &kSerif12Italic, &kSerif12BoldItalic);

const EpdFont kSerif14Regular(&notoserif_14_regular);
const EpdFont kSerif14Bold(&notoserif_14_bold);
const EpdFont kSerif14Italic(&notoserif_14_italic);
const EpdFont kSerif14BoldItalic(&notoserif_14_bolditalic);
const EpdFontFamily kSerif14(&kSerif14Regular, &kSerif14Bold, &kSerif14Italic, &kSerif14BoldItalic);

const EpdFont kSerif16Regular(&notoserif_16_regular);
const EpdFont kSerif16Bold(&notoserif_16_bold);
const EpdFont kSerif16Italic(&notoserif_16_italic);
const EpdFont kSerif16BoldItalic(&notoserif_16_bolditalic);
const EpdFontFamily kSerif16(&kSerif16Regular, &kSerif16Bold, &kSerif16Italic, &kSerif16BoldItalic);

}  // namespace

const EpdFontFamily& readerFontFamily(const int fontId) {
  switch (fontId) {
    case 0:
      return kSerif12;
    case 2:
      return kSerif16;
    case 1:
    default:
      return kSerif14;
  }
}

}  // namespace reader
