#include "FileTransferServer.h"

#include <MD5Builder.h>

#include <ArduinoJson.h>
#include <BoardConfig.h>
#include <SDCardManager.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include <cstring>
#include <functional>
#include <string>

#include "../reader/ReaderSettings.h"
#include "../reader/ReadingStats.h"
#include "../reader/FbpBook.h"
#include "../scenes/AppScenes.h"  // XPHONE_VERSION

// W3 session token (wifi-experience plan P4): minted by the phone over
// encrypted BLE before each session, required on mutating endpoints.
// /upload stays open — the W4 guest page needs it; a write-only upload is
// the accepted risk, deletes are not. RAM only; a restart clears it.
static char gSessionToken[48] = {0};

void transferSetSessionToken(const char* token) {
  snprintf(gSessionToken, sizeof(gSessionToken), "%s", token ? token : "");
  Serial.printf("[xphone-os] transfer: session token set (len=%u)\n",
                (unsigned)strlen(gSessionToken));
}

namespace {
// Single upload at a time (one phone, one request in flight); keeping the
// FsFile at file scope spares every includer the SdFat headers.
FsFile gUploadFile;

bool hasEpubExtension(const char* name) {
  const size_t len = strlen(name);
  if (len < 5) return false;
  const char* ext = name + len - 5;
  return strcasecmp(ext, ".epub") == 0;
}

bool isHiddenName(const char* name) { return name[0] == '.'; }

bool hasFbpExtension(const char* name) {
  const size_t len = strlen(name);
  if (len < 5) return false;
  return strcasecmp(name + len - 4, ".fbp") == 0;
}

// Honest deletes (0.6 workstream C): every removal of a book under /books is
// recorded here so the phone NEVER re-pushes a book the user deleted. One
// name per line, path relative to /books (subdir prefix kept). Hidden file:
// the shelf scanner skips dotfiles. The app clears it after acknowledging.
constexpr const char* kTombstonePath = "/books/.tombstones";

void appendTombstone(const char* relName) {
  FsFile f = SdMan.open(kTombstonePath, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) {
    Serial.printf("[xphone-os] transfer: tombstone write failed for %s\n", relName);
    return;
  }
  f.write(reinterpret_cast<const uint8_t*>(relName), strlen(relName));
  f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
  f.close();
  Serial.printf("[xphone-os] transfer: tombstoned %s\n", relName);
}

// Reject query paths that could escape or touch system areas. Absolute,
// no "..", no hidden path segments (".crosspoint" etc).
bool isSafePath(const char* path) {
  if (path[0] != '/') return false;
  if (strstr(path, "..") != nullptr) return false;
  for (const char* p = path; *p; p++) {
    if (*p == '/' && *(p + 1) == '.') return false;
  }
  return true;
}
}  // namespace

// True when no token is set (legacy phone) or the request carries it.
bool FileTransferServer::tokenOk() {
  if (gSessionToken[0] == '\0') return true;
  if (!_server->hasHeader("X-Flowe-Token")) return false;
  return _server->header("X-Flowe-Token") == gSessionToken;
}

bool FileTransferServer::begin() {
  _server.reset(new (std::nothrow) WebServer(80));
  if (!_server) {
    Serial.println("[xphone-os] transfer: OOM creating WebServer");
    return false;
  }

  _server->on("/", HTTP_GET, [this] { handleRoot(); });
  _server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  _server->on("/api/files", HTTP_GET, [this] { handleFileList(); });
  _server->on("/api/manifest", HTTP_GET, [this] { handleManifest(); });
  _server->on("/tombstones/clear", HTTP_POST, [this] {
    _requestCount++;
    if (!tokenOk()) { _server->send(403, "text/plain", "Missing session token"); return; }
    SdMan.remove(kTombstonePath);
    _server->send(200, "text/plain", "Cleared");
  });
  _server->on("/download", HTTP_GET, [this] { handleDownload(); });
  _server->on("/delete", HTTP_POST, [this] { handleDelete(); });
  _server->on("/stats", HTTP_GET, [this] {
    // Reading stats snapshot (pages/minutes per day + per book). ~1 KB JSON,
    // built from the resident store — no SD read on the request path.
    _server->send(200, "application/json", reader::ReadingStats::toJson().c_str());
  });
  _server->on("/health", HTTP_GET, [this] {
    // Device memory health for support reports (same numbers as the 60 s
    // serial stats line, reachable without a serial cable). BLE is always
    // down during a transfer session, so there is no "mode" field here.
    const uint32_t heapFree = ESP.getFreeHeap();
    const uint32_t largest = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    const unsigned fragPct = heapFree ? static_cast<unsigned>(100u - (largest * 100u) / heapFree) : 0;
    char json[224];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\",\"device\":\"%s\",\"uptimeMs\":%lu,"
             "\"heapFree\":%lu,\"heapMinFree\":%lu,\"largestBlock\":%lu,\"fragPct\":%u}",
             XPHONE_VERSION, BoardConfig::ACTIVE.name, static_cast<unsigned long>(millis()),
             static_cast<unsigned long>(heapFree), static_cast<unsigned long>(esp_get_minimum_free_heap_size()),
             static_cast<unsigned long>(largest), fragPct);
    _server->send(200, "application/json", json);
  });
  _server->on(
      "/upload", HTTP_POST, [this] { handleUploadDone(); }, [this] { handleUploadData(); });
  // End-of-session from the phone. BLE is torn down for the whole Wi-Fi
  // session (heap: the two stacks don't fit together on the X3), so "stop"
  // must arrive over HTTP. The restart is the session teardown AND what
  // brings BLE back.
  _server->on("/stop", HTTP_POST, [this] {
    if (!tokenOk()) {
      Serial.println("[xphone-os] transfer: stop REJECTED (token mismatch)");
      _server->send(403, "text/plain", "Missing session token");
      return;
    }
    _server->send(200, "text/plain", "Restarting");
    _server->client().flush();
    Serial.println("[xphone-os] transfer: stop via HTTP; restarting");
    Serial.flush();
    delay(150);  // let the response reach the phone
    esp_restart();
  });
  _server->onNotFound([this] { _server->send(404, "text/plain", "Not found"); });

  {
    const char* headerKeys[] = {"X-Flowe-Token"};
    _server->collectHeaders(headerKeys, 1);
  }
  _server->begin();
  _running = true;
  _bytesUploaded = 0;
  _bytesDownloaded = 0;
  _requestCount = 0;
  Serial.printf("[xphone-os] transfer: HTTP server up, free heap %u\n", static_cast<unsigned>(ESP.getFreeHeap()));
  return true;
}

void FileTransferServer::stop() {
  if (gUploadFile) gUploadFile.close();
  if (_server) {
    _server->stop();
    _server.reset();
  }
  _running = false;
}

void FileTransferServer::handleClient() {
  if (_running && _server) _server->handleClient();
}

bool FileTransferServer::queryPath(char* dst, const size_t dstSize, const bool required) {
  if (!_server->hasArg("path")) {
    if (required) {
      _server->send(400, "text/plain", "Missing path");
      return false;
    }
    snprintf(dst, dstSize, "/books");
    return true;
  }
  const String& arg = _server->arg("path");
  if (arg.length() == 0 || arg.length() >= dstSize) {
    _server->send(400, "text/plain", "Invalid path");
    return false;
  }
  if (arg.startsWith("/")) {
    snprintf(dst, dstSize, "%s", arg.c_str());
  } else {
    snprintf(dst, dstSize, "/%s", arg.c_str());
  }
  // Trim trailing slash (not root).
  size_t len = strlen(dst);
  while (len > 1 && dst[len - 1] == '/') dst[--len] = '\0';
  if (!isSafePath(dst)) {
    _server->send(403, "text/plain", "Forbidden path");
    return false;
  }
  return true;
}

// W4 guest page (wifi-experience plan P5): one self-contained mobile page.
// Anyone on the session's network can look and drop a book on; deletes stay
// token-gated (W3). No frameworks, no external assets.
static const char kGuestPage[] PROGMEM = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Flowe bookshelf</title>
<style>
body{font:16px/1.5 Georgia,serif;background:#F2F1EF;color:#2C2C29;margin:0;padding:24px}
h1{font-size:22px;font-weight:600;margin:0 0 4px}
p.sub{color:#3C3C43;opacity:.6;margin:0 0 20px;font-size:14px}
#drop{border:2px dashed #769071;border-radius:12px;padding:28px;text-align:center;color:#5F7A5A;background:#FAF9F7;margin-bottom:20px}
#drop.hot{background:#EAF0E8}
#bar{height:4px;background:#DDD;border-radius:2px;margin:12px 0;display:none}
#fill{height:100%;width:0;background:#769071;border-radius:2px}
ul{list-style:none;padding:0;margin:0}
li{background:#FAF9F7;border:1px solid rgba(0,0,0,.08);border-radius:10px;padding:12px 14px;margin-bottom:8px;display:flex;justify-content:space-between;font-size:15px}
li span.sz{color:#3C3C43;opacity:.55;font-size:13px}
#msg{color:#5F7A5A;font-size:14px;min-height:20px}
input[type=file]{display:none}
label{color:#769071;font-weight:600;cursor:pointer}
</style></head><body>
<h1>Flowe bookshelf</h1>
<p class="sub">Drop an EPUB here and it lands on the device. Use the Flowe app for everything else.</p>
<div id="drop">Drop a book here or <label for="f">choose a file</label>
<input id="f" type="file" accept=".epub" multiple></div>
<div id="bar"><div id="fill"></div></div>
<div id="msg"></div>
<ul id="shelf"></ul>
<script>
function fmt(n){return n>1048576?(n/1048576).toFixed(1)+' MB':Math.round(n/1024)+' KB'}
function load(){fetch('/api/files?path=/books').then(function(r){return r.json()}).then(function(d){
 var ul=document.getElementById('shelf');ul.innerHTML='';
 (Array.isArray(d)?d:(d.files||[])).filter(function(f){return !f.dir&&f.name[0]!=='.'}).forEach(function(f){
  var li=document.createElement('li');
  var n=document.createElement('span');n.textContent=f.name;
  var s=document.createElement('span');s.className='sz';s.textContent=fmt(f.size||0);
  li.appendChild(n);li.appendChild(s);ul.appendChild(li);});});}
function send(files){var i=0;function next(){if(i>=files.length){load();return}
 var f=files[i++];var fd=new FormData();fd.append('file',f,f.name);
 var x=new XMLHttpRequest();x.open('POST','/upload?path=/books');
 document.getElementById('bar').style.display='block';
 x.upload.onprogress=function(e){if(e.lengthComputable)document.getElementById('fill').style.width=(100*e.loaded/e.total)+'%'};
 x.onload=function(){document.getElementById('msg').textContent=x.status<300?f.name+' uploaded':'Upload failed: '+x.responseText;
  document.getElementById('bar').style.display='none';next()};
 x.onerror=function(){document.getElementById('msg').textContent='Upload failed';next()};
 x.send(fd)}next()}
var drop=document.getElementById('drop');
drop.addEventListener('dragover',function(e){e.preventDefault();drop.className='hot'});
drop.addEventListener('dragleave',function(){drop.className=''});
drop.addEventListener('drop',function(e){e.preventDefault();drop.className='';send(e.dataTransfer.files)});
document.getElementById('f').addEventListener('change',function(e){send(e.target.files)});
load();
</script></body></html>)HTML";

void FileTransferServer::handleRoot() {
  _requestCount++;
  // W4: the guest page. A phone-less friend on the same network (or the
  // hotspot) can see the shelf and drop a book on.
  _server->send_P(200, "text/html", kGuestPage);
}

void FileTransferServer::handleStatus() {
  _requestCount++;
  JsonDocument doc;
  {
    extern bool gDeviceIsX3;  // DeviceKind.h; set at boot
    doc["device"] = gDeviceIsX3 ? "X3" : "X4";
  }
  doc["version"] = XPHONE_VERSION;
  doc["gitRev"] = XPHONE_GIT_REV_STR;  // exact build for bug reports
  // In AP (Direct) mode localIP() is the STA side — 0.0.0.0. The phone
  // locks the whole session onto this address, so reporting 0.0.0.0 sent
  // every follow-up request (including /stop) to nowhere. (2026-08-18)
  const bool apMode = (WiFi.getMode() & WIFI_AP) != 0;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["ip"] = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  doc["freeHeap"] = ESP.getFreeHeap();
  char out[256];
  serializeJson(doc, out, sizeof(out));
  _server->send(200, "application/json", out);
}

void FileTransferServer::handleFileList() {
  _requestCount++;
  char path[192];
  if (!queryPath(path, sizeof(path), /*required=*/false)) return;

  FsFile dir = SdMan.open(path, O_RDONLY);
  if (!dir || !dir.isDir()) {
    // An empty shelf, not an error — the phone treats [] as "no books yet"
    // (e.g. /books does not exist until the first upload).
    _server->send(200, "application/json", "[]");
    return;
  }

  // Streamed (chunked) response, same shape as CrossPoint handleFileListData
  // (x4-os CrossPointWebServer.cpp:440-488) so the iOS decoder is shared.
  _server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server->send(200, "application/json", "");
  _server->sendContent("[");

  bool first = true;
  FsFile f;
  JsonDocument doc;
  char name[128];
  char out[256];
  while (f.openNext(&dir, O_RDONLY)) {
    const size_t got = f.getName(name, sizeof(name));
    if (got > 0 && got < sizeof(name) && !isHiddenName(name)) {
      doc.clear();
      doc["name"] = name;
      doc["size"] = f.isDir() ? 0 : static_cast<uint32_t>(f.fileSize());
      doc["isDirectory"] = f.isDir();
      doc["isEpub"] = !f.isDir() && hasEpubExtension(name);
      const size_t written = serializeJson(doc, out, sizeof(out));
      if (written < sizeof(out)) {
        if (!first) _server->sendContent(",");
        first = false;
        _server->sendContent(out);
      }
    }
    f.close();
    yield();
  }
  dir.close();
  _server->sendContent("]");
  _server->sendContent("");  // terminate chunked stream
}

void FileTransferServer::handleDownload() {
  _requestCount++;
  char path[192];
  if (!queryPath(path, sizeof(path), /*required=*/true)) return;

  FsFile file = SdMan.open(path, O_RDONLY);
  if (!file) {
    _server->send(404, "text/plain", "Not found");
    return;
  }
  if (file.isDir()) {
    file.close();
    _server->send(400, "text/plain", "Path is a directory");
    return;
  }

  const char* slash = strrchr(path, '/');
  const char* filename = slash ? slash + 1 : path;
  char disposition[224];
  snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename);

  _server->setContentLength(file.fileSize());
  _server->sendHeader("Content-Disposition", disposition);
  _server->send(200, hasEpubExtension(path) ? "application/epub+zip" : "application/octet-stream", "");

  // 4 KB chunked streaming, CrossPoint handleDownload pattern
  // (x4-os CrossPointWebServer.cpp:548-569). Reuses the upload batch buffer:
  // the HTTP server is synchronous single-client, so an upload body and a
  // download stream can never be in flight together — a second static 4 KB
  // here was pure BSS duplication.
  NetworkClient client = _server->client();
  uint8_t* const buffer = _upload.buffer;
  constexpr size_t kBufSize = UploadState::kBufferSize;
  bool ok = true;
  while (ok && file.available()) {
    const int result = file.read(buffer, kBufSize);
    if (result <= 0) break;
    size_t sent = 0;
    while (sent < static_cast<size_t>(result)) {
      esp_task_wdt_reset();
      const size_t wrote = client.write(buffer + sent, result - sent);
      if (wrote == 0) {
        ok = false;
        break;
      }
      sent += wrote;
      _bytesDownloaded += wrote;
    }
    yield();
  }
  client.clear();
  file.close();
}

// Canonical book key: drop a container extension, keep ASCII letters and
// digits, lowercase. Mirrors ReaderLibraryManager.libraryKey on both apps and
// library_key() in tools/convergence-check.py, so "The_Book_of_Elon" and
// "The Book of Elon" are ONE book to the device too. Both sides of every
// comparison run through this same function, so an accented title still
// pairs with itself even though the device cannot do the apps' NFD fold.
// One canonical-key implementation lives in FbpBook; this is a thin alias so
// the delete-siblings code below reads unchanged.
static void canonicalBookKey(const char* name, char* out, const size_t outSize) {
  reader::FbpBook::canonicalKey(name, out, outSize);
}

// Remove the four sidecars that belong to one book file, if present.
static void removeSidecars(const char* bookPath) {
  static const char* const kSide[] = {".cov", ".str", ".pos", ".bmk"};
  for (size_t i = 0; i < sizeof(kSide) / sizeof(kSide[0]); ++i) {
    char side[224];
    const int n = snprintf(side, sizeof(side), "%s%s", bookPath, kSide[i]);
    if (n > 0 && n < static_cast<int>(sizeof(side)) && SdMan.exists(side)) {
      SdMan.remove(side);
    }
  }
}

// A book is its package AND its source — but a delete must know where the
// book ENDS. The first version of this expanded a delete to every file with
// the same canonical key, and on 2026-08-22 that turned a routine variant
// cleanup into a massacre: one phone deleted the other's spelling of Elon,
// the expansion took BOTH packages and BOTH sources, and the book vanished
// from the device with all four names tombstoned. Nobody asked for that.
//
// The bounded rule:
//   1. Deleting a SOURCE expands to nothing.
//   2. Deleting a PACKAGE always takes its own same-stem source + sidecars.
//   3. It takes same-key sources under OTHER stems only when no other
//      same-key package survives — the orphan-cleanup case.
//   4. It NEVER removes another package. Under-deleting is recoverable
//      with a second tap; over-deleting is a book silently gone.
//
// Returns the number of extra files removed; each is tombstoned so no phone
// pushes half a book back.
int FileTransferServer::removeBookSiblings(const char* path) {
  if (!hasFbpExtension(path)) return 0;  // rule 1

  const char* slash = strrchr(path, '/');
  if (!slash) return 0;
  char dir[192];
  const size_t dirLen = static_cast<size_t>(slash - path);
  if (dirLen == 0 || dirLen >= sizeof(dir)) return 0;
  memcpy(dir, path, dirLen);
  dir[dirLen] = '\0';

  char wanted[96];
  canonicalBookKey(slash + 1, wanted, sizeof(wanted));
  if (wanted[0] == '\0') return 0;

  // Pass 1: does another package with this key survive? (rule 3 gate)
  bool packageSurvives = false;
  {
    FsFile d = SdMan.open(dir, O_RDONLY);
    if (!d || !d.isDir()) return 0;
    FsFile f;
    while (f.openNext(&d, O_RDONLY)) {
      char name[96];
      const int len = f.getName(name, sizeof(name));
      f.close();
      if (len <= 0 || name[0] == '.' || !hasFbpExtension(name)) continue;
      char key[96];
      canonicalBookKey(name, key, sizeof(key));
      if (strcmp(key, wanted) == 0) { packageSurvives = true; break; }
    }
    d.close();
  }

  // The deleted package's own stem, for the same-stem source (rule 2).
  char stem[96];
  {
    const size_t nameLen = strlen(slash + 1);
    const size_t stemLen = nameLen >= 4 ? nameLen - 4 : 0;  // ".fbp"
    if (stemLen == 0 || stemLen >= sizeof(stem)) return 0;
    memcpy(stem, slash + 1, stemLen);
    stem[stemLen] = '\0';
  }

  int removed = 0;
  FsFile d = SdMan.open(dir, O_RDONLY);
  if (!d || !d.isDir()) return 0;
  FsFile f;
  while (f.openNext(&d, O_RDONLY)) {
    char name[96];
    const int len = f.getName(name, sizeof(name));
    f.close();
    if (len <= 0 || name[0] == '.') continue;
    if (hasFbpExtension(name)) continue;  // rule 4: never another package
    const size_t nl = strlen(name);
    const bool isTxt = nl >= 4 && strcasecmp(name + nl - 4, ".txt") == 0;
    if (!hasEpubExtension(name) && !isTxt) continue;

    const char* dot = strrchr(name, '.');
    const size_t nameStemLen = dot ? static_cast<size_t>(dot - name) : 0;
    const bool sameStem = nameStemLen == strlen(stem) &&
                          strncmp(name, stem, nameStemLen) == 0;
    if (!sameStem) {
      if (packageSurvives) continue;  // rule 3
      char key[96];
      canonicalBookKey(name, key, sizeof(key));
      if (strcmp(key, wanted) != 0) continue;
    }

    char full[224];
    const int n = snprintf(full, sizeof(full), "%s/%s", dir, name);
    if (n <= 0 || n >= static_cast<int>(sizeof(full))) continue;
    if (strcmp(full, path) == 0) continue;

    removeSidecars(full);
    if (SdMan.remove(full)) {
      ++removed;
      appendTombstone(full + 7);
      Serial.printf("[xphone-os] transfer: also removed %s (same book)\n", name);
    }
  }
  d.close();
  return removed;
}

void FileTransferServer::handleDelete() {
  _requestCount++;
  if (!tokenOk()) { _server->send(403, "text/plain", "Missing session token"); return; }
  char path[192];
  if (!queryPath(path, sizeof(path), /*required=*/true)) return;

  if (!SdMan.exists(path)) {
    // Echo the decoded path so the phone's error message shows exactly what
    // this server looked for — a 404 here is always a path/name mismatch.
    char msg[224];
    snprintf(msg, sizeof(msg), "Not found: %s", path);
    _server->send(404, "text/plain", msg);
    return;
  }
  const bool isBook = strncmp(path, "/books/", 7) == 0 &&
                      (hasEpubExtension(path) || hasFbpExtension(path));
  if (isBook) removeSidecars(path);
  if (SdMan.remove(path)) {
    if (isBook) {
      appendTombstone(path + 7);
      removeBookSiblings(path);
    }
    _server->send(200, "text/plain", "Deleted");
  } else {
    _server->send(500, "text/plain", "Delete failed");
  }
}

bool FileTransferServer::flushUploadBuffer() {
  if (_upload.bufferPos == 0 || !gUploadFile) return true;
  esp_task_wdt_reset();  // SD writes can be slow (FAT cluster allocation)
  const size_t written = gUploadFile.write(_upload.buffer, _upload.bufferPos);
  const bool ok = written == _upload.bufferPos;
  _upload.bufferPos = 0;
  return ok;
}

void FileTransferServer::handleUploadData() {
  esp_task_wdt_reset();
  const HTTPUpload& up = _server->upload();

  if (up.status == UPLOAD_FILE_START) {
    _upload.bufferPos = 0;
    _upload.failed = false;
    _upload.fileOpen = false;
    _upload.received = 0;

    char dir[128];
    if (!_server->hasArg("path")) {
      snprintf(dir, sizeof(dir), "/books");
    } else {
      const String& arg = _server->arg("path");
      snprintf(dir, sizeof(dir), "%s%s", arg.startsWith("/") ? "" : "/", arg.c_str());
      size_t len = strlen(dir);
      while (len > 1 && dir[len - 1] == '/') dir[--len] = '\0';
    }
    if (!isSafePath(dir) || up.filename.length() == 0 || isHiddenName(up.filename.c_str()) ||
        up.filename.indexOf('/') >= 0) {
      _upload.failed = true;
      return;
    }
    // Names longer than the shelf's path budget store fine on FAT but are
    // invisible to every listing afterwards — the upload "succeeded" and the
    // book never appeared (audit I1, 2026-08-18). Refuse them honestly.
    if (up.filename.length() > 96) {
      Serial.printf("[xphone-os] transfer: name too long (%u chars), refused\n",
                    static_cast<unsigned>(up.filename.length()));
      _upload.failed = true;
      return;
    }
    if (!SdMan.exists(dir) && !SdMan.mkdir(dir)) {
      Serial.printf("[xphone-os] transfer: mkdir %s failed\n", dir);
      _upload.failed = true;
      return;
    }
    snprintf(_upload.path, sizeof(_upload.path), "%s/%s", dir, up.filename.c_str());

    esp_task_wdt_reset();
    if (SdMan.exists(_upload.path)) SdMan.remove(_upload.path);
    gUploadFile = SdMan.open(_upload.path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!gUploadFile) {
      Serial.printf("[xphone-os] transfer: create %s failed\n", _upload.path);
      _upload.failed = true;
      return;
    }
    _upload.fileOpen = true;
    Serial.printf("[xphone-os] transfer: upload start %s\n", _upload.path);
    _hookMark = _bytesUploaded;
    if (progressHook) progressHook();

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (_upload.failed || !_upload.fileOpen) return;
    const uint8_t* data = up.buf;
    size_t remaining = up.currentSize;
    while (remaining > 0) {
      const size_t space = UploadState::kBufferSize - _upload.bufferPos;
      const size_t toCopy = remaining < space ? remaining : space;
      memcpy(_upload.buffer + _upload.bufferPos, data, toCopy);
      _upload.bufferPos += toCopy;
      data += toCopy;
      remaining -= toCopy;
      if (_upload.bufferPos >= UploadState::kBufferSize && !flushUploadBuffer()) {
        _upload.failed = true;
        gUploadFile.close();
        _upload.fileOpen = false;
        return;
      }
    }
    _upload.received += up.currentSize;
    _bytesUploaded += up.currentSize;
    // ~4 MB cadence: at bench speed (~165 KB/s) that is one e-ink repaint
    // every ~25 s — visible progress for ~3% throughput cost.
    if (progressHook && _bytesUploaded - _hookMark >= 4u * 1024u * 1024u) {
      _hookMark = _bytesUploaded;
      progressHook();
    }

  } else if (up.status == UPLOAD_FILE_END) {
    if (_upload.fileOpen) {
      if (!flushUploadBuffer()) _upload.failed = true;
      gUploadFile.close();
      _upload.fileOpen = false;
    }
    if (!_upload.failed) {
      Serial.printf("[xphone-os] transfer: upload done %s (%u bytes)\n", _upload.path,
                    static_cast<unsigned>(_upload.received));
    }

  } else if (up.status == UPLOAD_FILE_ABORTED) {
    _upload.bufferPos = 0;
    if (_upload.fileOpen) {
      gUploadFile.close();
      _upload.fileOpen = false;
      SdMan.remove(_upload.path);  // drop the partial file
    }
    _upload.failed = true;
    Serial.println("[xphone-os] transfer: upload aborted");
  }
}

// The reader's place in a raw epub survives the upgrade to a package. The
// epub's cache holds {spine, page, section pages} (progress.bin) and the
// spine count (book.bin header); both are tiny reads, safe in the
// transfer-mode heap. The fraction lands as a .pos in a neutral
// 10,000-page pagination — every consumer (firmware loadPos, phone
// harvest) rescales via the trailer. Chapter lengths vary, so this is a
// same-chapter landing, not a same-word one; it always rounds down so
// furthest-wins can never jump a reader forward. If book.bin's version
// moves past v8, the handoff quietly stops until this reader learns the
// new header.
static void handoffEpubPosition(const char* epubPath, const char* fbpPath) {
  char posPath[196];
  snprintf(posPath, sizeof(posPath), "%s.pos", fbpPath);
  if (SdMan.exists(posPath)) return;  // a real position always wins

  const std::string cache = std::string(reader::kReaderCacheRoot) + "/epub_" +
                            std::to_string(std::hash<std::string>{}(std::string(epubPath)));
  FsFile pf = SdMan.open((cache + "/progress.bin").c_str(), O_RDONLY);
  if (!pf) return;
  uint8_t d[6] = {0};
  const int n = pf.read(d, sizeof(d));
  pf.close();
  if (n != 4 && n != 6) return;
  const uint16_t spine = (uint16_t)(d[0] | (d[1] << 8));
  const uint16_t page = (uint16_t)(d[2] | (d[3] << 8));
  const uint16_t secPages = (n == 6) ? (uint16_t)(d[4] | (d[5] << 8)) : 0;

  FsFile bf = SdMan.open((cache + "/book.bin").c_str(), O_RDONLY);
  if (!bf) return;
  uint8_t hdr[7] = {0};
  const int hn = bf.read(hdr, sizeof(hdr));
  bf.close();
  if (hn != 7 || hdr[0] != 8) return;  // book.bin v8 header only
  const uint16_t spineCount = (uint16_t)(hdr[5] | (hdr[6] << 8));
  if (spineCount == 0 || spine >= spineCount) return;

  float frac = (float)spine / (float)spineCount;
  if (secPages > 0 && page < secPages)
    frac += ((float)page / (float)secPages) / (float)spineCount;
  if (frac <= 0.0f || frac >= 1.0f) return;

  uint32_t p32 = (uint32_t)(frac * 10000.0f);
  const uint32_t c32 = 10000;
  if (p32 == 0) return;
  FsFile out = SdMan.open(posPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!out) return;
  out.write(&p32, 4);
  out.write(&c32, 4);
  out.close();
  Serial.printf("[xphone-os] transfer: epub position handed off (%lu/10000) -> %s\n",
                (unsigned long)p32, posPath);
}

void FileTransferServer::handleUploadDone() {
  _requestCount++;
  if (_upload.failed) {
    _server->send(400, "text/plain", "Upload failed");
    return;
  }
  // An optimized package takes over from its raw sideloaded epub: same
  // directory, same stem. The reading position moves across, and the
  // shelf shows one book, not two.
  //
  // 2026-08-20: the epub is now KEPT, where it used to be deleted and
  // tombstoned. Andrew: "we should make sure we never remove the epub
  // files then from the SD cards right?" He is right, and the cost is
  // small — a source is about 3% of its package (Karamazov 1.2 MB
  // against 32.6 MB, and against 8.9 MB now that packages are v4).
  //
  // What keeping it buys: ANY phone can rebuild ANY book, instead of
  // only the phone that happens to hold the original. That is the real
  // root of the two-phone problem, and it is also how a colour cover
  // reaches a phone that never imported the book. The card becomes the
  // master library rather than a cache of one phone's.
  //
  // The shelf hides an epub whose .fbp sits beside it (ReaderScene
  // scanDir), so there is still no double Sherlock.
  if (hasFbpExtension(_upload.path)) {
    char sibling[sizeof(_upload.path)];
    snprintf(sibling, sizeof(sibling), "%s", _upload.path);
    char* dot = strrchr(sibling, '.');
    const size_t room = sizeof(sibling) - (dot - sibling);
    snprintf(dot, room, ".epub");
    if (SdMan.exists(sibling)) {
      handoffEpubPosition(sibling, _upload.path);
      Serial.printf("[xphone-os] transfer: package took over from %s (source kept)\n", sibling);
    }
  }
  _server->send(200, "text/plain", "File uploaded");
}

// 0.6 workstream C — the sync manifest. One GET answers "what books does
// the device hold, exactly": path relative to /books (one subdir level,
// matching the shelf scanner), byte size, and the MD5 of the first 64 KB
// (cheap identity; whole-file hashing would take minutes on big cards).
// Tombstones ride along so the app learns about deletes in the same call.
// A book named  The "Good" Parts.epub  broke the whole manifest into
// unparseable JSON, so sync silently moved nothing. (Release audit,
// 2026-08-18.) The shelf path already escapes via ArduinoJson; this is the
// same job for the streaming writer.
static void appendEscaped(String& out, const char* s) {
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') out += '\\';
    out += *s;
  }
}

void FileTransferServer::handleManifest() {
  _requestCount++;
  _server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server->send(200, "application/json", "");
  _server->sendContent("{\"books\":[");

  bool first = true;
  char rel[192];
  const auto emitDir = [&](const char* dir, const char* prefix) {
    FsFile d = SdMan.open(dir, O_RDONLY);
    if (!d || !d.isDir()) return;
    FsFile f;
    while (f.openNext(&d, O_RDONLY)) {
      esp_task_wdt_reset();
      char name[128];
      const int len = f.getName(name, sizeof(name));
      if (len <= 0 || len >= static_cast<int>(sizeof(name)) - 1 || name[0] == '.' || f.isDir()) {
        f.close();
        continue;
      }
      if (!hasEpubExtension(name) && !hasFbpExtension(name)) {
        f.close();
        continue;
      }
      const uint32_t size = f.fileSize();
      // First-64KB MD5 through the already-open handle.
      MD5Builder md5;
      md5.begin();
      uint8_t buf[1024];
      uint32_t left = 65536;
      while (left > 0) {
        const int n = f.read(buf, left < sizeof(buf) ? left : sizeof(buf));
        if (n <= 0) break;
        md5.add(buf, static_cast<uint16_t>(n));
        left -= static_cast<uint32_t>(n);
      }
      md5.calculate();
      f.close();
      snprintf(rel, sizeof(rel), "%s%s", prefix, name);
      String row = first ? "{\"name\":\"" : ",{\"name\":\"";
      appendEscaped(row, rel);
      char tail[96];
      snprintf(tail, sizeof(tail), "\",\"size\":%lu,\"md5h\":\"%s\"}",
               static_cast<unsigned long>(size), md5.toString().c_str());
      row += tail;
      _server->sendContent(row);
      first = false;
    }
    d.close();
  };

  emitDir("/books", "");
  // One subdir level, same rule as the shelf.
  {
    FsFile d = SdMan.open("/books", O_RDONLY);
    if (d && d.isDir()) {
      FsFile f;
      while (f.openNext(&d, O_RDONLY)) {
        char name[128];
        const int len = f.getName(name, sizeof(name));
        const bool usable = len > 0 && len < static_cast<int>(sizeof(name)) - 1 && name[0] != '.' && f.isDir();
        f.close();
        if (!usable) continue;
        char sub[160], prefix[144];
        if (snprintf(sub, sizeof(sub), "/books/%s", name) >= static_cast<int>(sizeof(sub))) continue;
        snprintf(prefix, sizeof(prefix), "%s/", name);
        emitDir(sub, prefix);
      }
      d.close();
    }
  }

  _server->sendContent("],\"tombstones\":[");
  first = true;
  {
    FsFile t = SdMan.open(kTombstonePath, O_RDONLY);
    if (t) {
      char line[192];
      size_t pos = 0;
      int c;
      while ((c = t.read()) >= 0) {
        if (c == '\n' || pos >= sizeof(line) - 1) {
          line[pos] = 0;
          if (pos > 0) {
            String row = first ? "\"" : ",\"";
            appendEscaped(row, line);
            row += '"';
            _server->sendContent(row);
            first = false;
          }
          pos = 0;
        } else {
          line[pos++] = static_cast<char>(c);
        }
      }
      t.close();
    }
  }
  _server->sendContent("]}");
  _server->sendContent("");  // terminate chunked response
}
