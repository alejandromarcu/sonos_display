// Monochrome panels driven by U8g2, laid out for 128x64.
//
// To add another panel U8g2 supports, add a branch with its constructor
// (https://github.com/olikraus/u8g2/wiki/u8g2setupcpp) and a display section in platformio.ini.

#if defined(DISPLAY_SSD1306_128X64)
#define U8G2_PANEL U8G2_SSD1306_128X64_NONAME_F_HW_I2C
#define U8G2_PANEL_ARGS U8G2_R0, U8X8_PIN_NONE, PIN_I2C_SCL, PIN_I2C_SDA
#define U8G2_BUS_HZ 400000
#endif

#ifdef U8G2_PANEL

#include <U8g2lib.h>

#include <algorithm>

#include "config.h"
#include "display.h"

static U8G2_PANEL u8g2(U8G2_PANEL_ARGS);

namespace {

struct Marquee {
  String text;  // Last text drawn; a different one starts again from the left.
  uint32_t holdUntil = 0;
  void reset() { holdUntil = millis() + 2000; }
};

// Draws `text` at baseline `y`, scrolling it horizontally when it is wider than the screen.
void drawScrolling(const String &text, int y, Marquee &mq) {
  if (text != mq.text) {
    mq.text = text;
    mq.reset();
  }
  int w = u8g2.getUTF8Width(text.c_str());
  if (w <= u8g2.getDisplayWidth()) {
    u8g2.drawUTF8(0, y, text.c_str());
    return;
  }
  const int gap = 24;
  uint32_t now = millis();
  int offset = now < mq.holdUntil ? 0 : (now - mq.holdUntil) / 30;  // ~33 px/s
  if (offset >= w + gap) {
    mq.reset();
    offset = 0;
  }
  u8g2.drawUTF8(-offset, y, text.c_str());
  u8g2.drawUTF8(-offset + w + gap, y, text.c_str());
}

String fmtTime(unsigned s) {
  char buf[16];
  if (s >= 3600) snprintf(buf, sizeof buf, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
  else snprintf(buf, sizeof buf, "%u:%02u", s / 60, s % 60);
  return buf;
}

void drawStateIcon(PlayState s) {
  switch (s) {
    case PlayState::Playing: u8g2.drawTriangle(0, 0, 0, 8, 5, 4); break;
    case PlayState::Paused: u8g2.drawBox(0, 1, 2, 7); u8g2.drawBox(4, 1, 2, 7); break;
    case PlayState::Stopped: u8g2.drawBox(0, 1, 7, 7); break;
    default: break;
  }
}

class U8g2Display : public Display {
 public:
  void begin() override {
    u8g2.setBusClock(U8G2_BUS_HZ);
    u8g2.begin();
  }

  void setPower(bool on) override { u8g2.setPowerSave(!on); }

  void render(const NowPlaying &np, const String &overlay) override {
    const int W = u8g2.getDisplayWidth();
    u8g2.clearBuffer();

    // Header (fits in the top 16 rows, which are yellow on two-color panels).
    u8g2.setFont(u8g2_font_5x8_tf);
    drawStateIcon(np.state);
    int nameRight = 9 + u8g2.getUTF8Width(ROOM_NAME);
    u8g2.drawUTF8(9, 8, ROOM_NAME);
    if (np.status.isEmpty() && np.durationS > 0) {
      uint32_t pos = np.positionS;
      if (np.state == PlayState::Playing) pos += (millis() - np.fetchedAtMs) / 1000;
      pos = std::min(pos, np.durationS);
      String t = fmtTime(pos) + "/" + fmtTime(np.durationS);
      if (nameRight + 4 > W - u8g2.getUTF8Width(t.c_str())) t = fmtTime(pos);
      u8g2.drawUTF8(W - u8g2.getUTF8Width(t.c_str()), 8, t.c_str());
      u8g2.drawFrame(0, 11, W, 4);
      u8g2.drawBox(0, 11, W * pos / np.durationS, 4);
    } else {
      u8g2.drawHLine(0, 12, W);
    }

    if (np.status.length()) {
      u8g2.setFont(u8g2_font_6x10_tf);
      int w = u8g2.getUTF8Width(np.status.c_str());
      u8g2.drawUTF8(std::max(0, (W - w) / 2), 42, np.status.c_str());
      title_ = artist_ = album_ = Marquee();  // Restart scrolling when the track info comes back.
    } else {
      u8g2.setFont(u8g2_font_helvB12_tf);
      drawScrolling(np.title, 31, title_);
      u8g2.setFont(u8g2_font_6x10_tf);
      drawScrolling(np.artist, 46, artist_);
      drawScrolling(np.album, 59, album_);
    }

    if (overlay.length()) {
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.setDrawColor(0);
      u8g2.drawBox(0, 50, W, 14);
      u8g2.setDrawColor(1);
      u8g2.drawFrame(0, 50, W, 14);
      u8g2.drawUTF8(3, 61, overlay.c_str());
    }
    u8g2.sendBuffer();
  }

 private:
  Marquee title_, artist_, album_;
};

}  // namespace

Display &getDisplay() {
  static U8g2Display display;
  return display;
}

#endif  // U8G2_PANEL
