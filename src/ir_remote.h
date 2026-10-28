#pragma once

// IR remote control. The configuration's remote (see platformio.ini) maps button codes to commands
// with a table in src/remotes/. Without a remote these do nothing.

#include <Arduino.h>

void irRemoteBegin();

// Checks for a button press and sends known buttons' commands to the speaker. Returns true when a
// button was pressed. A code that isn't in the remote's table is described in `unknown`
// ("IR NEC 0:15") so it can be shown and added to the table.
bool irRemotePoll(String &unknown);
