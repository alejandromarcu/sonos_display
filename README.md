# Sonos Display

A small "now playing" screen for a Sonos room, built with an ESP32-S3 and a 128x64 SSD1306 OLED.
An optional IR receiver lets any cheap NEC remote control playback and volume.

- Shows the title, artist and album, scrolling long lines, plus a play/pause icon and a progress bar
- Follows grouping: finds the room's group coordinator and re-checks it every minute
- Handles radio streams, TV (home theater) and line-in sources
- IR remote: play/pause, next, previous, volume up/down (held to repeat), mute
- Turns the screen off after 10 minutes without playback to avoid OLED burn-in

It talks to the speakers directly over the local network (SSDP discovery and Sonos' UPnP/SOAP
services on port 1400), so it needs no cloud account or API key.

## Hardware

- Freenove ESP32-S3-WROOM-1 N8R8 (any ESP32-S3 should work after adjusting `platformio.ini`)
- SSD1306 128x64 I2C OLED
- Optional: 38 kHz IR receiver module (VS1838B or similar) and an NEC remote

| Signal      | ESP32-S3 GPIO |
|-------------|---------------|
| OLED SDA    | 8             |
| OLED SCL    | 9             |
| IR receiver | 14            |

Power the OLED and IR receiver from 3.3 V. The pins are set at the top of `src/main.cpp`.

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

## IR remote

Button codes are in the `IR_BUTTONS` table in `src/main.cpp`. Pressing a button that isn't in the
table shows its code on screen and logs it to the serial monitor (115200 baud), so you can point any
NEC remote at the receiver, note the codes and add them to the table.

## Configuration

Other settings, such as the poll interval, volume step and screen-off timeout, are constants at
the top of `src/main.cpp`.
