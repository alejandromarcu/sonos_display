#pragma once

// The 100 slots (00 to 99) that typing two digits on the remote plays. Each holds a Spotify or
// Apple Music link (see share_link.h) and an optional label. They're edited on the web page and
// kept in flash, which survives firmware uploads; export them to move them to another board.

#include <Arduino.h>

static const int SLOT_COUNT = 100;

struct Slot {
  String label;  // Optional name shown on screen.
  String link;   // Empty for an unused slot.
};

// Mounts the filesystem and loads the saved slots.
void slotsBegin();

// Changes slot `n` and saves. An empty link clears it. Returns false and sets `error` when the link
// isn't one we can play or the slots can't be saved.
bool slotsSet(int n, Slot slot, String &error);

// Starts playing slot `n` and sets `name` to what to show ("12: Abbey Road"). Returns false, with
// `name` saying so, when the slot is empty.
bool slotsPlay(int n, String &name);

// The used slots as JSON: {"slots": [{"n": 12, "label": "...", "link": "...", "kind": "Spotify album"}]}.
String slotsExport();

// Replaces every slot with those in JSON from slotsExport() ("kind" is ignored) and saves.
bool slotsImport(const String &json, String &error);
