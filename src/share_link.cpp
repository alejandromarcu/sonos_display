// Recognizes the same links as SoCo's share-link plugin (soco/plugins/sharelink.py).

#include "share_link.h"

static bool isWordChar(char c) { return isalnum((unsigned char)c) || c == '_'; }

// open.spotify.com/album/<id>?si=..., open.spotify.com/intl-de/album/<id> or spotify:album:<id>.
static bool parseSpotify(const String &link, ShareLink &out) {
  int start = link.indexOf("spotify");
  if (start < 0) return false;
  for (const char *type : {"album", "playlist", "track", "episode", "show"}) {
    for (int p = link.indexOf(type, start); p > 0; p = link.indexOf(type, p + 1)) {
      int idStart = p + strlen(type) + 1, idEnd = idStart;
      bool delimited = (link[p - 1] == '/' || link[p - 1] == ':') &&
                       (link[idStart - 1] == '/' || link[idStart - 1] == ':');
      while (isWordChar(link[idEnd])) idEnd++;  // String's [] gives 0 past the end.
      if (!delimited || idEnd == idStart) continue;
      out.service = MusicService::Spotify;
      out.type = type;
      out.id = "spotify:" + out.type + ":" + link.substring(idStart, idEnd);
      return true;
    }
  }
  return false;
}

static bool allDigits(const String &s) {
  for (int i = 0; i < (int)s.length(); i++)
    if (!isdigit((unsigned char)s[i])) return false;
  return s.length() > 0;
}

// music.apple.com/<country>/album/<name>/<id> (with ?i=<song id> for one song on it),
// music.apple.com/<country>/song/<name>/<id> or music.apple.com/<country>/playlist/<name>/pl.<id>.
// The <name> part may be missing.
static bool parseAppleMusic(const String &link, ShareLink &out) {
  const char *host = "music.apple.com/";
  int pathStart = link.indexOf(host);
  if (pathStart < 0) return false;
  pathStart += strlen(host);
  int pathEnd = pathStart;
  while (pathEnd < (int)link.length() && link[pathEnd] != '?' && link[pathEnd] != '#') pathEnd++;
  String path = link.substring(pathStart, pathEnd);
  if (path.endsWith("/")) path.remove(path.length() - 1);

  int typeStart = path.indexOf('/') + 1;  // After the country code.
  int typeEnd = typeStart > 0 ? path.indexOf('/', typeStart) : -1;
  if (typeEnd < 0) return false;
  String type = path.substring(typeStart, typeEnd);
  String id = path.substring(path.lastIndexOf('/') + 1);

  int song = link.indexOf("?i=", pathEnd);
  if (song < 0) song = link.indexOf("&i=", pathEnd);
  if (type == "album" && song >= 0) {
    int end = song + 3;
    while (isdigit((unsigned char)link[end])) end++;
    type = "song";
    id = link.substring(song + 3, end);
  }

  bool valid;
  if (type == "album" || type == "song") {
    valid = allDigits(id);
  } else if (type == "playlist") {
    valid = id.startsWith("pl.") && id.length() > 3;
    for (int i = 3; i < (int)id.length(); i++) valid = valid && (isalnum((unsigned char)id[i]) || id[i] == '-');
  } else {
    valid = false;
  }
  if (!valid) return false;
  out.service = MusicService::AppleMusic;
  out.type = type;
  out.id = type + ":" + id;
  return true;
}

bool parseShareLink(const String &link, ShareLink &out) {
  return parseAppleMusic(link, out) || parseSpotify(link, out);
}

String describeShareLink(const ShareLink &share) {
  return String(share.service == MusicService::Spotify ? "Spotify " : "Apple Music ") + share.type;
}
