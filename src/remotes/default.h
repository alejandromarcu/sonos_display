// Button codes for the cheap NEC remote this project started with. Included by ir_remote.cpp.
//
// Pressing a button that isn't listed shows its protocol, address and command on screen and in the
// serial log, so a new remote's table can be filled in by pressing each button.

static const IrButton IR_BUTTONS[] = {
    // protocol, address, command, action, repeats while held
    {NEC, 0x00, 0x15, Command::PlayPause, false},
    {NEC, 0x00, 0x09, Command::Next, false},
    {NEC, 0x00, 0x07, Command::Previous, false},
    {NEC, 0x00, 0x40, Command::VolumeUp, true},
    {NEC, 0x00, 0x19, Command::VolumeDown, true},
    {NEC, 0x00, 0x44, Command::Mute, false},
};
