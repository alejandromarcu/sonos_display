#pragma once

// IR remote control. The configuration's remote (see platformio.ini) maps button codes to commands
// with a table in src/remotes/. Without a remote these do nothing.

#include <Arduino.h>

void irRemoteBegin();

// Checks for a button press and acts on it: known buttons send their command to the speaker, and two
// digit buttons in a row play that slot (see slots.h). Returns true when a button was pressed.
// `message` is set to brief feedback to show: the slot being played ("12: Abbey Road" or
// "12: empty"), or a code that isn't in the remote's table ("IR NEC 0:15") so it can be added.
bool irRemotePoll(String &message);

// The slot number being typed ("1_") while waiting for its second digit, or "" otherwise.
String irRemoteTyping();
