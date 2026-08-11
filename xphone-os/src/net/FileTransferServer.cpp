#include "FileTransferServer.h"

#include <ArduinoJson.h>
#include <SDCardManager.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include <cstring>

#include "../reader/ReadingStats.h"
#include "../scenes/AppScenes.h"  // XPHONE_VERSION

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

bool FileTransferServer::begin() {
  _server.reset(new (std::nothrow) WebServer(80));
  if (!_server) {
    Serial.println("[xphone-os] transfer: OOM creating WebServer");
    return false;
  }

  _server->on("/", HTTP_GET, [this] { handleRoot(); });
  _server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  _server->on("/api/files", HTTP_GET, [this] { handleFileList(); });
  _server->on("/download", HTTP_GET, [this] { handleDownload(); });
  _server->on("/delete", HTTP_POST, [this] { handleDelete(); });
  _server->on("/stats", HTTP_GET, [this] {
    // Reading stats snapshot (pages/minutes per day + per book). ~1 KB JSON,
    // built from the resident store — no SD read on the request path.
    _server->send(200, "application/json", reader::ReadingStats::toJson().c_str());
  });
  _server->on(
      "/upload", HTTP_POST, [this] { handleUploadDone(); }, [this] { handleUploadData(); });
  // End-of-session from the phone. BLE is torn down for the whole Wi-Fi
  // session (heap: the two stacks don't fit together on the X3), so "stop"
  // must arrive over HTTP. The restart is the session teardown AND what
  // brings BLE back.
  _server->on("/stop", HTTP_POST, [this] {
    _server->send(200, "text/plain", "Restarting");
    _server->client().flush();
    Serial.println("[xphone-os] transfer: stop via HTTP; restarting");
    Serial.flush();
    delay(150);  // let the response reach the phone
    esp_restart();
  });
  _server->onNotFound([this] { _server->send(404, "text/plain", "Not found"); });

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

void FileTransferServer::handleRoot() {
  _requestCount++;
  char body[160];
  snprintf(body, sizeof(body),
           "xphone-os file transfer\nversion: %s\nip: %s\nUse the Flowe app to sync books.\n", XPHONE_VERSION,
           WiFi.localIP().toString().c_str());
  _server->send(200, "text/plain", body);
}

void FileTransferServer::handleStatus() {
  _requestCount++;
  JsonDocument doc;
#if FREEINK_DEVICE_X4
  doc["device"] = "X4";
#else
  doc["device"] = "X3";
#endif
  doc["version"] = XPHONE_VERSION;
  doc["mode"] = "STA";
  doc["ip"] = WiFi.localIP().toString();
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

  // 4 KB stack buffer streaming, CrossPoint handleDownload pattern
  // (x4-os CrossPointWebServer.cpp:548-569).
  NetworkClient client = _server->client();
  static uint8_t buffer[4096];  // static: keeps the loop-task stack small
  bool ok = true;
  while (ok && file.available()) {
    const int result = file.read(buffer, sizeof(buffer));
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

void FileTransferServer::handleDelete() {
  _requestCount++;
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
  if (SdMan.remove(path)) {
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

void FileTransferServer::handleUploadDone() {
  _requestCount++;
  if (_upload.failed) {
    _server->send(400, "text/plain", "Upload failed");
  } else {
    _server->send(200, "text/plain", "File uploaded");
  }
}
