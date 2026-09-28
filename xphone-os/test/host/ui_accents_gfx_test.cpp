// Real Gfx/fonts, with only the display replaced. Output logical ink for exact comparison.
#include "Gfx.h"
#include "Fonts.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
  assert(argc == 4);
  EInkDisplay display;
  Gfx gfx(display);
  assert(gfx.begin());
  display.clearScreen();
  const XpFont& font = !strcmp(argv[1], "bold") ? kFontBold :
                       !strcmp(argv[1], "small") ? kFontSmall : kFontRegular;
  const char* text = argv[3];
  const std::string original(text);
  const int lines = gfx.countWrappedLines(font, text, 143, 8);
  if (!strcmp(argv[2], "plain")) gfx.drawText(font, 11, 13, text);
  else if (!strcmp(argv[2], "center")) gfx.drawTextCentered(font, 230, 13, text);
  else if (!strcmp(argv[2], "scale_one")) gfx.drawTextScaled(font, 11, 13, text, 1);
  else if (!strcmp(argv[2], "scaled")) gfx.drawTextScaled(font, 11, 13, text, 2);
  else if (!strcmp(argv[2], "scaled_center")) gfx.drawTextScaledCentered(font, 230, 13, text, 2);
  else if (!strcmp(argv[2], "tail")) gfx.drawText(font, 450 - gfx.textWidth(font, text), 13, text);
  else if (!strcmp(argv[2], "wrap")) assert(gfx.drawTextWrapped(font, 11, 13, text, 143, 8) == lines);
  else assert(false);
  assert(original == text);  // Rendering must not modify a filename or caller buffer.
  printf("%d %d %d %d\n", gfx.canRender(font, text), gfx.textWidth(font, text),
         gfx.textWidthScaled(font, text, 2), lines);
  for (int y = 0; y < gfx.height(); ++y) {
    for (int x = 0; x < gfx.width(); ++x) {
      const int index = (gfx.width() - 1 - x) * display.getDisplayWidthBytes() + y / 8;
      putchar((display.bytes[index] & (0x80 >> (y % 8))) ? 0 : 1);
    }
  }
}
