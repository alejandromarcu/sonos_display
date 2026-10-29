// Sonos "now playing" display for ESP32 boards.
//
// The hardware is chosen per environment in platformio.ini: the board and its pins, a display
// driver from src/displays/ and an IR remote's button table from src/remotes/. This file wires
// them to the Sonos client (sonos.cpp), which runs in a background task, and serves the web page
// for setting up the slots that the remote's digit buttons play (web.cpp, slots.cpp).

#include <Arduino.h>

#include "config.h"
#include "display.h"
#include "ir_remote.h"
#include "slots.h"
#include "sonos.h"
#include "web.h"

#if defined(NO_DISPLAY_SELECTED) || defined(NO_REMOTE_SELECTED)
#error "Each env in platformio.ini must extend a display_* and a remote_* section (display_none / remote_none for none)."
#endif

static const uint32_t FRAME_MS = 25;  // Frame-rate cap (~40 fps); slow panels may take longer anyway.

static Display &display = getDisplay();

static String overlayText;  // Shown briefly over the track info.
static uint32_t overlayUntil = 0;

static void showOverlay(const String &text, uint32_t ms) {
  overlayText = text;
  overlayUntil = millis() + ms;
}

void setup() {
  Serial.begin(115200);
  display.begin();
  slotsBegin();
  irRemoteBegin();
  sonosStart();
  webBegin();  // After sonosStart(), which brings up the network stack.
}

void loop() {
  static NowPlaying np;
  static uint32_t lastActiveMs = 0;
  static bool screenOn = true;
  uint32_t frameStartMs = millis();

  webPoll();
  sonosTakeState(np);
  String msg;
  bool toast = sonosTakeToast(msg);
  if (toast) showOverlay(msg, 2000);
  String feedback;
  bool pressed = irRemotePoll(feedback);
  String link, metadata, title;
  if (sonosTakeSource(link, metadata, title)) slotsAdd(link, metadata, title, feedback);  // Command::SaveToSlot
  if (feedback.length()) showOverlay(feedback, 3000);

  // Turn the screen off after a while without playback to avoid OLED burn-in.
  bool active = np.state == PlayState::Playing || np.state == PlayState::Transitioning || lastActiveMs == 0 ||
                pressed || toast;
  if (active) lastActiveMs = millis();
  bool wantOn = millis() - lastActiveMs < SCREEN_OFF_AFTER_MS;
  if (wantOn != screenOn) {
    screenOn = wantOn;
    display.setPower(wantOn);
  }

  // A half-typed slot number ("1_") stays up until its second digit or the timeout.
  String typing = irRemoteTyping();
  if (screenOn) display.render(np, typing.length() ? typing : millis() < overlayUntil ? overlayText : String());

  uint32_t periodMs = screenOn ? FRAME_MS : 200;
  uint32_t elapsedMs = millis() - frameStartMs;
  if (elapsedMs < periodMs) delay(periodMs - elapsedMs);
}
