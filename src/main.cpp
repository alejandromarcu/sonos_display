// Sonos "now playing" display for an ESP32-S3 with an SSD1306 128x64 I2C OLED.
//
// A background task finds the speaker's group coordinator (via SSDP + the
// ZoneGroupTopology service) and polls its AVTransport service every few
// seconds. The Arduino loop renders the latest state, scrolling long lines.

#include <Arduino.h>
#include <HTTPClient.h>
#include <IRremote.hpp>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>

#include <algorithm>
#include <vector>

#if !__has_include("secrets.h")
#error "Missing include/secrets.h: copy include/secrets.example.h to include/secrets.h and edit it."
#endif
#include "secrets.h"

// ---- Configuration ---------------------------------------------------------

static const char *ROOM_NAME = SONOS_ROOM_NAME;  // Case-insensitive; curly quotes match straight ones.
static const char *SEED_IP = SONOS_SEED_IP;      // Optional known speaker IP, tried before SSDP.
static const uint32_t POLL_INTERVAL_MS = 3000;
static const uint32_t TOPOLOGY_REFRESH_MS = 60000;   // Re-check grouping this often.
static const uint32_t SCREEN_OFF_AFTER_MS = 10 * 60 * 1000;  // Blank the OLED when idle (burn-in).
static const int PIN_SDA = 8;
static const int PIN_SCL = 9;
static const int PIN_IR = 14;  // IR receiver DAT
static const int VOLUME_STEP = 2;

enum class Command : uint8_t { PlayPause, Next, Previous, VolumeUp, VolumeDown, Mute };

struct IrButton {
  uint16_t address, command;  // NEC codes, as shown by the learning mode.
  Command action;
  bool repeats;  // Keeps firing while the button is held.
};

static const IrButton IR_BUTTONS[] = {
    {0x00, 0x15, Command::PlayPause, false},
    {0x00, 0x09, Command::Next, false},
    {0x00, 0x07, Command::Previous, false},
    {0x00, 0x40, Command::VolumeUp, true},
    {0x00, 0x19, Command::VolumeDown, true},
    {0x00, 0x44, Command::Mute, false},
};

// ---- Shared state ----------------------------------------------------------

enum class PlayState { Unknown, Playing, Paused, Stopped, Transitioning };

struct NowPlaying {
  String status;  // Shown instead of track info when non-empty (connecting, errors, ...).
  String title, artist, album;
  PlayState state = PlayState::Unknown;
  uint32_t durationS = 0, positionS = 0;
  uint32_t fetchedAtMs = 0;
};

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);

static SemaphoreHandle_t stateMutex;
static NowPlaying sharedState;
static uint32_t sharedVersion = 0;
static String sharedToast;  // Short feedback message ("Volume 23"), shown as an overlay.
static uint32_t toastVersion = 0;
static QueueHandle_t commandQueue;

static void publish(const NowPlaying &np) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  sharedState = np;
  sharedVersion++;
  xSemaphoreGive(stateMutex);
}

static void publishToast(const String &msg) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  sharedToast = msg;
  toastVersion++;
  xSemaphoreGive(stateMutex);
}

static void publishStatus(const String &msg) {
  NowPlaying np;
  np.status = msg;
  publish(np);
}

// ---- XML helpers -----------------------------------------------------------

static void appendUtf8(String &out, uint32_t cp) {
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

static String xmlUnescape(const String &s) {
  String out;
  out.reserve(s.length());
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s[i];
    int semi = c == '&' ? s.indexOf(';', i) : -1;
    if (semi < 0 || semi - i > 10) {
      out += c;
      continue;
    }
    String ent = s.substring(i + 1, semi);
    if (ent == "amp") out += '&';
    else if (ent == "lt") out += '<';
    else if (ent == "gt") out += '>';
    else if (ent == "quot") out += '"';
    else if (ent == "apos") out += '\'';
    else if (ent.length() > 1 && ent[0] == '#') {
      bool hex = ent[1] == 'x' || ent[1] == 'X';
      appendUtf8(out, strtoul(ent.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10));
    } else {
      out += c;
      continue;
    }
    i = semi;
  }
  return out;
}

// Text between <tag ...> and </tag>, or "" if absent or self-closing.
static String tagContent(const String &xml, const String &tag) {
  String open = "<" + tag;
  int start = 0;
  for (;;) {
    start = xml.indexOf(open, start);
    if (start < 0) return "";
    char next = xml[start + open.length()];
    if (next == '>' || next == ' ' || next == '/') break;
    start += open.length();
  }
  int gt = xml.indexOf('>', start);
  if (gt < 0 || xml[gt - 1] == '/') return "";
  int end = xml.indexOf("</" + tag + ">", gt);
  return end < 0 ? "" : xml.substring(gt + 1, end);
}

static String attrValue(const String &elem, const char *name) {
  String key = String(" ") + name + "=\"";
  int p = elem.indexOf(key);
  if (p < 0) return "";
  p += key.length();
  int q = elem.indexOf('"', p);
  return q < 0 ? "" : xmlUnescape(elem.substring(p, q));
}

static String normalizeName(String s) {
  s.replace("\xE2\x80\x98", "'");
  s.replace("\xE2\x80\x99", "'");
  s.toLowerCase();
  s.trim();
  return s;
}

// The U8g2 fonts cover Latin-1 only; map common typographic characters.
static String forDisplay(String s) {
  s.replace("\xE2\x80\x98", "'");
  s.replace("\xE2\x80\x99", "'");
  s.replace("\xE2\x80\x9C", "\"");
  s.replace("\xE2\x80\x9D", "\"");
  s.replace("\xE2\x80\x93", "-");
  s.replace("\xE2\x80\x94", "-");
  s.replace("\xE2\x80\xA6", "...");
  s.trim();
  return s;
}

static uint32_t parseHms(const String &s) {
  int h, m, sec;
  return sscanf(s.c_str(), "%d:%d:%d", &h, &m, &sec) == 3 ? h * 3600 + m * 60 + sec : 0;
}

// ---- Sonos UPnP ------------------------------------------------------------

static bool soapCall(const String &ip, const char *path, const char *service, const char *action,
                     const char *args, String &response) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(4000);
  if (!http.begin(client, ip, 1400, path)) return false;

  String urn = String("urn:schemas-upnp-org:service:") + service + ":1";
  http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
  http.addHeader("SOAPACTION", "\"" + urn + "#" + action + "\"");
  String body = String(
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:") +
                action + " xmlns:u=\"" + urn + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>";

  int code = http.POST(body);
  bool ok = code == 200;
  if (ok) response = http.getString();
  else Serial.printf("SOAP %s %s -> %d\n", ip.c_str(), action, code);
  http.end();
  return ok;
}

static std::vector<String> discoverSonos() {
  std::vector<String> ips;
  WiFiUDP udp;
  if (!udp.begin(1901)) return ips;
  const char *msg =
      "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\n"
      "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";
  for (int i = 0; i < 2; i++) {
    udp.beginPacket(IPAddress(239, 255, 255, 250), 1900);
    udp.write((const uint8_t *)msg, strlen(msg));
    udp.endPacket();
  }
  uint32_t start = millis();
  while (millis() - start < 2500) {
    if (udp.parsePacket() > 0) {
      String ip = udp.remoteIP().toString();
      if (std::find(ips.begin(), ips.end(), ip) == ips.end()) ips.push_back(ip);
    } else {
      delay(20);
    }
  }
  udp.stop();
  Serial.printf("SSDP found %u Sonos device(s)\n", (unsigned)ips.size());
  return ips;
}

static String coordinatorIp;   // Group coordinator for ROOM_NAME; owned by the network task.
static String knownSpeakerIp;  // Last speaker that answered a topology query.
static String roomIp;          // The room's own (visible) speaker; volume and mute go here.

static String hostFromLocation(const String &location) {  // "http://1.2.3.4:1400/..." -> "1.2.3.4"
  int hostStart = location.indexOf("://");
  int hostEnd = hostStart < 0 ? -1 : location.indexOf(':', hostStart + 3);
  return hostEnd < 0 ? "" : location.substring(hostStart + 3, hostEnd);
}

// Asks `ip` for the household topology and finds the coordinator of the group containing ROOM_NAME.
static bool findCoordinatorVia(const String &ip) {
  String resp;
  if (!soapCall(ip, "/ZoneGroupTopology/Control", "ZoneGroupTopology", "GetZoneGroupState", "", resp))
    return false;
  String zgs = xmlUnescape(tagContent(resp, "ZoneGroupState"));
  String want = normalizeName(ROOM_NAME);

  for (int pos = 0;;) {
    int gStart = zgs.indexOf("<ZoneGroup ", pos);
    int gEnd = gStart < 0 ? -1 : zgs.indexOf("</ZoneGroup>", gStart);
    if (gEnd < 0) break;
    String group = zgs.substring(gStart, gEnd);
    pos = gEnd;

    // Stereo pairs list both speakers under the room's name; the secondary one is Invisible.
    String memberIp;
    for (int m = 0; (m = group.indexOf("<ZoneGroupMember ", m)) >= 0;) {
      int mEnd = group.indexOf('>', m);
      String member = group.substring(m, mEnd);
      if (normalizeName(attrValue(member, "ZoneName")) == want && attrValue(member, "Invisible") != "1") {
        memberIp = hostFromLocation(attrValue(member, "Location"));
        break;
      }
      m = mEnd;
    }
    if (memberIp.isEmpty()) continue;

    String coordUuid = attrValue(group.substring(0, group.indexOf('>')), "Coordinator");
    int c = group.indexOf("UUID=\"" + coordUuid + "\"");
    if (c < 0) return false;
    int cStart = group.lastIndexOf('<', c);
    String newIp = hostFromLocation(attrValue(group.substring(cStart, group.indexOf('>', c)), "Location"));
    if (newIp.isEmpty()) return false;

    if (newIp != coordinatorIp) Serial.printf("Coordinator for %s: %s\n", ROOM_NAME, newIp.c_str());
    coordinatorIp = newIp;
    roomIp = memberIp;
    knownSpeakerIp = ip;
    return true;
  }
  Serial.printf("Room '%s' not in topology from %s\n", ROOM_NAME, ip.c_str());
  return false;
}

static bool resolveCoordinator() {
  std::vector<String> tried;
  auto tryIp = [&](const String &ip) {
    if (ip.isEmpty() || std::find(tried.begin(), tried.end(), ip) != tried.end()) return false;
    tried.push_back(ip);
    return findCoordinatorVia(ip);
  };
  if (tryIp(coordinatorIp) || tryIp(knownSpeakerIp) || tryIp(SEED_IP)) return true;
  for (const String &ip : discoverSonos())
    if (tryIp(ip)) return true;
  return false;
}

static bool pollNowPlaying(NowPlaying &np) {
  const char *args = "<InstanceID>0</InstanceID>";
  const char *path = "/MediaRenderer/AVTransport/Control";
  String transport, position;
  if (!soapCall(coordinatorIp, path, "AVTransport", "GetTransportInfo", args, transport)) return false;
  if (!soapCall(coordinatorIp, path, "AVTransport", "GetPositionInfo", args, position)) return false;
  np.fetchedAtMs = millis();

  String st = tagContent(transport, "CurrentTransportState");
  np.state = st == "PLAYING"           ? PlayState::Playing
             : st == "PAUSED_PLAYBACK" ? PlayState::Paused
             : st == "STOPPED"         ? PlayState::Stopped
             : st == "TRANSITIONING"   ? PlayState::Transitioning
                                       : PlayState::Unknown;

  String uri = xmlUnescape(tagContent(position, "TrackURI"));
  if (uri.startsWith("x-rincon:")) {  // The room joined another group; find the new coordinator.
    coordinatorIp = "";
    return false;
  }
  np.durationS = parseHms(tagContent(position, "TrackDuration"));
  np.positionS = parseHms(tagContent(position, "RelTime"));

  // TrackMetaData is DIDL-Lite XML escaped inside the SOAP response, so values are escaped twice.
  String didl = xmlUnescape(tagContent(position, "TrackMetaData"));
  np.title = forDisplay(xmlUnescape(tagContent(didl, "dc:title")));
  np.artist = forDisplay(xmlUnescape(tagContent(didl, "dc:creator")));
  np.album = forDisplay(xmlUnescape(tagContent(didl, "upnp:album")));

  // Radio streams put "Artist - Title" in streamContent and the station in dc:title.
  String stream = forDisplay(xmlUnescape(tagContent(didl, "r:streamContent")));
  if (stream.length() && !stream.startsWith("ZPSTR_")) {
    np.album = np.title;
    int dash = stream.indexOf(" - ");
    np.artist = dash > 0 ? stream.substring(0, dash) : "";
    np.title = dash > 0 ? stream.substring(dash + 3) : stream;
  }
  if (uri.startsWith("x-sonos-htastream:")) np.title = "TV";
  else if (uri.startsWith("x-rincon-stream:")) np.title = "Line-In";

  if (np.title.isEmpty()) np.status = np.state == PlayState::Playing ? "Playing" : "Nothing playing";
  return true;
}

// Transport commands go to the group coordinator; volume and mute to this room only.
static void runCommand(Command cmd, PlayState current) {
  const char *av = "/MediaRenderer/AVTransport/Control";
  const char *rc = "/MediaRenderer/RenderingControl/Control";
  const char *instance = "<InstanceID>0</InstanceID>";
  String channel = String(instance) + "<Channel>Master</Channel>";
  String resp;
  bool ok = false;
  switch (cmd) {
    case Command::PlayPause:
      if (current == PlayState::Playing || current == PlayState::Transitioning) {
        ok = soapCall(coordinatorIp, av, "AVTransport", "Pause", instance, resp);
        if (ok) publishToast("Pause");
      } else {
        ok = soapCall(coordinatorIp, av, "AVTransport", "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>", resp);
        if (ok) publishToast("Play");
      }
      break;
    case Command::Next:
      ok = soapCall(coordinatorIp, av, "AVTransport", "Next", instance, resp);
      if (ok) publishToast("Next");
      break;
    case Command::Previous:
      ok = soapCall(coordinatorIp, av, "AVTransport", "Previous", instance, resp);
      if (ok) publishToast("Previous");
      break;
    case Command::VolumeUp:
    case Command::VolumeDown: {
      int step = cmd == Command::VolumeUp ? VOLUME_STEP : -VOLUME_STEP;
      String args = channel + "<Adjustment>" + step + "</Adjustment>";
      ok = soapCall(roomIp, rc, "RenderingControl", "SetRelativeVolume", args.c_str(), resp);
      if (ok) publishToast("Volume " + tagContent(resp, "NewVolume"));
      break;
    }
    case Command::Mute: {
      if (!soapCall(roomIp, rc, "RenderingControl", "GetMute", channel.c_str(), resp)) break;
      bool mute = tagContent(resp, "CurrentMute") != "1";
      String args = channel + "<DesiredMute>" + (mute ? "1" : "0") + "</DesiredMute>";
      ok = soapCall(roomIp, rc, "RenderingControl", "SetMute", args.c_str(), resp);
      if (ok) publishToast(mute ? "Muted" : "Unmuted");
      break;
    }
  }
  if (!ok) publishToast("Command failed");
}

static void networkTask(void *) {
  uint32_t lastTopologyMs = 0;
  uint32_t connectingSinceMs = millis();
  int failures = 0;
  NowPlaying np;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      publishStatus("Connecting to WiFi...");
      if (millis() - connectingSinceMs > 30000) {  // Calling this mid-attempt just fails, so wait first.
        Serial.printf("WiFi still not connected (status %d), retrying\n", WiFi.status());
        connectingSinceMs = millis();
        WiFi.reconnect();
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      if (WiFi.status() == WL_CONNECTED)
        Serial.printf("WiFi connected, IP %s\n", WiFi.localIP().toString().c_str());
      continue;
    }
    connectingSinceMs = millis();

    if (coordinatorIp.isEmpty() || millis() - lastTopologyMs > TOPOLOGY_REFRESH_MS) {
      bool hadCoordinator = !coordinatorIp.isEmpty();
      if (!hadCoordinator) publishStatus("Finding speaker...");
      if (resolveCoordinator()) {
        lastTopologyMs = millis();
      } else if (!hadCoordinator) {
        publishStatus("Speaker not found");
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }  // Otherwise keep using the coordinator we already had.
    }

    np = NowPlaying();
    if (pollNowPlaying(np)) {
      failures = 0;
      publish(np);
    } else if (++failures >= 3) {
      failures = 0;
      coordinatorIp = "";
      publishStatus("Speaker unreachable");
    }

    // Wait for the next poll, waking early for remote commands (then polling right away to show the result).
    Command cmd;
    if (xQueueReceive(commandQueue, &cmd, pdMS_TO_TICKS(POLL_INTERVAL_MS)) == pdTRUE) {
      do runCommand(cmd, np.state);
      while (xQueueReceive(commandQueue, &cmd, 0) == pdTRUE);
    }
  }
}

// ---- IR remote -------------------------------------------------------------

static String overlayText;  // Shown briefly at the bottom of the screen.
static uint32_t overlayUntil = 0;

static void showOverlay(const String &text, uint32_t ms) {
  overlayText = text;
  overlayUntil = millis() + ms;
}

// Queues the command for a known button. Unknown buttons are logged and shown (learning mode)
// so they can be added to IR_BUTTONS. Returns true when a button was pressed.
static bool pollIr() {
  if (!IrReceiver.decode()) return false;
  IRData d = IrReceiver.decodedIRData;
  IrReceiver.resume();
  bool repeat = d.flags & IRDATA_FLAGS_IS_REPEAT;
  if (!repeat)
    Serial.printf("IR: protocol=%s address=0x%04X command=0x%04X raw=0x%llX\n", getProtocolString(d.protocol),
                  d.address, d.command, (unsigned long long)d.decodedRawData);

  static const IrButton *lastButton = nullptr;
  static uint32_t lastSentMs = 0;
  for (const IrButton &b : IR_BUTTONS) {
    if (d.protocol != NEC || d.address != b.address || d.command != b.command) continue;
    // Held buttons only repeat volume, and at a limited rate; others need a fresh press.
    uint32_t now = millis();
    if (repeat && !b.repeats) return true;
    if (&b == lastButton && now - lastSentMs < (b.repeats ? 150u : 400u)) return true;
    lastButton = &b;
    lastSentMs = now;
    xQueueSend(commandQueue, &b.action, 0);
    return true;
  }

  if (repeat) return false;
  char buf[40];
  snprintf(buf, sizeof buf, "IR %s %X:%X", getProtocolString(d.protocol), d.address, d.command);
  showOverlay(buf, 3000);
  return true;
}

// ---- Rendering -------------------------------------------------------------

struct Marquee {
  uint32_t holdUntil = 0;
  void reset() { holdUntil = millis() + 2000; }
};

// Draws `text` at baseline `y`, scrolling it horizontally when it is wider than the screen.
static void drawScrolling(const String &text, int y, Marquee &mq) {
  int w = display.getUTF8Width(text.c_str());
  if (w <= display.getDisplayWidth()) {
    display.drawUTF8(0, y, text.c_str());
    return;
  }
  const int gap = 24;
  uint32_t now = millis();
  int offset = now < mq.holdUntil ? 0 : (now - mq.holdUntil) / 30;  // ~33 px/s
  if (offset >= w + gap) {
    mq.reset();
    offset = 0;
  }
  display.drawUTF8(-offset, y, text.c_str());
  display.drawUTF8(-offset + w + gap, y, text.c_str());
}

static String fmtTime(unsigned s) {
  char buf[16];
  if (s >= 3600) snprintf(buf, sizeof buf, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
  else snprintf(buf, sizeof buf, "%u:%02u", s / 60, s % 60);
  return buf;
}

static void drawStateIcon(PlayState s) {
  switch (s) {
    case PlayState::Playing: display.drawTriangle(0, 0, 0, 8, 5, 4); break;
    case PlayState::Paused: display.drawBox(0, 1, 2, 7); display.drawBox(4, 1, 2, 7); break;
    case PlayState::Stopped: display.drawBox(0, 1, 7, 7); break;
    default: break;
  }
}

static void render(const NowPlaying &np, Marquee &mqTitle, Marquee &mqArtist, Marquee &mqAlbum) {
  display.clearBuffer();

  // Header (fits in the top 16 rows, which are yellow on two-color panels).
  display.setFont(u8g2_font_5x8_tf);
  drawStateIcon(np.state);
  int nameRight = 9 + display.getUTF8Width(ROOM_NAME);
  display.drawUTF8(9, 8, ROOM_NAME);
  if (np.status.isEmpty() && np.durationS > 0) {
    uint32_t pos = np.positionS;
    if (np.state == PlayState::Playing) pos += (millis() - np.fetchedAtMs) / 1000;
    pos = std::min(pos, np.durationS);
    String t = fmtTime(pos) + "/" + fmtTime(np.durationS);
    if (nameRight + 4 > 128 - display.getUTF8Width(t.c_str())) t = fmtTime(pos);
    display.drawUTF8(128 - display.getUTF8Width(t.c_str()), 8, t.c_str());
    display.drawFrame(0, 11, 128, 4);
    display.drawBox(0, 11, 128 * pos / np.durationS, 4);
  } else {
    display.drawHLine(0, 12, 128);
  }

  if (np.status.length()) {
    display.setFont(u8g2_font_6x10_tf);
    int w = display.getUTF8Width(np.status.c_str());
    display.drawUTF8(std::max(0, (128 - w) / 2), 42, np.status.c_str());
  } else {
    display.setFont(u8g2_font_helvB12_tf);
    drawScrolling(np.title, 31, mqTitle);
    display.setFont(u8g2_font_6x10_tf);
    drawScrolling(np.artist, 46, mqArtist);
    drawScrolling(np.album, 59, mqAlbum);
  }

  if (millis() < overlayUntil) {
    display.setFont(u8g2_font_6x10_tf);
    display.setDrawColor(0);
    display.drawBox(0, 50, 128, 14);
    display.setDrawColor(1);
    display.drawFrame(0, 50, 128, 14);
    display.drawUTF8(3, 61, overlayText.c_str());
  }
  display.sendBuffer();
}

// ---- Arduino entry points --------------------------------------------------

void setup() {
  Serial.begin(115200);
  stateMutex = xSemaphoreCreateMutex();
  commandQueue = xQueueCreate(8, sizeof(Command));

  display.setBusClock(400000);
  display.begin();

  IrReceiver.begin(PIN_IR, DISABLE_LED_FEEDBACK);

  publishStatus("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  xTaskCreatePinnedToCore(networkTask, "sonos", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  static NowPlaying np;
  static uint32_t seenVersion = UINT32_MAX;
  static uint32_t seenToast = 0;
  static Marquee mqTitle, mqArtist, mqAlbum;
  static uint32_t lastActiveMs = 0;
  static bool screenOn = true;

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (sharedVersion != seenVersion) {
    seenVersion = sharedVersion;
    if (sharedState.title != np.title || sharedState.status != np.status) mqTitle.reset();
    if (sharedState.artist != np.artist) mqArtist.reset();
    if (sharedState.album != np.album) mqAlbum.reset();
    bool changed = sharedState.title != np.title || sharedState.state != np.state;
    if (changed && !sharedState.title.isEmpty() && sharedState.status.isEmpty())
      Serial.printf("Now: [%s] %s - %s (%s)\n", sharedState.state == PlayState::Playing ? "playing" : "paused",
                    sharedState.title.c_str(), sharedState.artist.c_str(), sharedState.album.c_str());
    np = sharedState;
  }
  bool toast = toastVersion != seenToast;
  if (toast) {
    seenToast = toastVersion;
    showOverlay(sharedToast, 2000);
  }
  xSemaphoreGive(stateMutex);

  bool irPressed = pollIr() || toast;

  // Turn the panel off after a while without playback to avoid OLED burn-in.
  bool active = np.state == PlayState::Playing || np.state == PlayState::Transitioning || lastActiveMs == 0 ||
                irPressed;
  if (active) lastActiveMs = millis();
  bool wantOn = millis() - lastActiveMs < SCREEN_OFF_AFTER_MS;
  if (wantOn != screenOn) {
    screenOn = wantOn;
    display.setPowerSave(!wantOn);
  }

  if (screenOn) render(np, mqTitle, mqArtist, mqAlbum);
  else delay(200);
}
