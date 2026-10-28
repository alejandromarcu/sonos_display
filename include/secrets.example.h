#pragma once

// Local settings. Copy this file to secrets.h (which is gitignored) and fill in your values.

#define WIFI_SSID "your-network-name"
#define WIFI_PASSWORD "your-password"

// Sonos room to show, as named in the Sonos app (case-insensitive).
#define SONOS_ROOM_NAME "Living Room"

// Optional IP of any Sonos speaker, tried before SSDP discovery. Leave "" to always discover.
#define SONOS_SEED_IP ""
