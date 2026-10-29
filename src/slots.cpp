// Slots are saved to LittleFS as /slots.json, in the same format as slotsExport().

#include "slots.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include <algorithm>
#include <vector>

#include "share_link.h"
#include "sonos.h"

static const char *const SLOTS_FILE = "/slots.json";
static const unsigned MAX_LABEL = 200, MAX_LINK = 400;  // Bytes; the page limits labels to 80 characters.
static const unsigned MAX_METADATA = 4096;

static Slot slots[SLOT_COUNT];  // Only used from the Arduino loop task (remote and web page).
static bool mounted = false;

// Tidies a slot from the web page or a file and checks that it's something we can play.
static bool clean(Slot &slot, String &error) {
  slot.label.trim();
  slot.link.trim();
  for (char &c : slot.label)
    if ((uint8_t)c < 0x20) c = ' ';
  // The screen's fonts are Latin-1 only, and names from Spotify often have curly quotes or dashes.
  slot.label.replace("\xE2\x80\x98", "'");
  slot.label.replace("\xE2\x80\x99", "'");
  slot.label.replace("\xE2\x80\x9C", "\"");
  slot.label.replace("\xE2\x80\x9D", "\"");
  slot.label.replace("\xE2\x80\x93", "-");
  slot.label.replace("\xE2\x80\x94", "-");
  if (slot.link.isEmpty()) slot.label = "";
  if (slot.label.length() > MAX_LABEL) {
    error = "The label is too long";
    return false;
  }
  ShareLink share;
  bool shareLink = parseShareLink(slot.link, share);
  if (shareLink || slot.link.isEmpty()) slot.metadata = "";
  bool sonosItem = slot.metadata.startsWith("<DIDL-Lite") && slot.metadata.length() <= MAX_METADATA;
  if (slot.link.length() && (slot.link.length() > MAX_LINK || !(shareLink || sonosItem))) {
    error = "Not a Spotify or Apple Music album, playlist or track link";
    return false;
  }
  return true;
}

// "Spotify album", "Spotify station", ...
static String kind(const Slot &slot) {
  ShareLink share;
  if (slot.metadata.length()) return sonosDescribeItem(slot.metadata);
  return parseShareLink(slot.link, share) ? describeShareLink(share) : String();
}

// What a link plays, ignoring details such as a share link's "?si=" or a Sonos URI's account flags.
static String identity(const String &link) {
  ShareLink share;
  if (parseShareLink(link, share)) return share.id;
  int query = link.indexOf('?');
  return query < 0 ? link : link.substring(0, query);
}

// Reads JSON in slotsExport()'s format into `out`, which gets SLOT_COUNT entries.
static bool parse(const String &json, std::vector<Slot> &out, String &error) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    error = String("Not valid JSON (") + err.c_str() + ")";
    return false;
  }
  if (!doc["slots"].is<JsonArray>()) {
    error = "Expected {\"slots\": [...]}";
    return false;
  }
  out.assign(SLOT_COUNT, Slot());
  for (JsonObject o : doc["slots"].as<JsonArray>()) {
    int n = o["n"] | -1;
    if (n < 0 || n >= SLOT_COUNT) {
      error = "Slot numbers go from 0 to 99";
      return false;
    }
    Slot slot{o["label"] | "", o["link"] | "", o["metadata"] | ""};
    if (!clean(slot, error)) {
      error = "Slot " + String(n) + ": " + error;
      return false;
    }
    out[n] = slot;
  }
  return true;
}

// Writes a new file and then replaces the old one, so a reset halfway through loses nothing.
static bool save(String &error) {
  String json = slotsExport();
  String tmp = String(SLOTS_FILE) + ".tmp";
  File f = mounted ? LittleFS.open(tmp, "w") : File();
  bool ok = f && f.print(json) == json.length();
  f.close();
  ok = ok && LittleFS.rename(tmp, SLOTS_FILE);
  if (!ok) error = "Couldn't save the slots to flash";
  return ok;
}

void slotsBegin() {
  mounted = LittleFS.begin(true);  // Formats the partition the first time.
  if (!mounted) {
    Serial.println("Slots: can't mount flash storage, so changes won't be saved");
    return;
  }
  if (!LittleFS.exists(SLOTS_FILE)) return;
  File f = LittleFS.open(SLOTS_FILE, "r");
  String json = f.readString();
  f.close();

  std::vector<Slot> loaded;
  String error;
  if (!parse(json, loaded, error)) {
    Serial.printf("Slots: ignoring %s: %s\n", SLOTS_FILE, error.c_str());
    return;
  }
  std::copy(loaded.begin(), loaded.end(), slots);
  int used = std::count_if(slots, slots + SLOT_COUNT, [](const Slot &s) { return s.link.length() > 0; });
  Serial.printf("Slots: loaded %d\n", used);
}

bool slotsSet(int n, Slot slot, String &error) {
  if (n < 0 || n >= SLOT_COUNT) {
    error = "Slot numbers go from 0 to 99";
    return false;
  }
  if (!clean(slot, error)) return false;
  Slot old = slots[n];
  slots[n] = slot;
  if (save(error)) return true;
  slots[n] = old;
  return false;
}

bool slotsMove(int from, int to, String &error) {
  if (from < 0 || from >= SLOT_COUNT || to < 0 || to >= SLOT_COUNT) {
    error = "Slot numbers go from 0 to 99";
    return false;
  }
  std::swap(slots[from], slots[to]);
  if (save(error)) return true;
  std::swap(slots[from], slots[to]);
  return false;
}

// "12: Abbey Road", or "12: Spotify album" without a label, or "12: empty".
static String slotName(int n) {
  char num[12];
  snprintf(num, sizeof num, "%02d: ", n);
  const Slot &slot = slots[n];
  if (slot.label.length()) return num + slot.label;
  return num + (slot.link.length() ? kind(slot) : String("empty"));
}

bool slotsPlay(int n, String &name) {
  bool used = n >= 0 && n < SLOT_COUNT && slots[n].link.length();
  if (used) {
    name = slotName(n);
    sonosPlay(slots[n].link, slots[n].metadata, name);
  } else {
    char num[12];
    snprintf(num, sizeof num, "%02d: empty", n);
    name = num;
  }
  Serial.printf("Slot %s\n", name.c_str());
  return used;
}

bool slotsAdd(const String &link, const String &metadata, const String &label, String &message) {
  int empty = -1;
  for (int n = 0; n < SLOT_COUNT; n++) {
    if (slots[n].link.isEmpty()) {
      if (empty < 0) empty = n;
    } else if (identity(slots[n].link) == identity(link)) {
      message = "Already in " + slotName(n);
      Serial.println(message);
      return false;
    }
  }
  String error;
  bool saved = empty >= 0 && slotsSet(empty, {label, link, metadata}, error);
  message = saved ? "Saved " + slotName(empty) : empty < 0 ? String("No empty slot") : error;
  Serial.println(message);
  return saved;
}

String slotsExport() {
  JsonDocument doc;
  JsonArray list = doc["slots"].to<JsonArray>();
  for (int n = 0; n < SLOT_COUNT; n++) {
    if (slots[n].link.isEmpty()) continue;
    JsonObject o = list.add<JsonObject>();
    o["n"] = n;
    o["label"] = slots[n].label;
    o["link"] = slots[n].link;
    if (slots[n].metadata.length()) o["metadata"] = slots[n].metadata;
    o["kind"] = kind(slots[n]);
  }
  String json;
  serializeJsonPretty(doc, json);
  return json;
}

bool slotsImport(const String &json, String &error) {
  std::vector<Slot> loaded;
  if (!parse(json, loaded, error)) return false;
  std::vector<Slot> old(slots, slots + SLOT_COUNT);
  std::copy(loaded.begin(), loaded.end(), slots);
  if (save(error)) return true;
  std::copy(old.begin(), old.end(), slots);
  return false;
}
