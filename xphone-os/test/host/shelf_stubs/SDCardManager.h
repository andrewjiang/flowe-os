#pragma once
// Real host files, rooted in the test's temporary directory. Only I/O is
// replaced; the test runs the production shelf index, scan and FBPK reader.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include <fcntl.h>
inline std::string testSdRoot;
inline unsigned testIndexWrites = 0;
inline bool testFailCoverWrite = false;
inline size_t testCoverWriteLimit = SIZE_MAX;
class FsFile {
  std::shared_ptr<FILE> f;
  std::string path;
  std::vector<std::string> children;
  size_t child = 0;
  bool directory = false;
 public:
  FsFile() = default;
  FsFile(const std::string& p, int flags) : path(p) {
    directory = std::filesystem::is_directory(p);
    if (directory) {
      for (const auto& entry : std::filesystem::directory_iterator(p)) children.push_back(entry.path());
    } else {
      const bool cover = p.size() >= 4 && p.substr(p.size() - 4) == ".cov";
      if (flags & O_WRONLY) {
        if (cover && testFailCoverWrite) return;
        if (p.find(".shelfidx.t") != std::string::npos) ++testIndexWrites;
      }
      f = std::shared_ptr<FILE>(fopen(p.c_str(), flags & O_WRONLY ? "wb" : "rb"),
                               [](FILE* p) { if (p) fclose(p); });
    }
  }
  explicit operator bool() const { return directory || bool(f); }
  bool seekSet(uint64_t off) { return f && !fseeko(f.get(), off, SEEK_SET); }
  uint64_t size() const { return f ? std::filesystem::file_size(path) : 0; }
  uint64_t fileSize() const { return size(); }
  int read(void* p, size_t n) { return f ? fread(p, 1, n, f.get()) : -1; }
  size_t write(const void* p, size_t n) {
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".cov") {
      n = std::min(n, testCoverWriteLimit);
      testCoverWriteLimit -= n;
    }
    return f ? fwrite(p, 1, n, f.get()) : 0;
  }
  bool close() { f.reset(); directory = false; return true; }
  bool isDir() const { return directory; }
  bool openNext(FsFile* dir, int flags) {
    if (dir->child >= dir->children.size()) return false;
    *this = FsFile(dir->children[dir->child++], flags);
    return bool(*this);
  }
  int getName(char* out, size_t cap) {
    const std::string name = std::filesystem::path(path).filename();
    return snprintf(out, cap, "%s", name.c_str());
  }
};
struct TestSD {
  bool ready() const { return true; }
  bool begin() const { return true; }
  FsFile open(const char* path, int flags) { return FsFile(testSdRoot + path, flags); }
  bool exists(const char* path) { return std::filesystem::exists(testSdRoot + path); }
  bool remove(const char* path) { return std::remove((testSdRoot + path).c_str()) == 0; }
  bool rename(const char* from, const char* to) {
    return std::rename((testSdRoot + from).c_str(), (testSdRoot + to).c_str()) == 0;
  }
};
inline TestSD SdMan;
