#pragma once
#include <cstdio>
#include <cstdint>
#include <memory>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(FLOWE_TEST_FBP_IO)
inline size_t testFailReadSize = 0;
inline int testFailReadCount = 0;
inline uint64_t testFailReadOffsets[8] = {};
inline unsigned testFailReadOffsetsCount = 0;
#endif
class FsFile {
 std::shared_ptr<FILE> f;
 public:
 FsFile()=default;
 explicit FsFile(FILE* p):f(p,[](FILE* q){if(q)fclose(q);}){}
 explicit operator bool()const{return bool(f);}
 bool seekSet(uint64_t off){return f && !fseeko(f.get(),(off_t)off,SEEK_SET);}
 uint64_t size(){struct stat s{};return f && !fstat(fileno(f.get()),&s)?s.st_size:0;}
 int read(void* p,size_t n){
#if defined(FLOWE_TEST_FBP_IO)
  for (unsigned i=0; f && i<testFailReadOffsetsCount; ++i)
   if ((uint64_t)ftello(f.get()) == testFailReadOffsets[i]) return -1;
  if (n == testFailReadSize && testFailReadCount) {
   if (testFailReadCount > 0) --testFailReadCount;
   return -1;
  }
#endif
  return f?(int)fread(p,1,n,f.get()):-1;
 }
 size_t write(const void* p,size_t n){return f?fwrite(p,1,n,f.get()):0;}
 bool close(){f.reset();return true;}
 bool openNext(FsFile*,int){return false;}
 bool isDir(){return false;}
 int getName(char*,size_t){return 0;}
};
struct TestSD {
 bool ready(){return true;} bool begin(){return true;}
 FsFile open(const char* path,int flags){return FsFile(fopen(path,flags&O_WRONLY?"wb":"rb"));}
 bool exists(const char* path){return access(path,F_OK)==0;}
 bool remove(const char* path){return std::remove(path)==0;}
};
inline TestSD SdMan;
