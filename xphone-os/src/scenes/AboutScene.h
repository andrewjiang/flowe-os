#pragma once

// xphone-os M1 — About: version, heap, boot time, panel dims.

#include "../Scene.h"

class AboutScene : public Scene {
 public:
  void handleInput(Input& in) override;
  void render(Gfx& gfx) override;
};
