#pragma once

// xphone-os R2 — File Transfer HTTP server (trimmed port of CrossPoint's
// CrossPointWebServer, x4-os src/network/CrossPointWebServer.cpp).
//
// Serves the same JSON API the Flowe iOS app already speaks:
//   GET  /             tiny plain-text status (sanity check from a browser)
//   GET  /api/status   {"device","version","ip","mode","freeHeap"}
//   GET  /api/files    ?path=/books -> [{"name","size","isDirectory","isEpub"}]
//   GET  /download     ?path=/books/x.epub -> streamed file (4 KB chunks)
//   POST /upload       ?path=/books, multipart field "file" -> SD write
//   POST /delete       ?path=/books/x.epub -> remove file
//
// Deliberately dropped from CrossPoint: HTML pages, WebSockets, WebDAV,
// captive-portal DNS, fonts/settings/OPDS handlers. The only client is the
// phone app, which needs raw JSON + bytes.
//
// Threading: everything runs on the main loop via handleClient() polling
// (FileTransferScene pumps it), so SdFat access stays main-loop-only —
// the same single-task rule as the rest of xphone-os. CrossPoint needed a
// storage mutex only because other FreeRTOS tasks share its SD card.

#include <WebServer.h>

#include <memory>

class FileTransferServer {
 public:
  // Starts listening on port 80. Call only after Wi-Fi is up (STA got an IP
  // or softAP started). Returns false on OOM.
  bool begin();
  void stop();
  bool isRunning() const { return _running; }

  // Pump pending HTTP work; one call handles at most one full request
  // (Arduino WebServer processes a request synchronously inside
  // handleClient, including the whole multipart upload).
  void handleClient();

  // Bytes moved since begin() — for the on-screen activity line.
  uint32_t bytesUploaded() const { return _bytesUploaded; }
  uint32_t bytesDownloaded() const { return _bytesDownloaded; }
  uint16_t requestCount() const { return _requestCount; }

 private:
  struct UploadState {
    // 4 KB batches small multipart chunks into fewer, larger SD writes —
    // CrossPoint measured this as the sweet spot between syscall overhead
    // and per-write watchdog headroom (CrossPointWebServer.h:44).
    static constexpr size_t kBufferSize = 4096;
    uint8_t buffer[kBufferSize];
    size_t bufferPos = 0;
    char path[192] = {0};  // final SD path of the file being written
    bool fileOpen = false;
    bool failed = false;
    size_t received = 0;
  };

  void handleRoot();
  void handleStatus();
  void handleFileList();
  void handleDownload();
  void handleDelete();
  void handleUploadData();  // multipart body callback (START/WRITE/END/ABORT)
  void handleUploadDone();  // final response after body consumed
  bool flushUploadBuffer();

  // Normalized "path" query arg into dst ("/books" default). Returns false
  // (and sends a 400) when missing/invalid.
  bool queryPath(char* dst, size_t dstSize, bool required);

  std::unique_ptr<WebServer> _server;
  UploadState _upload;
  bool _running = false;
  uint32_t _bytesUploaded = 0;
  uint32_t _bytesDownloaded = 0;
  uint16_t _requestCount = 0;

  // The upload FsFile is kept out of the header to avoid dragging SdFat
  // types into every includer; see .cpp (file-scope, single instance).
};
