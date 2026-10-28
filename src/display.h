#pragma once

// A screen showing what's playing. Each configuration compiles exactly one driver from
// src/displays/, enabled by the DISPLAY_* flag in platformio.ini, which implements this class
// and getDisplay().

#include "sonos.h"

class Display {
 public:
  virtual ~Display() = default;
  virtual void begin() = 0;
  // Draws a frame; called continuously while the screen is on. `overlay` is a short message to show
  // on top of the track info ("Volume 23", an unknown IR code), or empty.
  virtual void render(const NowPlaying &np, const String &overlay) = 0;
  // Turns the panel (or its backlight) off when idle and back on. render() isn't called while off.
  virtual void setPower(bool on) = 0;
};

Display &getDisplay();
