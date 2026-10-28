#pragma once

// Settings shared by every hardware configuration. The hardware itself (board, pins, display,
// IR remote) is chosen per environment in platformio.ini.

#include <stdint.h>

#if !__has_include("secrets.h")
#error "Missing include/secrets.h: copy include/secrets.example.h to include/secrets.h and edit it."
#endif
#include "secrets.h"

static const char *const ROOM_NAME = SONOS_ROOM_NAME;  // Case-insensitive; curly quotes match straight ones.
static const char *const SEED_IP = SONOS_SEED_IP;      // Optional known speaker IP, tried before SSDP.
static const uint32_t POLL_INTERVAL_MS = 3000;
static const uint32_t TOPOLOGY_REFRESH_MS = 60000;           // Re-check grouping this often.
static const uint32_t SCREEN_OFF_AFTER_MS = 10 * 60 * 1000;  // Turn the screen off when idle (burn-in).
static const int VOLUME_STEP = 2;
