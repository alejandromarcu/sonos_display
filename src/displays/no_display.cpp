// No screen. What's playing and command feedback are still logged to the serial monitor.

#ifdef DISPLAY_NONE

#include "display.h"

namespace {

class NoDisplay : public Display {
 public:
  void begin() override {}
  void render(const NowPlaying &, const String &) override {}
  void setPower(bool) override {}
};

}  // namespace

Display &getDisplay() {
  static NoDisplay display;
  return display;
}

#endif
