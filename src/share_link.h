#pragma once

// Recognizes Spotify and Apple Music share links (the apps' "Share > Copy link"), which the Sonos
// client knows how to queue.

#include <Arduino.h>

enum class MusicService : uint8_t { Spotify, AppleMusic };

struct ShareLink {
  MusicService service;
  String type;  // As the service names it: "album", "playlist", "track", "song", ...
  String id;    // The service's own URI for the item ("spotify:album:3T4t...", "album:975952384").
};

// Parses links such as https://open.spotify.com/album/3T4tUhGYeRNVUGevb0wThu (or
// spotify:album:3T4t...) and https://music.apple.com/us/album/amused-to-death/975952384. Returns
// false for anything else, including short links (spotify.link/...) that need a redirect.
bool parseShareLink(const String &link, ShareLink &out);

// "Spotify album", "Apple Music song", ...
String describeShareLink(const ShareLink &share);
