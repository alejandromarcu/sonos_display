# Sonos Display

A small "now playing" screen for a Sonos room, built with an ESP32-S3 and a 128x64 SSD1306 OLED.
An optional IR receiver lets a cheap IR remote control playback and volume. The board, screen and
remote can be swapped; see [Hardware configurations](#hardware-configurations).

- Shows the title, artist and album, scrolling long lines, plus a play/pause icon and a progress bar
- Follows grouping: finds the room's group coordinator and re-checks it every minute
- Handles radio streams, TV (home theater) and line-in sources
- IR remote: play/pause, next, previous, volume up/down (held to repeat), mute, and saving what's
  playing to a slot
- Remote slots: typing two digits (00-99) on the remote plays an album, playlist, track or station,
  set up from a web page on the board (http://sonos-display.local by default) with Spotify or Apple
  Music links or your Sonos Favorites, or saved from what's playing with a button
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
   the Sonos app, optionally the IP of one of your speakers to skip discovery on boot, and the
   board's network name (its web page address). This file is gitignored.
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
- `src/slots.cpp`, `src/web.cpp`, `src/share_link.cpp`: the remote slots, their web page, and
  recognizing Spotify and Apple Music links
- `src/displays/`: display drivers
- `src/ir_remote.cpp`: IR receiver; `src/remotes/` has each remote's button table

## IR remote

Each remote's button codes are a table in `src/remotes/`. Pressing a button that isn't in the
table shows its protocol, address and command on screen and logs them to the serial monitor
(115200 baud). This means you can point any remote that IRremote decodes (NEC, Samsung, Sony, RC5,
...) at the receiver, note the codes and add them to the table.

Digit buttons are listed in the table as `Digit(0)` to `Digit(9)`; see [Remote slots](#remote-slots).

## Remote slots

Typing two digits on the remote plays one of 100 slots, 00 to 99 (so slot 7 is "0", "7"). The first
digit shows as "1_" on screen; the second starts the slot and shows its name, or "12: empty". A first
digit is forgotten after 5 seconds, and pressing any other button cancels it and does that button's
action.

Set up the slots from a computer or phone on the same network at **http://sonos-display.local**,
or the name set with `SONOS_HOSTNAME` in `include/secrets.h` (give each board its own, such as
`sonos-kitchen`, to run several). The board's IP address also works; it's shown on screen when the
board connects to WiFi and logged to the serial monitor. A slot can hold:

- A share link from the Spotify or Apple Music app (Share > Copy link). The page fills in an empty
  label with the album, playlist or song name.
  - Spotify: albums, playlists, tracks, podcast shows and episodes (`https://open.spotify.com/...`).
    Artist pages and `spotify.link` short links aren't supported.
  - Apple Music: albums, playlists and songs (`https://music.apple.com/...`), and albums and
    playlists in your library (`https://music.apple.com/library/...`).
- One of your Sonos Favorites, picked with the star button next to the link: albums, playlists,
  tracks and stations from any service. Spotify and Apple Music ones become share links; others are
  kept as Sonos had them.

The service has to be added to your Sonos system. Playing an album or playlist replaces the room's
queue, as "Play Now" does in the Sonos app, on the whole group if the room is grouped. Sonos'
"Recently Played" isn't offered because speakers don't share it on the local network; it lives in
Sonos' cloud.

A button set to `Command::SaveToSlot` (0x44 on the default remote, which used to be mute) saves
what's playing to the first empty slot and shows "Saved 05: ...", or "Already in 05: ..." when a slot
has it. For music played from the queue that's the album or playlist it was started from; otherwise
it's the station itself. This covers whatever was started from the Sonos app or a slot, from any
service. TV, line-in and music started from another app such as Spotify (Spotify Connect or AirPlay)
show "Can't save this", because Sonos doesn't know what those are. The board finds out what's playing
by briefly subscribing to the speaker's events, which arrive on port 3500.

Slots are saved in the board's flash, so they survive firmware uploads but stay with the board. Use
**Export** on the page to save them as a JSON file and **Import** to load them onto another board.

With several boards, keep in mind that each one obeys any remote in sight that sends its codes. Boards
in different rooms are fine with the same remote model; for two within sight of each other, give one
a remote with different codes (its own table in `src/remotes/`).

Sonos accepts Spotify links with a region-specific account number (2311 in Europe, 3079 in the US).
Both are tried and the one that works is remembered until the next restart, so the serial log may
show one `AddURIToQueue -> error` the first time a Spotify slot plays.

## Configuration

Settings that apply to every configuration, such as the poll interval, volume step, screen-off
timeout, digit timeout and hostname, are in `src/config.h`.
