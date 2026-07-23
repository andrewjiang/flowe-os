#include "TextFold.h"

#include <Utf8.h>

namespace reader {
namespace {

// Romanize one Greek codepoint (monotonic + polytonic) to Latin. Returns the
// Latin string, or nullptr if `cp` is not in a Greek block. Accents/breathings
// are dropped to the base letter — the device fonts have no Greek and the goal
// is a pronounceable transliteration, not a scholarly one. Iota subscripts and
// standalone diacritics fold to "".
const char* greekToLatin(const uint32_t cp) {
  // Greek and Coptic (0x0370-0x03FF): the letters we care about.
  switch (cp) {
    // Uppercase
    case 0x0391: return "A";  case 0x0392: return "B";  case 0x0393: return "G";
    case 0x0394: return "D";  case 0x0395: return "E";  case 0x0396: return "Z";
    case 0x0397: return "E";  case 0x0398: return "Th"; case 0x0399: return "I";
    case 0x039A: return "K";  case 0x039B: return "L";  case 0x039C: return "M";
    case 0x039D: return "N";  case 0x039E: return "X";  case 0x039F: return "O";
    case 0x03A0: return "P";  case 0x03A1: return "R";  case 0x03A3: return "S";
    case 0x03A4: return "T";  case 0x03A5: return "Y";  case 0x03A6: return "Ph";
    case 0x03A7: return "Ch"; case 0x03A8: return "Ps"; case 0x03A9: return "O";
    // Accented uppercase vowels
    case 0x0386: return "A";  case 0x0388: return "E";  case 0x0389: return "E";
    case 0x038A: return "I";  case 0x038C: return "O";  case 0x038E: return "Y";
    case 0x038F: return "O";
    // Lowercase
    case 0x03B1: return "a";  case 0x03B2: return "b";  case 0x03B3: return "g";
    case 0x03B4: return "d";  case 0x03B5: return "e";  case 0x03B6: return "z";
    case 0x03B7: return "e";  case 0x03B8: return "th"; case 0x03B9: return "i";
    case 0x03BA: return "k";  case 0x03BB: return "l";  case 0x03BC: return "m";
    case 0x03BD: return "n";  case 0x03BE: return "x";  case 0x03BF: return "o";
    case 0x03C0: return "p";  case 0x03C1: return "r";  case 0x03C2: return "s";
    case 0x03C3: return "s";  case 0x03C4: return "t";  case 0x03C5: return "y";
    case 0x03C6: return "ph"; case 0x03C7: return "ch"; case 0x03C8: return "ps";
    case 0x03C9: return "o";
    // Accented lowercase vowels (monotonic tonos + dialytika)
    case 0x03AC: return "a";  case 0x03AD: return "e";  case 0x03AE: return "e";
    case 0x03AF: return "i";  case 0x03CC: return "o";  case 0x03CD: return "y";
    case 0x03CE: return "o";  case 0x03CA: return "i";  case 0x03CB: return "y";
    case 0x0390: return "i";  case 0x03B0: return "y";
    default: break;
  }
  if (cp >= 0x0370 && cp <= 0x03FF) return "";  // other Greek marks/archaic — drop

  // Greek Extended (0x1F00-0x1FFF): base vowel + breathing/accent/iota. The rows
  // are grouped by base letter, so reduce by range to the base romanization.
  if (cp >= 0x1F00 && cp <= 0x1FFF) {
    if (cp >= 0x1F00 && cp <= 0x1F0F) return "a";  // alpha
    if (cp >= 0x1F10 && cp <= 0x1F1F) return "e";  // epsilon
    if (cp >= 0x1F20 && cp <= 0x1F2F) return "e";  // eta
    if (cp >= 0x1F30 && cp <= 0x1F3F) return "i";  // iota
    if (cp >= 0x1F40 && cp <= 0x1F4F) return "o";  // omicron
    if (cp >= 0x1F50 && cp <= 0x1F5F) return "y";  // upsilon
    if (cp >= 0x1F60 && cp <= 0x1F6F) return "o";  // omega
    if (cp >= 0x1F70 && cp <= 0x1F71) return "a";  // alpha varia/oxia
    if (cp >= 0x1F72 && cp <= 0x1F73) return "e";  // epsilon
    if (cp >= 0x1F74 && cp <= 0x1F75) return "e";  // eta
    if (cp >= 0x1F76 && cp <= 0x1F77) return "i";  // iota
    if (cp >= 0x1F78 && cp <= 0x1F79) return "o";  // omicron
    if (cp >= 0x1F7A && cp <= 0x1F7B) return "y";  // upsilon
    if (cp >= 0x1F7C && cp <= 0x1F7D) return "o";  // omega
    if (cp >= 0x1F80 && cp <= 0x1FAF) {            // vowels with iota subscript
      const uint32_t row = cp & 0xF0;
      if (row == 0x80 || row == 0x90) return "a";  // alpha
      if (row == 0xA0) return "o";                 // omega (0x1FA0-0x1FAF)
      return "e";                                  // eta (0x1F90-0x1F9F)
    }
    if (cp >= 0x1FB0 && cp <= 0x1FBC) return "a";  // alpha
    if (cp >= 0x1FC0 && cp <= 0x1FCC) return "e";  // eta
    if (cp >= 0x1FD0 && cp <= 0x1FDF) return "i";  // iota
    if (cp >= 0x1FE0 && cp <= 0x1FE4) return "y";  // upsilon
    if (cp == 0x1FE5 || cp == 0x1FEC) return "r";  // rho with dasia
    if (cp >= 0x1FE6 && cp <= 0x1FEF) return "y";  // upsilon
    if (cp >= 0x1FF0 && cp <= 0x1FFF) return "o";  // omega
    return "";  // stray Extended diacritic — drop
  }
  return nullptr;  // not Greek
}

}  // namespace

bool codepointNeedsFold(const uint32_t cp) {
  // Return false ONLY for ranges that are byte-identical passthrough (font
  // covers them, no remap), so a false result lets the caller skip decoding.
  // Everything else — Greek (0x0370-0x03FF), Greek Extended (0x1F00-0x1FFF),
  // symbols, emoji — returns true and is inspected by appendFoldedCodepoint
  // (which still passes through whatever it can't map).
  if (cp < 0x0370) return false;                     // ASCII, Latin-1, Latin Ext, IPA, combining
  if (cp >= 0x0400 && cp <= 0x04FF) return false;    // Cyrillic
  if (cp >= 0x2012 && cp <= 0x2064) return false;    // en/em dash, quotes, ellipsis, bullet, sub/superscript
  if (cp >= 0x20A0 && cp <= 0x20C0) return false;    // currency symbols
  return true;
}

void appendFoldedCodepoint(const uint32_t cp, std::string& out) {
  if (!codepointNeedsFold(cp)) {
    utf8AppendCodepoint(cp, out);
    return;
  }

  // Greek -> Latin.
  if (const char* rom = greekToLatin(cp)) {
    out += rom;  // "" for pure marks; drops them
    return;
  }

  // Curated symbol remaps for glyphs the font lacks.
  switch (cp) {
    case 0x2122: out += "(TM)"; return;  // ™ trademark
    case 0x2120: out += "(SM)"; return;  // ℠ service mark
    case 0x2117: out += "(P)";  return;  // ℗ sound recording copyright
    case 0x2190: out += "<-";   return;  // ← left arrow
    case 0x2192: out += "->";   return;  // → right arrow
    case 0x2194: out += "<->";  return;  // ↔ left-right arrow
    case 0x21D2: out += "=>";   return;  // ⇒ rightwards double arrow
    case 0x21D0: out += "<=";   return;  // ⇐ leftwards double arrow
    case 0x2191: out += '^';    return;  // ↑ up arrow
    case 0x2193: out += 'v';    return;  // ↓ down arrow
    case 0x2248: out += '~';    return;  // ≈ almost equal
    case 0x2260: out += "!=";   return;  // ≠ not equal
    case 0x2264: out += "<=";   return;  // ≤ less-or-equal
    case 0x2265: out += ">=";   return;  // ≥ greater-or-equal
    case 0x2219: out += '*';    return;  // ∙ bullet operator
    case 0x2605:                          // ★ black star
    case 0x2606: out += '*';    return;  // ☆ white star
    case 0x2665:                          // ♥ black heart
    case 0x2764: out += "<3";   return;  // ❤ heavy black heart
    case 0x2713:                          // ✓ check mark
    case 0x2714: out += 'v';    return;  // ✔ heavy check mark
    default: break;
  }

  // Drop whole symbol/emoji planes — a gap reads better than a box.
  if ((cp >= 0x1F000 && cp <= 0x1FAFF) ||  // emoji, pictographs, symbols & pictographs
      (cp >= 0x2300 && cp <= 0x27BF) ||    // misc technical, misc symbols, dingbats
      (cp >= 0x2B00 && cp <= 0x2BFF) ||    // misc symbols and arrows
      (cp >= 0x2500 && cp <= 0x25FF) ||    // box drawing + geometric shapes
      (cp >= 0xFE00 && cp <= 0xFE0F) ||    // variation selectors (emoji styling)
      cp == 0x200D || cp == 0x2060) {      // ZWJ / word joiner
    return;
  }

  // Everything else: pass through. The font covers most remaining ranges
  // (currency, ligatures, Vietnamese); a genuinely rare uncovered glyph still
  // falls back to the renderer's U+FFFD box, which is now a rare exception.
  utf8AppendCodepoint(cp, out);
}

}  // namespace reader
