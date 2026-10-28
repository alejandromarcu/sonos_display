#include "ir_remote.h"

#ifdef IR_REMOTE_CODES

#include <IRremote.hpp>

#include "config.h"
#include "slots.h"
#include "sonos.h"

// What a button does: send a command to the speaker, or type a digit of a slot number.
struct Action {
  constexpr Action(Command c) : command(c), digit(-1) {}
  constexpr explicit Action(int8_t d) : command(), digit(d) {}
  Command command;
  int8_t digit;  // 0-9 for digit buttons, -1 for commands.
};
constexpr Action Digit(int8_t d) { return Action(d); }

struct IrButton {
  decode_type_t protocol;
  uint16_t address, command;  // As shown by the learning mode.
  Action action;
  bool repeats;  // Keeps firing while the button is held.
};

#include IR_REMOTE_CODES  // Defines IR_BUTTONS for the selected remote.

static int firstDigit = -1;  // A slot's first digit, while waiting for the second.
static uint32_t firstDigitMs = 0;

static bool typing() { return firstDigit >= 0 && millis() - firstDigitMs < DIGIT_TIMEOUT_MS; }

// Two digits in a row play that slot; a lone digit is forgotten after DIGIT_TIMEOUT_MS.
static void typeDigit(int digit, String &message) {
  if (!typing()) {
    firstDigit = digit;
    firstDigitMs = millis();
    Serial.printf("Slot %d_\n", digit);
    return;
  }
  slotsPlay(firstDigit * 10 + digit, message);
  firstDigit = -1;
}

void irRemoteBegin() { IrReceiver.begin(PIN_IR_RECEIVER, DISABLE_LED_FEEDBACK); }

bool irRemotePoll(String &message) {
  if (!IrReceiver.decode()) return false;
  IRData d = IrReceiver.decodedIRData;
  IrReceiver.resume();
  bool repeat = d.flags & IRDATA_FLAGS_IS_REPEAT;
  if (!repeat)
    Serial.printf("IR: protocol=%s address=0x%04X command=0x%04X raw=0x%llX\n", getProtocolString(d.protocol),
                  d.address, d.command, (unsigned long long)d.decodedRawData);

  static const IrButton *lastButton = nullptr;
  static uint32_t lastSentMs = 0;
  for (const IrButton &b : IR_BUTTONS) {
    if (d.protocol != b.protocol || d.address != b.address || d.command != b.command) continue;
    // Held buttons only repeat volume, and at a limited rate; others need a fresh press. Some remotes
    // send a press twice, so a quick second one is dropped, but less eagerly for digits ("11").
    uint32_t now = millis();
    if (repeat && !b.repeats) return true;
    bool fast = b.repeats || b.action.digit >= 0;
    if (&b == lastButton && now - lastSentMs < (fast ? 150u : 400u)) return true;
    lastButton = &b;
    lastSentMs = now;
    if (b.action.digit >= 0) {
      typeDigit(b.action.digit, message);
    } else {
      firstDigit = -1;  // Any other button cancels a half-typed slot number.
      sonosSend(b.action.command);
    }
    return true;
  }

  if (repeat) return false;
  if (d.protocol != UNKNOWN) firstDigit = -1;  // A button not in the table, rather than noise.
  char buf[40];
  snprintf(buf, sizeof buf, "IR %s %X:%X", getProtocolString(d.protocol), d.address, d.command);
  message = buf;
  return true;
}

String irRemoteTyping() { return typing() ? String(firstDigit) + "_" : String(); }

#else

void irRemoteBegin() {}
bool irRemotePoll(String &) { return false; }
String irRemoteTyping() { return String(); }

#endif
