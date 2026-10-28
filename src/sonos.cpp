// A background task finds the speaker's group coordinator (via SSDP + the ZoneGroupTopology
// service) and polls its AVTransport service every few seconds, running queued remote commands
// in between.

#include "sonos.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include <algorithm>
#include <vector>

#include "config.h"

// ---- Shared state ----------------------------------------------------------

static SemaphoreHandle_t stateMutex;
static NowPlaying sharedState;
static uint32_t sharedVersion = 0;
static String sharedToast;
static uint32_t toastVersion = 0;
static QueueHandle_t commandQueue;

static void publish(const NowPlaying &np) {
  // Only the network task writes sharedState, so reading it here needs no lock.
  bool changed = np.title != sharedState.title || np.state != sharedState.state;
  if (changed && !np.title.isEmpty() && np.status.isEmpty())
    Serial.printf("Now: [%s] %s - %s (%s)\n", np.state == PlayState::Playing ? "playing" : "paused",
                  np.title.c_str(), np.artist.c_str(), np.album.c_str());

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  sharedState = np;
  sharedVersion++;
  xSemaphoreGive(stateMutex);
}

static void publishToast(const String &msg) {
  Serial.println(msg);
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

void sonosSend(Command cmd) { xQueueSend(commandQueue, &cmd, 0); }

bool sonosTakeState(NowPlaying &np) {
  static uint32_t seen = UINT32_MAX;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  bool changed = sharedVersion != seen;
  if (changed) {
    seen = sharedVersion;
    np = sharedState;
  }
  xSemaphoreGive(stateMutex);
  return changed;
}

bool sonosTakeToast(String &msg) {
  static uint32_t seen = 0;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  bool changed = toastVersion != seen;
  if (changed) {
    seen = toastVersion;
    msg = sharedToast;
  }
  xSemaphoreGive(stateMutex);
  return changed;
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

void sonosStart() {
  stateMutex = xSemaphoreCreateMutex();
  commandQueue = xQueueCreate(8, sizeof(Command));
  publishStatus("Connecting to WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  xTaskCreatePinnedToCore(networkTask, "sonos", 8192, nullptr, 1, nullptr, 0);
}
