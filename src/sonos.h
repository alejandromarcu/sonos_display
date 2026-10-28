#pragma once

// Sonos client, independent of the display and remote. A background task finds the room's group
// coordinator and polls what's playing; the UI reads the latest state and queues commands here.

#include <Arduino.h>

#include <vector>

// SaveToSlot looks up the album, playlist, station, ... that's playing, for sonosTakeSource().
enum class Command : uint8_t { PlayPause, Next, Previous, VolumeUp, VolumeDown, Mute, SaveToSlot };

enum class PlayState { Unknown, Playing, Paused, Stopped, Transitioning };

struct NowPlaying {
  String status;  // Shown instead of track info when non-empty (connecting, errors, ...).
  String title, artist, album;
  PlayState state = PlayState::Unknown;
  uint32_t durationS = 0, positionS = 0;
  uint32_t fetchedAtMs = 0;
};

// Connects to WiFi and starts the background task.
void sonosStart();

// Queues a command for the speaker. Its result arrives as a toast.
void sonosSend(Command cmd);

// What a slot plays: a Spotify or Apple Music link (see share_link.h), or a Sonos URI with the
// metadata Sonos had for it (a station, a favorite, ...). Albums and playlists replace the queue.
// `name` is shown as a toast once it starts ("12: Abbey Road").
void sonosPlay(const String &link, const String &metadata, const String &name);

// "Spotify station", "YouTube Music playlist", ... for an item with Sonos metadata.
String sonosDescribeItem(const String &metadata);

// Something Sonos can play, as sonosPlay() takes it; `metadata` is empty for share links.
struct SonosItem {
  String title, link, metadata;
};

// The household's Sonos Favorites that can be played. Asks a speaker, so it takes a moment.
bool sonosFavorites(std::vector<SonosItem> &items);

// Copies the latest state into `np`; returns true if it changed since the last call.
bool sonosTakeState(NowPlaying &np);

// Returns true and sets `msg` when there is new feedback to show briefly ("Volume 23").
bool sonosTakeToast(String &msg);

// Returns true once after Command::SaveToSlot found what's playing, with `link` and `metadata` set
// as sonosPlay() takes them and `title` to its name, which may be empty. When it can't be saved (TV,
// line-in, music started from the Spotify app, ...) a toast says so instead.
bool sonosTakeSource(String &link, String &metadata, String &title);
