#pragma once

// Sonos client, independent of the display and remote. A background task finds the room's group
// coordinator and polls what's playing; the UI reads the latest state and queues commands here.

#include <Arduino.h>

enum class Command : uint8_t { PlayPause, Next, Previous, VolumeUp, VolumeDown, Mute };

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

// Copies the latest state into `np`; returns true if it changed since the last call.
bool sonosTakeState(NowPlaying &np);

// Returns true and sets `msg` when there is new feedback to show briefly ("Volume 23").
bool sonosTakeToast(String &msg);
