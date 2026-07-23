#include "HalStorage.h"

#include <Arduino.h>
#include <SDCardManager.h>

namespace {

// The boot path (src/SdUpdate.cpp checkAndApply) already mounts the card, but
// re-arm lazily in case the reader runs on a boot where that mount failed.
bool ensureMounted() {
  if (SdMan.ready()) return true;
  if (SdMan.begin()) return true;
  Serial.printf("[xphone-os] readerfs: SD not mounted\n");
  return false;
}

}  // namespace

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  if (!ensureMounted()) return false;
  return SdMan.openFileForRead(moduleName, path, file.f);  // SDCardManager logs failures
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  if (!ensureMounted()) return false;
  return SdMan.openFileForWrite(moduleName, path, file.f);  // SDCardManager logs failures
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::exists(const char* path) {
  if (!ensureMounted()) return false;
  return SdMan.exists(path);
}

bool HalStorage::remove(const char* path) {
  if (!ensureMounted()) return false;
  return SdMan.remove(path);
}

bool HalStorage::rename(const char* oldPath, const char* newPath) {
  if (!ensureMounted()) return false;
  return SdMan.rename(oldPath, newPath);
}

bool HalStorage::mkdir(const char* path, const bool pFlag) {
  if (!ensureMounted()) return false;
  return SdMan.mkdir(path, pFlag);
}

bool HalStorage::removeDir(const char* path) {
  if (!ensureMounted()) return false;
  return SdMan.removeDir(path);  // recursive delete (SDCardManager.cpp)
}
