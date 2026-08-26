#!/usr/bin/env python3
"""Generate a UI font subset header straight from a TTF (no x4-os originals).

Replaces subset_epd_font.py's dependency on the fontconvert.py-generated
x4-os headers, which are no longer on disk. The rasterization recipe below
was reverse-engineered against the shipped ubuntu_*_ascii.h subsets and
reproduces every one of their 957 glyphs BYTE-IDENTICALLY (metrics, advance,
and bitmap bits), so regenerating with extra ranges cannot move a single
existing pixel. The recipe:

  face.set_char_size(pt << 6, 0, 150, 0)     # points at 150 dpi
  FT_LOAD_RENDER                             # hinted 8-bit grayscale
  1bpp threshold: gray >= 32
  advanceX (12.4) = (linearHoriAdvance + 2048) >> 12
  left/top from bitmap_left/bitmap_top; bits MSB-first, row-major,
  no per-row padding (pixelPosition = y * width + x)

Self-check (mandatory): when --verify-against is given, every glyph present
in that header must come out byte-identical or the tool dies. Regeneration:

  python3 tools/gen_epd_font.py <font.ttf> <pt> <font_name> <out.h> \
      [--verify-against <old.h>]

Requested ranges live in RANGES below; codepoints the TTF lacks are skipped
and the emitted intervals are the maximal contiguous runs actually covered.
"""

import re
import sys

import freetype

# ASCII + Latin-1 + Latin Extended-A (the original M2.1c flash-diet set),
# plus Greek and Cyrillic (flowe-os#3: UI text renders '?' outside the set).
RANGES = [
    (0x20, 0x7E),
    (0xA0, 0xFF),
    (0x100, 0x17F),
    (0x370, 0x3FF),
    (0x400, 0x4FF),
]

GRAY_THRESHOLD = 32
DPI = 150


def render_glyph(face, cp):
    face.load_char(chr(cp), freetype.FT_LOAD_RENDER)
    slot = face.glyph
    bmp = slot.bitmap
    bits = [
        1 if bmp.buffer[row * bmp.pitch + col] >= GRAY_THRESHOLD else 0
        for row in range(bmp.rows)
        for col in range(bmp.width)
    ]
    data = [0] * ((len(bits) + 7) // 8)
    for i, bit in enumerate(bits):
        if bit:
            data[i >> 3] |= 0x80 >> (i & 7)
    return {
        "width": bmp.width,
        "height": bmp.rows,
        "advance": (slot.linearHoriAdvance + 2048) >> 12,
        "left": slot.bitmap_left,
        "top": slot.bitmap_top,
        "data": data,
    }


def parse_existing(path):
    src = open(path).read()
    seg = re.search(r"Bitmaps\[\d+\] = \{(.*?)\};", src, re.S).group(1)
    bitmap = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}\b", seg)]
    glyphs = {}
    for m in re.finditer(
        r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+)\s*\},\s*//\s*U\+([0-9A-Fa-f]+)",
        src,
    ):
        w, h, adv, left, top, dlen, doff, cp = m.groups()
        glyphs[int(cp, 16)] = {
            "width": int(w),
            "height": int(h),
            "advance": int(adv),
            "left": int(left),
            "top": int(top),
            "data": bitmap[int(doff) : int(doff) + int(dlen)],
        }
    return glyphs


def main():
    args = sys.argv[1:]
    verify_path = None
    if "--verify-against" in args:
        i = args.index("--verify-against")
        verify_path = args[i + 1]
        del args[i : i + 2]
    ttf, pt, name, out_path = args[0], int(args[1]), args[2], args[3]

    face = freetype.Face(ttf)
    face.set_char_size(pt << 6, 0, DPI, 0)

    covered = []  # (cp, glyph) in codepoint order
    for lo, hi in RANGES:
        for cp in range(lo, hi + 1):
            if face.get_char_index(cp) == 0 and cp != 0x20:
                continue
            covered.append((cp, render_glyph(face, cp)))

    if verify_path:
        old = parse_existing(verify_path)
        new = dict(covered)
        missing = [cp for cp in old if cp not in new]
        assert not missing, "codepoints lost vs %s: %s" % (
            verify_path,
            ["U+%04X" % c for c in missing[:8]],
        )
        for cp, g in old.items():
            assert new[cp] == g, "U+%04X differs from %s" % (cp, verify_path)
        print("verify: %d existing glyphs byte-identical" % len(old))

    # Maximal contiguous runs -> intervals; glyph array in run order.
    intervals = []
    for cp, _ in covered:
        if intervals and cp == intervals[-1][1] + 1:
            intervals[-1][1] = cp
        else:
            intervals.append([cp, cp])

    bitmap = []
    glyph_rows = []
    for cp, g in covered:
        off = len(bitmap)
        bitmap.extend(g["data"])
        glyph_rows.append(
            "    { %d, %d, %d, %d, %d, %d, %d }, // U+%04X"
            % (g["width"], g["height"], g["advance"], g["left"], g["top"], len(g["data"]), off, cp)
        )

    range_desc = " + ".join("U+%04X..U+%04X" % (a, b) for a, b in RANGES)
    lines = []
    lines.append("/**")
    lines.append(" * Subset (%s) of %s — generated, do not hand-edit." % (range_desc, name))
    lines.append(" * Rendered directly from the Ubuntu TTF by tools/gen_epd_font.py; the")
    lines.append(" * recipe reproduces the retired x4-os fontconvert output byte-exactly.")
    lines.append(" * Regenerate from the xphone-os directory with:")
    lines.append(" *   python3 tools/gen_epd_font.py <Ubuntu ttf> %d %s src/fonts/%s" % (pt, name, out_path.split("/")[-1]))
    lines.append(" */")
    lines.append("#pragma once")
    lines.append('#include "EpdFontData.h"')
    lines.append("")
    lines.append("static const uint8_t %sBitmaps[%d] = {" % (name, len(bitmap)))
    for i in range(0, len(bitmap), 16):
        lines.append("    " + ", ".join("0x%02X" % b for b in bitmap[i : i + 16]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("static const EpdGlyph %sGlyphs[] = {" % name)
    lines.extend(glyph_rows)
    lines.append("};")
    lines.append("")
    lines.append("static const EpdUnicodeInterval %sIntervals[] = {" % name)
    offset = 0
    for a, b in intervals:
        lines.append("    { 0x%X, 0x%X, 0x%X }," % (a, b, offset))
        offset += b - a + 1
    lines.append("};")
    lines.append("")

    open(out_path, "w").write("\n".join(lines))
    print(
        "%s: %d glyphs, %d intervals, %d bitmap bytes"
        % (out_path, len(covered), len(intervals), len(bitmap))
    )


if __name__ == "__main__":
    main()
