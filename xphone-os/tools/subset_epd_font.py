#!/usr/bin/env python3
"""Subset an EpdFont builtin header to the printable-ASCII range (M2.1c flash diet).

Parses the three plain (uncompressed) C arrays of a fontconvert.py-generated
header — <name>Bitmaps (uint8_t), <name>Glyphs (EpdGlyph initializers) and
<name>Intervals (EpdUnicodeInterval initializers) — keeps only codepoints
0x20..0x7E, repacks the bitmap (each kept glyph's [dataOffset,
dataOffset+dataLength) slice concatenated, dataOffsets recomputed) and emits a
header with the SAME three array names plus a single interval {0x20, 0x7E, 0}.
The kern/ligature/EpdFontData tables of the original are intentionally dropped
(they were already unreferenced and gc-section'd away).

Self-check (mandatory, runs on every invocation): after emitting, the emitted
header FILE is re-parsed through a C-faithful reader — translation-phase-2
backslash-newline splicing FIRST, then comment stripping, the order a real
compiler uses — and every codepoint 0x20..0x7E is resolved INDEPENDENTLY
through each side's own interval table (findGlyph semantics), comparing
metrics and bitmap bytes. Prints PASS or dies with an assert.

Why C-faithful matters: a glyph comment label that is a lone `\\` at end of
line splices the NEXT array line into the comment (this deleted the ']'
entry and shifted every glyph after backslash down one index — the "Block"
renders as "Bmpdl" hardware bug). A data-model-only check cannot see that;
re-parsing the emitted text the way GCC does can.

Regeneration (from the xphone-os directory; inputs are the read-only
x4-os/lib/EpdFont/builtinFonts originals):
  python3 tools/subset_epd_font.py ../x4-os/lib/EpdFont/builtinFonts/ubuntu_12_regular.h ubuntu_12_regular src/fonts/ubuntu_12_regular_ascii.h
  python3 tools/subset_epd_font.py ../x4-os/lib/EpdFont/builtinFonts/ubuntu_12_bold.h    ubuntu_12_bold    src/fonts/ubuntu_12_bold_ascii.h

Verify an already-emitted header without regenerating:
  python3 tools/subset_epd_font.py --verify <original.h> <font_name> <emitted.h>
"""

import re
import sys

# Kept ranges, each emitted as one EpdUnicodeInterval (so every range must be
# contiguously covered by the original font — the x4-os Ubuntu originals are).
# ASCII + Latin-1 Supplement + Latin Extended-A: book titles/authors on the
# grid ("Karamázov", "Fiódor") rendered '?' with the ASCII-only subset.
RANGES = [(0x20, 0x7E), (0xA0, 0xFF), (0x100, 0x17F)]


def kept_codepoints():
    for first, last in RANGES:
        yield from range(first, last + 1)


def strip_comments(text):
    """C translation-phase view of the text: splice backslash-newlines FIRST
    (phase 2), then remove /*...*/ and //... comments (phase 3), matching a
    real compiler — so a comment ending in `\\` eats the following line here
    exactly like it does in GCC. (No string literals occur in these arrays.)"""
    text = text.replace("\\\r\n", "").replace("\\\n", "")
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def extract_array_body(text, array_name):
    """Return the text between the braces of `... <array_name>[...] = { ... };`."""
    m = re.search(re.escape(array_name) + r"\s*\[[^\]]*\]\s*=\s*\{", text)
    if not m:
        raise SystemExit(f"array {array_name} not found")
    start = m.end()  # just past the opening brace
    depth = 1
    i = start
    while depth:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    return text[start : i - 1]


def parse_ints(body):
    """Parse a flat comma-separated list of C integer literals."""
    return [int(tok, 0) for tok in re.findall(r"-?0[xX][0-9a-fA-F]+|-?\d+", body)]


def parse_struct_rows(body):
    """Parse `{ a, b, ... }, { ... }, ...` into a list of int tuples."""
    rows = []
    for m in re.finditer(r"\{([^{}]*)\}", body):
        rows.append(tuple(parse_ints(m.group(1))))
    return rows


def load_font(header_path, name):
    text = strip_comments(open(header_path).read())
    bitmaps = bytes(parse_ints(extract_array_body(text, name + "Bitmaps")))
    glyphs = parse_struct_rows(extract_array_body(text, name + "Glyphs"))
    intervals = parse_struct_rows(extract_array_body(text, name + "Intervals"))
    for g in glyphs:
        assert len(g) == 7, f"EpdGlyph initializer with {len(g)} fields: {g}"
    for iv in intervals:
        assert len(iv) == 3, f"EpdUnicodeInterval initializer with {len(iv)} fields: {iv}"
    return bitmaps, glyphs, intervals


def glyph_for(cp, glyphs, intervals, what="font"):
    """Resolve cp through the interval table with Gfx::findGlyph semantics
    (offset + (cp - first)); returns None for uncovered codepoints."""
    for first, last, offset in intervals:
        if first <= cp <= last:
            idx = offset + (cp - first)
            assert idx < len(glyphs), (
                f"{what}: U+{cp:04X} resolves to glyph index {idx} but the "
                f"glyph array has only {len(glyphs)} entries (an emitted "
                f"array entry was lost, e.g. by comment line-splicing)"
            )
            return glyphs[idx]
    return None


def subset(bitmaps, glyphs, intervals):
    """Return (new_bitmap: bytes, new_glyphs: list of 7-tuples in cp order)."""
    out_bmp = bytearray()
    out_glyphs = []
    for cp in kept_codepoints():
        g = glyph_for(cp, glyphs, intervals, "original")
        if g is None:
            raise SystemExit(f"codepoint U+{cp:04X} not covered by the original font")
        w, h, adv, left, top, dlen, doff = g
        assert doff + dlen <= len(bitmaps), f"U+{cp:04X} slice out of range"
        out_glyphs.append((w, h, adv, left, top, dlen, len(out_bmp)))
        out_bmp += bitmaps[doff : doff + dlen]
    return bytes(out_bmp), out_glyphs


def verify(orig, emitted):
    """Resolve every codepoint 0x20..0x7E independently through the ORIGINAL
    interval table and the EMITTED header's own interval table (both loaded
    from their files via the C-faithful parser), comparing metrics and bitmap
    bytes. Coverage must agree on both sides."""
    obmp, oglyphs, ointervals = orig
    nbmp, nglyphs, nintervals = emitted
    for cp in kept_codepoints():
        og = glyph_for(cp, oglyphs, ointervals, "original")
        ng = glyph_for(cp, nglyphs, nintervals, "emitted")
        assert (og is None) == (ng is None), (
            f"coverage mismatch at U+{cp:04X}: original "
            f"{'covers' if og else 'lacks'} it, emitted "
            f"{'covers' if ng else 'lacks'} it"
        )
        if og is None:
            continue
        assert og[:6] == ng[:6], f"metrics mismatch at U+{cp:04X}: {og} vs {ng}"
        oslice = obmp[og[6] : og[6] + og[5]]
        nslice = nbmp[ng[6] : ng[6] + ng[5]]
        assert og[5] == len(oslice) == len(nslice) and oslice == nslice, (
            f"bitmap slice mismatch at U+{cp:04X}"
        )
    # Explicit spot checks: the 'Block' regression set plus the fallback char.
    for ch in "lockBA g0?\\][":
        assert glyph_for(ord(ch), nglyphs, nintervals, "emitted") is not None


def emit(out_path, name, new_bmp, new_glyphs):
    lines = []
    a = lines.append
    a("/**")
    ranges_desc = " + ".join(f"U+{f:04X}..U+{l:04X}" for f, l in RANGES)
    a(f" * Subset ({ranges_desc}) of {name} — generated, do not hand-edit.")
    a(" * M2.1c flash diet: only the Bitmaps/Glyphs/Intervals arrays are emitted")
    a(" * (kern/ligature/EpdFontData tables of the original were unreferenced).")
    a(" * Regenerate from the xphone-os directory with:")
    a(f" *   python3 tools/subset_epd_font.py ../x4-os/lib/EpdFont/builtinFonts/{name}.h {name} {out_path}")
    a(" */")
    a("#pragma once")
    a('#include "EpdFontData.h"')
    a("")
    a(f"static const uint8_t {name}Bitmaps[{len(new_bmp)}] = {{")
    for i in range(0, len(new_bmp), 16):
        a("    " + ", ".join(f"0x{b:02X}" for b in new_bmp[i : i + 16]) + ",")
    a("};")
    a("")
    a(f"static const EpdGlyph {name}Glyphs[] = {{")
    for cp, g in zip(kept_codepoints(), new_glyphs):
        # NEVER emit a raw glyph char in the comment: a lone '\' at end of a
        # //-comment line splices the next array entry into the comment (C
        # translation phase 2) and silently deletes it. Label by codepoint.
        a("    { %d, %d, %d, %d, %d, %d, %d }, // U+%04X" % (*g, cp))
    a("};")
    a("")
    a(f"static const EpdUnicodeInterval {name}Intervals[] = {{")
    offset = 0
    for first, last in RANGES:
        a("    { 0x%X, 0x%X, 0x%X }," % (first, last, offset))
        offset += last - first + 1
    a("};")
    a("")
    open(out_path, "w").write("\n".join(lines))


def main():
    args = sys.argv[1:]
    verify_only = bool(args) and args[0] == "--verify"
    if verify_only:
        args = args[1:]
    if len(args) != 3:
        raise SystemExit(
            "usage: subset_epd_font.py [--verify] <input_header> <font_name> <output_header>"
        )
    in_path, name, out_path = args
    orig = load_font(in_path, name)
    if not verify_only:
        new_bmp, new_glyphs = subset(*orig)
        emit(out_path, name, new_bmp, new_glyphs)
    # Mandatory: re-parse the emitted FILE (C-faithfully) and cross-verify.
    verify(orig, load_font(out_path, name))
    nbmp, nglyphs, _ = load_font(out_path, name)
    table = len(nglyphs) * 12 + len(nbmp) + 12  # sizeof(EpdGlyph)=12 w/ padding
    print(
        f"PASS  {name}: {len(nglyphs)} glyphs, bitmap {len(nbmp)} B, "
        f"~{table} B of tables -> {out_path}"
        + (" (verify only)" if verify_only else "")
    )


if __name__ == "__main__":
    main()
