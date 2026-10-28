// Requests are handled from loop(), on the same task as the IR remote, so the slots need no locking.
//
//   GET  /                 the page
//   GET  /api/slots        used slots as JSON (see slotsExport()); ?download=1 saves it as a file
//   POST /api/slots        import: replaces every slot with the JSON body
//   POST /api/slot         saves one slot from form fields n, label, link and metadata (see Slot); an
//                          empty link clears it
//   POST /api/move?from=6&to=12  moves a slot, swapping the two if the other one is used
//   POST /api/play?n=12    plays a slot
//   GET  /api/favorites    the household's Sonos Favorites: {"items": [{title, link, metadata, kind}]}
//
// Changes answer with the new slot list, errors with a plain-text message.

#include "web.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <WebServer.h>

#include <vector>

#include "config.h"
#include "share_link.h"
#include "slots.h"
#include "sonos.h"

static WebServer server(80);

static const char PAGE[] = R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{{ROOM}} - Sonos Display</title>
<style>
:root{--bg:#f5f5f3;--card:#fff;--text:#1c1c1e;--muted:#6c6c70;--line:#e2e2df;--field:#fafaf9;--accent:#0b63ce;--err:#c4291c;--ok:#23803a}
@media (prefers-color-scheme:dark){:root{--bg:#141415;--card:#1d1d1f;--text:#f2f2f3;--muted:#9b9ba1;--line:#323235;--field:#252527;--accent:#4b9bff;--err:#ff6b5e;--ok:#5ac46f}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.4 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:1080px;margin:0 auto;padding:20px 16px 40px}
h1{font-size:22px;margin:0 0 4px}
.sub{color:var(--muted);margin:0 0 16px}
.bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:12px}
.bar label{display:flex;gap:6px;align-items:center;color:var(--muted)}
.bar .grow{flex:1}
#note{color:var(--muted);margin:0 0 12px}
#note:empty{display:none}
#note.err{color:var(--err)}
button,.btn{font:inherit;font-size:14px;padding:6px 12px;border:1px solid var(--line);border-radius:8px;background:var(--card);color:var(--text);cursor:pointer;text-decoration:none;white-space:nowrap}
button:hover:not(:disabled),.btn:hover{border-color:var(--muted)}
button:disabled{opacity:.35;cursor:default}
button.save{background:var(--accent);border-color:var(--accent);color:#fff}
#list{background:var(--card);border:1px solid var(--line);border-radius:12px}
.row{display:grid;grid-template-columns:40px minmax(0,1fr) minmax(0,1.6fr) 150px auto;gap:8px;align-items:center;padding:8px 12px;border-top:1px solid var(--line)}
.row:first-child{border-top:0}
button.num{font:600 16px ui-monospace,SFMono-Regular,Menlo,monospace;color:var(--muted);border:0;background:none;padding:0;text-align:left}
button.num:disabled{opacity:1}
.used .num{color:var(--accent)}
.used .num:hover{text-decoration:underline}
input[type=text]{width:100%;font:inherit;padding:6px 8px;border:1px solid var(--line);border-radius:8px;background:var(--field);color:var(--text)}
input[type=text]:focus{outline:2px solid var(--accent);outline-offset:-1px}
.kind{font-size:13px;color:var(--muted);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.kind.ok{color:var(--ok)}
.kind.err{color:var(--err);white-space:normal}
.acts,.linkbox{display:flex;gap:6px}
.only-used .row:not(.used){display:none}
dialog{border:1px solid var(--line);border-radius:12px;background:var(--card);color:var(--text);padding:0;width:min(560px,calc(100vw - 32px));max-height:min(640px,calc(100vh - 32px))}
dialog::backdrop{background:rgba(0,0,0,.45)}
dialog[open]{display:flex;flex-direction:column}
dialog header{display:flex;align-items:center;padding:12px 14px;border-bottom:1px solid var(--line)}
dialog h2{font-size:16px;margin:0;flex:1}
#favSearch{margin:10px 14px 6px;width:calc(100% - 28px)}
#favList{overflow:auto;padding-bottom:8px}
.fav{display:flex;gap:12px;justify-content:space-between;width:100%;text-align:left;border:0;border-radius:0;background:none;padding:9px 14px;white-space:normal}
.fav:hover,.fav:focus{background:var(--field);outline:none}
.fav span:last-child{color:var(--muted);font-size:13px;white-space:nowrap}
#favList p{margin:0;padding:12px 14px;color:var(--muted)}
#moveGrid{display:grid;grid-template-columns:repeat(10,minmax(0,1fr));gap:6px;padding:14px 14px 8px}
#moveGrid button{padding:6px 0;font:600 13px ui-monospace,SFMono-Regular,Menlo,monospace}
#moveGrid button.used{background:var(--field);color:var(--muted);border-style:dashed}
#moveHint{margin:0;padding:0 14px 14px;color:var(--muted);font-size:13px}
@media (max-width:720px){.row{grid-template-columns:32px minmax(0,1fr)}.row>:nth-child(n+3){grid-column:2}}
</style>
</head>
<body>
<main>
<h1>Remote slots</h1>
<p class="sub">Type two digits on the remote (00 to 99) to play a slot in {{ROOM}}. Each slot takes a Spotify or
Apple Music album, playlist or track link (Share &gt; Copy link in their apps), or one of your Sonos Favorites
(&#9733;), stations included. Click a slot's number to move it.</p>
<div class="bar">
<label><input type="checkbox" id="onlyUsed"> Only used slots</label>
<span class="grow"></span>
<a class="btn" href="/api/slots?download=1">Export</a>
<button id="importBtn">Import&hellip;</button>
<input type="file" id="importFile" accept=".json,application/json" hidden>
</div>
<p id="note"></p>
<div id="list"></div>
</main>
<dialog id="picker">
<header><h2>Sonos Favorites</h2><button id="favClose" aria-label="Close">&times;</button></header>
<input type="text" id="favSearch" placeholder="Search" aria-label="Search favorites">
<div id="favList"></div>
</dialog>
<dialog id="mover">
<header><h2 id="moveTitle">Move</h2><button id="moveClose" aria-label="Close">&times;</button></header>
<div id="moveGrid"></div>
<p id="moveHint">Pick a free slot (solid), or a used one (dashed) to swap the two.</p>
</dialog>
<script>
const $ = (sel, el = document) => el.querySelector(sel);
const list = $("#list"), note = $("#note"), onlyUsed = $("#onlyUsed");
const picker = $("#picker"), favList = $("#favList"), favSearch = $("#favSearch");
const mover = $("#mover"), moveGrid = $("#moveGrid");
const pad = n => String(n).padStart(2, "0");
let saved = {};          // Slot number -> {label, link, metadata, kind}, as saved on the board.
const metadata = {};     // Slot number -> Sonos metadata for the row's link, for items from Sonos.
let favorites = null, pickFor = -1, moveFrom = -1;

function setNote(text, err) { note.textContent = text; note.className = err ? "err" : ""; }
function setStatus(n, text, cls) {
  const k = $("#r" + n + " .kind");
  k.textContent = k.title = text;
  k.className = "kind" + (cls ? " " + cls : "");
}

async function call(url, opts) {
  const res = await fetch(url, opts);
  const text = await res.text();
  if (!res.ok) throw new Error(text || res.statusText);
  return text;
}

// Takes the board's slot list and refreshes the given rows, or all of them.
function show(json, rows) {
  saved = {};
  for (const s of JSON.parse(json).slots) saved[s.n] = s;
  for (const n of rows || [...Array(100).keys()]) {
    const row = $("#r" + n), s = saved[n] || {label: "", link: "", kind: ""};
    $(".label", row).value = s.label;
    $(".link", row).value = s.link;
    metadata[n] = s.metadata || "";
    row.classList.toggle("used", !!s.link);
    $(".save", row).disabled = true;
    $(".num", row).disabled = $(".play", row).disabled = $(".clear", row).disabled = !s.link;
    setStatus(n, s.kind);
  }
}

// Enables Save while a row differs from what's saved.
function edited(n) {
  const row = $("#r" + n), s = saved[n] || {};
  const label = $(".label", row).value, link = $(".link", row).value;
  $(".save", row).disabled = label === (s.label || "") && link === (s.link || "") && metadata[n] === (s.metadata || "");
  $(".play", row).disabled = !link.trim();
}

async function save(n) {
  const row = $("#r" + n);
  const body = new URLSearchParams({n, label: $(".label", row).value, link: $(".link", row).value, metadata: metadata[n]});
  try {
    show(await call("/api/slot", {method: "POST", body}), [n]);
    setStatus(n, saved[n] ? "Saved: " + saved[n].kind : "Cleared", "ok");
    return true;
  } catch (e) {
    setStatus(n, e.message, "err");
    return false;
  }
}

async function play(n) {
  if (!$("#r" + n + " .save").disabled && !await save(n)) return;
  try {
    setStatus(n, "Sent " + await call("/api/play?n=" + n, {method: "POST"}), "ok");
  } catch (e) {
    setStatus(n, e.message, "err");
  }
}

function clear(n) {
  if (!confirm("Clear slot " + pad(n) + "?")) return;
  const row = $("#r" + n);
  $(".label", row).value = $(".link", row).value = metadata[n] = "";
  save(n);
}

async function openPicker(n) {
  pickFor = n;
  favSearch.value = "";
  picker.showModal();
  if (!favorites) {
    showFavorites("Loading favorites from Sonos\u2026");
    try {
      favorites = JSON.parse(await call("/api/favorites")).items;
    } catch (e) {
      return showFavorites(e.message);
    }
  }
  showFavorites();
}

// Lists the favorites matching the search, or shows a message instead.
function showFavorites(message) {
  const q = favSearch.value.trim().toLowerCase();
  const shown = message ? [] : favorites.filter(f => (f.title + " " + f.kind).toLowerCase().includes(q));
  favList.replaceChildren();
  for (const f of shown) {
    const b = document.createElement("button"), title = document.createElement("span"), kind = document.createElement("span");
    b.className = "fav";
    title.textContent = f.title;
    kind.textContent = f.kind;
    b.append(title, kind);
    b.onclick = () => pick(f);
    favList.append(b);
  }
  if (!shown.length) {
    const p = document.createElement("p");
    p.textContent = message || (favorites.length ? "No matches" : "No favorites that can be played");
    favList.append(p);
  }
}

async function openMover(n) {
  if (!$("#r" + n + " .save").disabled && !await save(n)) return;
  if (!saved[n]) return;
  moveFrom = n;
  $("#moveTitle").textContent = "Move " + pad(n) + ": " + (saved[n].label || saved[n].kind);
  [...moveGrid.children].forEach((b, i) => {
    const s = saved[i];
    b.disabled = i === n;
    b.className = s ? "used" : "";
    b.title = i === n ? "" : s ? "Swap with " + pad(i) + ": " + (s.label || s.kind) : "Move to " + pad(i);
  });
  mover.showModal();
}

async function move(to) {
  const from = moveFrom;
  mover.close();
  try {
    show(await call("/api/move?from=" + from + "&to=" + to, {method: "POST"}), [from, to]);
    setStatus(to, "Moved from " + pad(from), "ok");
    setStatus(from, saved[from] ? "Swapped with " + pad(to) : "Moved to " + pad(to), "ok");
    $("#r" + to).scrollIntoView({block: "center", behavior: "smooth"});
  } catch (e) {
    setStatus(from, e.message, "err");
  }
}

function pick(f) {
  const row = $("#r" + pickFor);
  $(".label", row).value = f.title;
  $(".link", row).value = f.link;
  metadata[pickFor] = f.metadata || "";
  edited(pickFor);
  setStatus(pickFor, f.kind + ", not saved yet");
  picker.close();
}

// Fills in an empty label with the album, playlist or song name from Spotify or Apple.
async function autoLabel(n) {
  const row = $("#r" + n), label = $(".label", row), link = $(".link", row).value.trim();
  if (label.value.trim() || !link) return;
  let title = "";
  try {
    if (link.includes("spotify")) {
      const res = await fetch("https://open.spotify.com/oembed?url=" + encodeURIComponent(link));
      if (res.ok) title = (await res.json()).title || "";
    } else {
      const m = link.match(/music\.apple\.com\/(\w+)\/(album|song)\/(?:[^\/?#]+\/)?(\d+)/);
      const song = link.match(/[?&]i=(\d+)/);
      if (m) {
        const res = await fetch("https://itunes.apple.com/lookup?id=" + (song ? song[1] : m[3]) + "&country=" + m[1]);
        const item = res.ok && (await res.json()).results[0];
        if (item) title = item.wrapperType === "track" ? item.trackName : item.collectionName;
      }
    }
  } catch (e) {}  // Labels are optional; the lookup is a convenience.
  if (title && !label.value.trim()) {
    label.value = title;
    label.dispatchEvent(new Event("input"));
  }
}

for (let n = 0; n < 100; n++) {
  const row = document.createElement("div");
  row.className = "row";
  row.id = "r" + n;
  row.innerHTML = '<button class="num" disabled title="Move slot ' + pad(n) + '">' + pad(n) + '</button>' +
    '<input type="text" class="label" placeholder="Label" maxlength="80" aria-label="Label for slot ' + pad(n) + '">' +
    '<span class="linkbox"><input type="text" class="link" placeholder="Spotify or Apple Music link" aria-label="Link for slot ' +
    pad(n) + '"><button class="pick" title="Choose from Sonos Favorites" aria-label="Choose from Sonos Favorites for slot ' +
    pad(n) + '">&#9733;</button></span>' +
    '<span class="kind"></span><span class="acts"><button class="save" disabled>Save</button>' +
    '<button class="play" disabled>Play</button><button class="clear" disabled>Clear</button></span>';
  list.append(row);
  const label = $(".label", row), link = $(".link", row);
  label.oninput = () => edited(n);
  link.oninput = () => {
    metadata[n] = "";  // A typed link is a share link.
    edited(n);
  };
  link.onchange = () => autoLabel(n);
  $(".pick", row).onclick = () => openPicker(n);
  $(".num", row).onclick = () => openMover(n);
  row.onkeydown = e => { if (e.key === "Enter" && e.target.tagName === "INPUT") save(n); };
  $(".save", row).onclick = () => save(n);
  $(".play", row).onclick = () => play(n);
  $(".clear", row).onclick = () => clear(n);
}

onlyUsed.onchange = () => {
  list.classList.toggle("only-used", onlyUsed.checked);
  try { localStorage.onlyUsed = onlyUsed.checked ? "1" : ""; } catch (e) {}
};
try { onlyUsed.checked = localStorage.onlyUsed === "1"; } catch (e) {}
onlyUsed.onchange();

for (let n = 0; n < 100; n++) {
  const b = document.createElement("button");
  b.textContent = pad(n);
  b.onclick = () => move(n);
  moveGrid.append(b);
}
$("#moveClose").onclick = () => mover.close();

favSearch.oninput = () => showFavorites();
favSearch.onkeydown = e => { if (e.key === "Enter" && $(".fav", favList)) $(".fav", favList).click(); };
$("#favClose").onclick = () => picker.close();

$("#importBtn").onclick = () => $("#importFile").click();
$("#importFile").onchange = async e => {
  const file = e.target.files[0];
  e.target.value = "";
  if (!file || !confirm("Replace all slots with the ones in " + file.name + "?")) return;
  try {
    const body = await file.text();
    show(await call("/api/slots", {method: "POST", headers: {"Content-Type": "application/json"}, body}));
    setNote("Imported " + Object.keys(saved).length + " slots from " + file.name);
  } catch (err) {
    setNote("Import failed: " + err.message, true);
  }
};

call("/api/slots").then(json => show(json)).catch(e => setNote("Couldn't load the slots: " + e.message, true));
</script>
</body>
</html>
)html";

static String htmlEscape(const char *s) {
  String out;
  for (; *s; s++) {
    if (*s == '&') out += "&amp;";
    else if (*s == '<') out += "&lt;";
    else if (*s == '>') out += "&gt;";
    else if (*s == '"') out += "&quot;";
    else out += *s;
  }
  return out;
}

// An argument as a slot number, or -1.
static int slotArg(const char *name = "n") {
  String s = server.arg(name);
  if (s.isEmpty() || s.length() > 2) return -1;
  for (char c : s)
    if (!isdigit((unsigned char)c)) return -1;
  return s.toInt();
}

static void sendSlotsOr(bool ok, const String &error) {
  if (ok) server.send(200, "application/json", slotsExport());
  else server.send(400, "text/plain", error);
}

void webBegin() {
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
  else Serial.println("mDNS failed to start; use the board's IP address");

  server.on("/", HTTP_GET, [] {
    String page = PAGE;
    page.replace("{{ROOM}}", htmlEscape(ROOM_NAME));
    server.send(200, "text/html", page);
  });
  server.on("/api/slots", HTTP_GET, [] {
    if (server.hasArg("download"))
      server.sendHeader("Content-Disposition", "attachment; filename=\"sonos-slots.json\"");
    server.send(200, "application/json", slotsExport());
  });
  server.on("/api/slots", HTTP_POST, [] {
    String error;
    sendSlotsOr(slotsImport(server.arg("plain"), error), error);
  });
  server.on("/api/slot", HTTP_POST, [] {
    String error;
    sendSlotsOr(slotsSet(slotArg(), {server.arg("label"), server.arg("link"), server.arg("metadata")}, error), error);
  });
  server.on("/api/move", HTTP_POST, [] {
    String error;
    sendSlotsOr(slotsMove(slotArg("from"), slotArg("to"), error), error);
  });
  server.on("/api/play", HTTP_POST, [] {
    String name;
    int n = slotArg();
    if (n < 0) return server.send(400, "text/plain", "Slot numbers go from 0 to 99");
    bool ok = slotsPlay(n, name);
    server.send(ok ? 200 : 404, "text/plain", name);
  });
  server.on("/api/favorites", HTTP_GET, [] {
    std::vector<SonosItem> items;
    if (!sonosFavorites(items)) return server.send(503, "text/plain", "Couldn't get the favorites from Sonos");
    JsonDocument doc;
    JsonArray list = doc["items"].to<JsonArray>();
    for (const SonosItem &item : items) {
      JsonObject o = list.add<JsonObject>();
      ShareLink share;
      o["title"] = item.title;
      o["link"] = item.link;
      if (item.metadata.length()) o["metadata"] = item.metadata;
      o["kind"] = item.metadata.length() ? sonosDescribeItem(item.metadata)
                  : parseShareLink(item.link, share) ? describeShareLink(share) : String();
    }
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });
  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void webPoll() { server.handleClient(); }
