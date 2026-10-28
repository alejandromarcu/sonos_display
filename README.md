# Sonos Display

A small "now playing" screen for a Sonos room, built with an ESP32-S3 and a 128x64 SSD1306 OLED.
An optional IR receiver lets a cheap IR remote control playback and volume. The board, screen and
remote can be swapped; see [Hardware configurations](#hardware-configurations).

- Shows the title, artist and album, scrolling long lines, plus a play/pause icon and a progress bar
- Follows grouping: finds the room's group coordinator and re-checks it every minute
- Handles radio streams, TV (home theater) and line-in sources
- IR remote: play/pause, next, previous, volume up/down (held to repeat), mute
- Turns the screen off after 10 minutes without playback to avoid OLED burn-in

It talks to the speakers directly over the local network (SSDP discovery and Sonos' UPnP/SOAP
services on port 1400), so it needs no cloud account or API key.

## Hardware

Each PlatformIO environment is one hardware configuration:

| Environment            | Board                          | Display                 | IR remote          |
|------------------------|--------------------------------|-------------------------|--------------------|
| `freenove_s3_ssd1306`  | Freenove ESP32-S3-WROOM-1 N8R8 | SSD1306 128x64 I2C OLED | Cheap NEC remote   |
| `freenove_s3_headless` | Freenove ESP32-S3-WROOM-1 N8R8 | None (serial log only)  | Cheap NEC remote   |

Wiring on the Freenove board:

| Signal      | ESP32-S3 GPIO |
|-------------|---------------|
| OLED SDA    | 8             |
| OLED SCL    | 9             |
| IR receiver | 14            |

Power the OLED and the IR receiver (a 38 kHz module such as the VS1838B) from 3.3 V.

## Setup

1. Install [PlatformIO](https://platformio.org/) (CLI or the VS Code extension).
2. Create your local settings file:

   ```bash
   cp include/secrets.example.h include/secrets.h
   ```

   Then edit `include/secrets.h` with your WiFi network, the Sonos room name exactly as it appears in
   the Sonos app, and optionally the IP of one of your speakers to skip discovery on boot. This file
   is gitignored.
3. Build, flash and watch the serial log:

   ```bash
   pio run -t upload -t monitor
   ```

   This uses the default environment (`default_envs` in `platformio.ini`). Add `-e <name>` to pick
   another one.

## Hardware configurations

`platformio.ini` has one section per board, display and IR remote, and each environment combines
one of each:

```ini
[env:freenove_s3_ssd1306]
extends = board_freenove_s3, display_ssd1306, remote_default
```

To try different hardware, add a section for the part that changes and an environment that uses it:

- **Board**: copy `[board_freenove_s3]` and set the PlatformIO `board`, its flash and PSRAM options,
  and the GPIOs you wired in `custom_board_flags` (`PIN_I2C_SDA`, `PIN_I2C_SCL`, `PIN_IR_RECEIVER`).
  No code changes are needed.
- **Display**: for another monochrome panel that U8g2 supports (such as the 1.3" SH1106), add its
  constructor at the top of `src/displays/u8g2_display.cpp` under a new flag. For anything else
  (a color TFT, e-paper, ...), add a driver to `src/displays/` that implements the `Display` class
  from `src/display.h`, enabled by its own `#ifdef` flag. Then add a display section that sets the
  flag and lists the libraries it needs.
- **IR remote**: copy `src/remotes/default.h`, fill in the new remote's codes (see below), and add
  a remote section that points `IR_REMOTE_CODES` at the new file.

`display_none` and `remote_none` build without a screen or IR receiver. Every environment must
extend one display and one remote section; if one is missing, the build stops with an error saying so.

Code layout:

- `src/sonos.cpp`: discovery, polling and commands; the same for all hardware
- `src/main.cpp`: connects the Sonos client to the display and remote, and handles the screen-off timer
- `src/displays/`: display drivers
- `src/ir_remote.cpp`: IR receiver; `src/remotes/` has each remote's button table

## IR remote

Each remote's button codes are a table in `src/remotes/`. Pressing a button that isn't in the
table shows its protocol, address and command on screen and logs them to the serial monitor
(115200 baud). This means you can point any remote that IRremote decodes (NEC, Samsung, Sony, RC5,
...) at the receiver, note the codes and add them to the table.

## Configuration

Settings that apply to every configuration, such as the poll interval, volume step and screen-off
timeout, are in `src/config.h`.
