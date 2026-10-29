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
#include "share_link.h"

// ---- Shared state ----------------------------------------------------------

// A queued remote command, or a request to play pendingLink.
struct Request {
  bool play;
  Command cmd;
};

static SemaphoreHandle_t stateMutex;
static NowPlaying sharedState;
static uint32_t sharedVersion = 0;
static String sharedToast;
static uint32_t toastVersion = 0;
static String pendingLink, pendingMetadata, pendingName;  // Guarded by stateMutex, like the above.
static String sourceLink, sourceMetadata, sourceTitle;
static String sharedSpeakerIp;  // knownSpeakerIp, for the web page's Favorites.
static uint32_t sourceVersion = 0;
static QueueHandle_t commandQueue;  // Of Request.

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

static void publishSource(const String &link, const String &metadata, const String &title) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  sourceLink = link;
  sourceMetadata = metadata;
  sourceTitle = title;
  sourceVersion++;
  xSemaphoreGive(stateMutex);
}

static void publishStatus(const String &msg) {
  NowPlaying np;
  np.status = msg;
  publish(np);
}

void sonosSend(Command cmd) {
  Request req{false, cmd};
  xQueueSend(commandQueue, &req, 0);
}

void sonosPlay(const String &link, const String &metadata, const String &name) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  pendingLink = link;
  pendingMetadata = metadata;
  pendingName = name;
  xSemaphoreGive(stateMutex);
  Request req{true};
  xQueueSend(commandQueue, &req, 0);
}

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

bool sonosTakeSource(String &link, String &metadata, String &title) {
  static uint32_t seen = 0;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  bool changed = sourceVersion != seen;
  if (changed) {
    seen = sourceVersion;
    link = sourceLink;
    metadata = sourceMetadata;
    title = sourceTitle;
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

static String xmlEscape(const String &s) {
  String out;
  out.reserve(s.length() + s.length() / 4);
  for (char c : s) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
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
                     const char *args, String &response, uint16_t timeoutMs = 4000) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(timeoutMs);
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
  else if (code == 500)  // A UPnP fault; its errorCode says why.
    Serial.printf("SOAP %s %s -> error %s\n", ip.c_str(), action, tagContent(http.getString(), "errorCode").c_str());
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

static String coordinatorIp;    // Group coordinator for ROOM_NAME; owned by the network task.
static String coordinatorUuid;  // Its "RINCON_..." id, which names its queue.
static String knownSpeakerIp;   // Last speaker that answered a topology query.
static String roomIp;           // The room's own (visible) speaker; volume and mute go here.

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
    coordinatorUuid = coordUuid;
    roomIp = memberIp;
    knownSpeakerIp = ip;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    sharedSpeakerIp = ip;
    xSemaphoreGive(stateMutex);
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

// ---- Saving what's playing ---------------------------------------------------
//
// Sonos reports what the queue was started from (EnqueuedTransportURI) only in UPnP events, so
// Command::SaveToSlot subscribes to the coordinator's AVTransport events, takes the first one (which
// holds the whole state) and unsubscribes.

static const uint16_t EVENT_PORT = 3500;  // The speaker sends the event here.

// Sends a SUBSCRIBE (with `sid` empty) or UNSUBSCRIBE for AVTransport events; returns the SID.
static String eventSubscription(const char *method, const String &sid) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(4000);
  if (!http.begin(client, coordinatorIp, 1400, "/MediaRenderer/AVTransport/Event")) return "";
  const char *keys[] = {"SID"};
  http.collectHeaders(keys, 1);
  if (sid.isEmpty()) {
    http.addHeader("CALLBACK", "<http://" + WiFi.localIP().toString() + ":" + EVENT_PORT + "/>");
    http.addHeader("NT", "upnp:event");
    http.addHeader("TIMEOUT", "Second-60");
  } else {
    http.addHeader("SID", sid);
  }
  int code = http.sendRequest(method);
  String result = code == 200 ? http.header("SID") : "";
  if (code != 200) Serial.printf("%s %s -> %d\n", method, coordinatorIp.c_str(), code);
  http.end();
  return result;
}

// Accepts the speaker's NOTIFY request, answers it and returns its body.
static bool readEvent(WiFiServer &listener, String &body) {
  uint32_t start = millis();
  WiFiClient c;
  while (!(c = listener.accept()) && millis() - start < 3000) delay(10);
  if (!c) return false;
  int remaining = 0;
  for (;;) {  // Headers
    String line = c.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) break;
    line.toLowerCase();
    if (line.startsWith("content-length:")) remaining = line.substring(15).toInt();
  }
  char buf[512];
  while (remaining > 0 && millis() - start < 5000) {
    int n = c.read((uint8_t *)buf, std::min<int>(sizeof buf, remaining));
    if (n > 0) {
      body.concat(buf, n);
      remaining -= n;
    } else {
      delay(5);
    }
  }
  c.print("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  c.stop();
  return remaining == 0;
}

// The coordinator's AVTransport state as event XML (<Event><InstanceID><TransportState val=.../>...).
static bool fetchTransportEvent(String &event) {
  WiFiServer listener(EVENT_PORT);
  listener.begin();
  String body;
  String sid = eventSubscription("SUBSCRIBE", "");
  bool ok = sid.length() && readEvent(listener, body);
  if (sid.length()) eventSubscription("UNSUBSCRIBE", sid);
  listener.end();
  event = xmlUnescape(tagContent(body, "LastChange"));
  return ok && event.length();
}

// The val attribute of <name val="..."/> in event XML.
static String eventValue(const String &event, const String &name) {
  int p = event.indexOf("<" + name + " ");
  return p < 0 ? "" : attrValue(event.substring(p, event.indexOf('>', p)), "val");
}

// Apple Music items as Sonos names them ("libraryalbum:l.IeG3Znh") and where their links point. The
// library ones come first, as "album:" is also part of "libraryalbum:".
struct AppleItem {
  const char *type, *path;
};
static const AppleItem APPLE_ITEMS[] = {
    {"libraryalbum", "library/albums/"}, {"libraryplaylist", "library/playlist/"},
    {"album", "us/album/"},              {"playlist", "us/playlist/"},
    {"song", "us/song/"},
};

// Turns what Sonos enqueued back into a share link. Spotify's URIs hold the item's own URI
// ("x-rincon-cpcontainer:1006206cspotify%3aplaylist%3a<id>?sid=12&..."); Apple Music's
// ("x-rincon-cpcontainer:1004206calbum%3a<id>?sid=204&...") are told apart by the service's id or
// account token. Anything else gives "".
static String shareLinkFor(const String &uri, const String &didl) {
  String item = uri;
  item.replace("%3a", ":");
  item.replace("%3A", ":");
  ShareLink share;
  String link;
  if (item.indexOf("spotify:") >= 0 && parseShareLink(item, share)) {
    link = "https://open.spotify.com/" + share.type + "/" + share.id.substring(share.id.lastIndexOf(':') + 1);
  } else if (uri.indexOf("sid=204&") >= 0 || didl.indexOf("SA_RINCON52231_") >= 0) {
    for (const AppleItem &a : APPLE_ITEMS) {
      int start = item.indexOf(String(a.type) + ":");
      if (start < 0) continue;
      start += strlen(a.type) + 1;
      int end = start;
      while (isalnum((unsigned char)item[end]) || item[end] == '.' || item[end] == '-' || item[end] == '_') end++;
      String id = item.substring(start, end);
      if (id.endsWith(".mp4")) id.remove(id.length() - 4);  // Songs: "x-sonos-http:song%3a<id>.mp4?..."
      link = String("https://music.apple.com/") + a.path + id;
      break;
    }
  }
  return parseShareLink(link, share) ? link : "";
}

// Sources that can't be played again: TV, line-in, Spotify Connect and AirPlay, and following
// another group.
static bool replayable(const String &uri) {
  for (const char *scheme : {"x-sonos-htastream:", "x-rincon-stream:", "x-sonos-vli:", "x-rincon:"})
    if (uri.startsWith(scheme)) return false;
  return uri.length() > 0;
}

// Saves what's playing (Command::SaveToSlot): for the queue, what it was started from, and
// otherwise the station or stream itself. Spotify and Apple Music items become share links; anything
// else is kept as the URI and metadata Sonos had, to play it again the same way.
static void saveSource() {
  String event;
  if (!fetchTransportEvent(event)) {
    publishToast("Command failed");
    return;
  }
  String transport = eventValue(event, "AVTransportURI");
  bool queue = transport.startsWith("x-rincon-queue:");
  String uri = queue ? eventValue(event, "r:EnqueuedTransportURI") : transport;
  String didl = eventValue(event, queue ? "r:EnqueuedTransportURIMetaData" : "AVTransportURIMetaData");
  if (!replayable(uri) || didl.isEmpty()) {
    Serial.printf("Can't save: playing %s, enqueued %s\n", transport.c_str(), uri.c_str());
    publishToast("Can't save this");
    return;
  }
  String link = shareLinkFor(uri, didl);
  publishSource(link.length() ? link : uri, link.length() ? "" : didl,
                forDisplay(xmlUnescape(tagContent(didl, "dc:title"))));
}

// Music services by Sonos id; their account token number is id * 256 + 7 ("SA_RINCON3079_X_...").
struct MusicServiceName {
  uint16_t id;
  const char *name;
};
static const MusicServiceName SERVICE_NAMES[] = {
    {9, "Spotify"},       {12, "Spotify"},          {201, "Amazon Music"}, {204, "Apple Music"},
    {254, "TuneIn"},      {284, "YouTube Music"},   {303, "Sonos Radio"},
};

String sonosDescribeItem(const String &metadata) {
  int token = metadata.indexOf("SA_RINCON");
  long id = token < 0 ? 0 : (metadata.substring(token + 9).toInt() - 7) / 256;
  String service = "Saved ";
  for (const MusicServiceName &s : SERVICE_NAMES)
    if (s.id == id) service = String(s.name) + " ";
  String cls = tagContent(metadata, "upnp:class");
  if (cls.indexOf("audioBroadcast") >= 0) return service + "station";
  if (cls.indexOf("musicAlbum") >= 0) return service + "album";
  if (cls.startsWith("object.container")) return service + "playlist";
  return service + "track";
}

bool sonosFavorites(std::vector<SonosItem> &items) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  String ip = sharedSpeakerIp;
  xSemaphoreGive(stateMutex);
  const char *args =
      "<ObjectID>FV:2</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag><Filter>dc:title,res,r:resMD</Filter>"
      "<StartingIndex>0</StartingIndex><RequestedCount>200</RequestedCount><SortCriteria></SortCriteria>";
  String resp;
  if (ip.isEmpty() || !soapCall(ip, "/MediaServer/ContentDirectory/Control", "ContentDirectory", "Browse", args, resp, 8000))
    return false;
  // Result is DIDL-Lite escaped in the SOAP response; each favorite's own metadata (resMD) is escaped again.
  String didl = xmlUnescape(tagContent(resp, "Result"));
  for (int pos = 0; (pos = didl.indexOf("<item ", pos)) >= 0;) {
    int end = didl.indexOf("</item>", pos);
    if (end < 0) break;
    String item = didl.substring(pos, end);
    pos = end;
    String uri = xmlUnescape(tagContent(item, "res"));
    String metadata = xmlUnescape(tagContent(item, "r:resMD"));
    if (!replayable(uri) || metadata.isEmpty()) continue;  // Folders and shortcuts have no res.
    String link = shareLinkFor(uri, metadata);
    if (link.length()) metadata = "";
    items.push_back({forDisplay(xmlUnescape(tagContent(item, "dc:title"))), link.length() ? link : uri, metadata});
  }
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
    case Command::SaveToSlot:
      saveSource();  // Reports its own result.
      return;
  }
  if (!ok) publishToast("Command failed");
}

// How Sonos refers to each kind of shared item: the URI to queue, and the id prefix and class in
// its metadata. From SoCo's share-link plugin (soco/plugins/sharelink.py).
struct ShareMagic {
  const char *type, *uriPrefix, *idPrefix, *upnpClass;
};
static const ShareMagic SHARE_MAGIC[] = {
    {"album", "x-rincon-cpcontainer:1004206c", "00040000", "object.container.album.musicAlbum"},
    {"playlist", "x-rincon-cpcontainer:1006206c", "1006206c", "object.container.playlistContainer"},
    {"show", "x-rincon-cpcontainer:1006206c", "1006206c", "object.container.playlistContainer"},
    {"track", "", "00032020", "object.item.audioItem.musicTrack"},
    {"episode", "", "00032020", "object.item.audioItem.musicTrack"},
    {"song", "", "10032020", "object.item.audioItem.musicTrack"},
    // Apple Music library items, as the Sonos app queues them.
    {"libraryalbum", "x-rincon-cpcontainer:1004206c", "1004206c", "object.container.album.musicAlbum"},
    {"libraryplaylist", "x-rincon-cpcontainer:1006206c", "1006206c", "object.container.playlistContainer"},
};

// The number in each service's account token ("SA_RINCON2311_X_#Svc2311-0-Token"). Spotify's
// depends on the region (2311 in Europe, 3079 in the US), so the speaker is offered each in turn
// until it accepts one, which is then tried first.
static std::vector<uint16_t> spotifyTokens = {2311, 3079};
static std::vector<uint16_t> appleMusicTokens = {52231};

static const char *const AV_PATH = "/MediaRenderer/AVTransport/Control";

static String queueUri() { return "x-rincon-queue:" + coordinatorUuid + "#0"; }

static bool clearQueue() {
  String resp;
  return soapCall(coordinatorIp, AV_PATH, "AVTransport", "RemoveAllTracksFromQueue", "<InstanceID>0</InstanceID>", resp);
}

// Sonos looks the item up with the service, which can take a while for long playlists.
static bool addToQueue(const String &uri, const String &didl) {
  String args = String("<InstanceID>0</InstanceID><EnqueuedURI>") + xmlEscape(uri) + "</EnqueuedURI><EnqueuedURIMetaData>" +
                xmlEscape(didl) +
                "</EnqueuedURIMetaData><DesiredFirstTrackNumberEnqueued>0</DesiredFirstTrackNumberEnqueued>"
                "<EnqueueAsNext>0</EnqueueAsNext>";
  String resp;
  return soapCall(coordinatorIp, AV_PATH, "AVTransport", "AddURIToQueue", args.c_str(), resp, 15000);
}

// Makes `uri` the speaker's source and plays it; the queue from its first track.
static bool playFrom(const String &uri, const String &didl) {
  String args = String("<InstanceID>0</InstanceID><CurrentURI>") + xmlEscape(uri) + "</CurrentURI><CurrentURIMetaData>" +
                xmlEscape(didl) + "</CurrentURIMetaData>";
  String resp;
  if (!soapCall(coordinatorIp, AV_PATH, "AVTransport", "SetAVTransportURI", args.c_str(), resp)) return false;
  if (uri.startsWith("x-rincon-queue:")) {
    const char *firstTrack = "<InstanceID>0</InstanceID><Unit>TRACK_NR</Unit><Target>1</Target>";
    soapCall(coordinatorIp, AV_PATH, "AVTransport", "Seek", firstTrack, resp);  // Best effort.
  }
  return soapCall(coordinatorIp, AV_PATH, "AVTransport", "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>", resp);
}

// Replaces the queue with a share link's album, playlist or track and plays it from the start.
static bool playShareLink(const String &link) {
  ShareLink share;
  const ShareMagic *magic = nullptr;
  if (parseShareLink(link, share))
    for (const ShareMagic &m : SHARE_MAGIC)
      if (share.type == m.type) magic = &m;
  if (!magic) {
    Serial.printf("Can't play %s\n", link.c_str());
    return false;
  }
  String id = share.id;
  id.replace(":", "%3a");
  String uri = magic->uriPrefix + id;
  if (!clearQueue()) return false;

  std::vector<uint16_t> &tokens = share.service == MusicService::Spotify ? spotifyTokens : appleMusicTokens;
  bool added = false;
  for (size_t i = 0; i < tokens.size() && !added; i++) {
    String token = String(tokens[i]);
    String didl = String(
                      "<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
                      "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
                      "xmlns:r=\"urn:schemas-rinconnetworks-com:metadata-1-0/\" "
                      "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\"><item id=\"") +
                  magic->idPrefix + id + "\" parentID=\"-1\" restricted=\"true\"><dc:title></dc:title><upnp:class>" +
                  magic->upnpClass +
                  "</upnp:class><desc id=\"cdudn\" nameSpace=\"urn:schemas-rinconnetworks-com:metadata-1-0/\">"
                  "SA_RINCON" + token + "_X_#Svc" + token + "-0-Token</desc></item></DIDL-Lite>";
    if (addToQueue(uri, didl)) {
      added = true;
      std::rotate(tokens.begin(), tokens.begin() + i, tokens.begin() + i + 1);
    }
  }
  return added && playFrom(queueUri(), "");
}

// Plays an item saved from Sonos as Sonos had it: albums and playlists (containers) through the
// queue, and stations and tracks directly.
static bool playSonosItem(const String &uri, const String &didl) {
  if (tagContent(didl, "upnp:class").startsWith("object.container"))
    return clearQueue() && addToQueue(uri, didl) && playFrom(queueUri(), "");
  return playFrom(uri, didl);
}

static void playPending() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  String link = pendingLink, metadata = pendingMetadata, name = pendingName;
  pendingLink = "";  // Several quick requests play only the latest one, once.
  xSemaphoreGive(stateMutex);
  if (link.isEmpty()) return;
  bool ok = metadata.length() ? playSonosItem(link, metadata) : playShareLink(link);
  publishToast(ok ? name : String("Play failed"));
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
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WiFi connected, web page at http://%s.local\n", HOSTNAME);
        publishToast("IP " + WiFi.localIP().toString());
      }
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
    Request req;
    if (xQueueReceive(commandQueue, &req, pdMS_TO_TICKS(POLL_INTERVAL_MS)) == pdTRUE) {
      do {
        if (req.play) playPending();
        else runCommand(req.cmd, np.state);
      } while (xQueueReceive(commandQueue, &req, 0) == pdTRUE);
    }
  }
}

void sonosStart() {
  stateMutex = xSemaphoreCreateMutex();
  commandQueue = xQueueCreate(8, sizeof(Request));
  publishStatus("Connecting to WiFi...");

  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  xTaskCreatePinnedToCore(networkTask, "sonos", 8192, nullptr, 1, nullptr, 0);
}
