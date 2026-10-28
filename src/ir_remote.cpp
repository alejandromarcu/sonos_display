#include "ir_remote.h"

#ifdef IR_REMOTE_CODES

#include <IRremote.hpp>

#include "sonos.h"

struct IrButton {
  decode_type_t protocol;
  uint16_t address, command;  // As shown by the learning mode.
  Command action;
  bool repeats;  // Keeps firing while the button is held.
};

#include IR_REMOTE_CODES  // Defines IR_BUTTONS for the selected remote.

void irRemoteBegin() { IrReceiver.begin(PIN_IR_RECEIVER, DISABLE_LED_FEEDBACK); }

bool irRemotePoll(String &unknown) {
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
    // Held buttons only repeat volume, and at a limited rate; others need a fresh press.
    uint32_t now = millis();
    if (repeat && !b.repeats) return true;
    if (&b == lastButton && now - lastSentMs < (b.repeats ? 150u : 400u)) return true;
    lastButton = &b;
    lastSentMs = now;
    sonosSend(b.action);
    return true;
  }

  if (repeat) return false;
  char buf[40];
  snprintf(buf, sizeof buf, "IR %s %X:%X", getProtocolString(d.protocol), d.address, d.command);
  unknown = buf;
  return true;
}

#else

void irRemoteBegin() {}
bool irRemotePoll(String &) { return false; }

#endif
