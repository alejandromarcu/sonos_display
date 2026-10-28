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

    // Digits: typing two plays that slot (00-99) from the web page.
    {NEC, 0x00, 0x16, Digit(0), false},
    {NEC, 0x00, 0x0C, Digit(1), false},
    {NEC, 0x00, 0x18, Digit(2), false},
    {NEC, 0x00, 0x5E, Digit(3), false},
    {NEC, 0x00, 0x08, Digit(4), false},
    {NEC, 0x00, 0x1C, Digit(5), false},
    {NEC, 0x00, 0x5A, Digit(6), false},
    {NEC, 0x00, 0x42, Digit(7), false},
    {NEC, 0x00, 0x52, Digit(8), false},
    {NEC, 0x00, 0x4A, Digit(9), false},
};
