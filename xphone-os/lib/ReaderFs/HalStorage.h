#pragma once
// ReaderFs — minimal file-access shim for the vendored x4-os EPUB-engine libs.
//
// Provides the same names and include path (<HalStorage.h>: HalFile, HalStorage,
// the `Storage` singleton macro) that ZipFile and Serialization compile against
// in x4-os, implemented directly over SdFat via the FreeInk SDK's SDCardManager
// (`SdMan`) — the same SD backend src/SdUpdate.cpp already uses. This is NOT a
// port of x4-os/lib/hal/HalStorage.
//
// CONCURRENCY: single-task use only. xphone-os touches the SD card exclusively
// from the Arduino main loop task, so unlike x4-os HalStorage there is NO mutex
// here (SdFat itself is not thread-safe — never call into this shim from a
// second FreeRTOS task).
//
// SCOPE: only the surface the vendored libs and src/reader actually use is
// implemented —
// ZipFile: default ctor, operator bool, seek, seekCur, read, available,
//          position, size, close, Storage.openFileForRead(module, path, file),
//          and Print (readFileToStream writes into a Print&).
// Serialization.h: HalFile::read/write (and openFileForWrite to obtain a
//          writable HalFile).
// src/reader: Storage.exists/remove/rename/mkdir/removeDir, HalFile::flush.
//
// NOTE: xphone-os does not define DESTRUCTOR_CLOSES_FILE (x4-os does), so a raw
// FsFile does NOT auto-close on scope exit there. ~HalFile() closes explicitly,
// preserving the x4-os semantics the vendored code relies on.

#include <Print.h>
#include <SdFat.h>

#include <string>

class HalFile : public Print {
  friend class HalStorage;
  FsFile f;

 public:
  HalFile() = default;
  ~HalFile() override { f.close(); }  // explicit close: DESTRUCTOR_CLOSES_FILE is not set in this build
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool seek(size_t pos) { return f.seekSet(pos); }
  bool seekCur(int64_t offset) { return f.seekCur(offset); }
  int read(void* buf, size_t count) { return f.read(buf, count); }
  size_t write(const void* buf, size_t count) { return f.write(buf, count); }
  size_t write(const uint8_t* buf, size_t count) override { return f.write(buf, count); }
  size_t write(uint8_t b) override { return f.write(&b, 1); }
  void flush() { f.flush(); }
  // FsFile::available() is non-const in SdFat (Stream override); const_cast to
  // keep the const signature the vendored callers expect.
  int available() const { return const_cast<FsFile&>(f).available(); }
  size_t position() const { return static_cast<size_t>(f.curPosition()); }
  size_t size() const { return static_cast<size_t>(f.fileSize()); }
  bool close() { return f.close(); }
  bool isOpen() const { return f.isOpen(); }
  operator bool() const { return f.isOpen(); }
};

class HalStorage {
 public:
  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file);

  // Directory/path ops used by src/reader cache management (Epub, Section,
  // BookMetadataCache, ContentOpfParser, ProgressFile). Same signatures as
  // x4-os HalStorage; thin pass-throughs to SdMan/SdFat.
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool mkdir(const char* path, const bool pFlag = true);
  bool removeDir(const char* path);

  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

 private:
  HalStorage() = default;
};

#define Storage HalStorage::getInstance()
