#pragma once

// The 100 slots (00 to 99) that typing two digits on the remote plays. Each holds a Spotify or
// Apple Music link (see share_link.h), or something saved from Sonos (a station, a favorite, ...)
// as a Sonos URI and its metadata, and an optional label. They're edited on the web page and kept
// in flash, which survives firmware uploads; export them to move them to another board.

#include <Arduino.h>

static const int SLOT_COUNT = 100;

struct Slot {
  String label;     // Optional name shown on screen.
  String link;      // Share link or Sonos URI; empty for an unused slot.
  String metadata;  // Sonos' DIDL-Lite metadata for a Sonos URI; empty for share links.
};

// Mounts the filesystem and loads the saved slots.
void slotsBegin();

// Changes slot `n` and saves. An empty link clears it. Returns false and sets `error` when the link
// isn't one we can play or the slots can't be saved.
bool slotsSet(int n, Slot slot, String &error);

// Moves slot `from` to `to`, swapping the two when `to` is used, and saves.
bool slotsMove(int from, int to, String &error);

// Starts playing slot `n` and sets `name` to what to show ("12: Abbey Road"). Returns false, with
// `name` saying so, when the slot is empty.
bool slotsPlay(int n, String &name);

// Saves a link (and metadata, as in Slot) in the first empty slot, unless a slot has it already,
// and sets `message` to what to show ("Saved 05: Abbey Road", "Already in 05: ...", "No empty slot").
bool slotsAdd(const String &link, const String &metadata, const String &label, String &message);

// The used slots as JSON: {"slots": [{"n": 12, "label": "...", "link": "...", "kind": "Spotify album"}]},
// plus "metadata" for Sonos URIs.
String slotsExport();

// Replaces every slot with those in JSON from slotsExport() ("kind" is ignored) and saves.
bool slotsImport(const String &json, String &error);
