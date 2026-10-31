#pragma once
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <stdint.h>

namespace fs {
class File {
 public:
  File() = default;
  explicit File(std::string* data) : data_(data) {}
  explicit operator bool() const { return data_ != nullptr; }
  size_t size() const { return data_->size(); }
  bool isDirectory() const { return false; }
  int read() { return pos_ < data_->size() ? static_cast<unsigned char>((*data_)[pos_++]) : -1; }
  size_t readBytes(char* dst, size_t n) {
    n = std::min(n, data_->size() - pos_);
    memcpy(dst, data_->data() + pos_, n);
    pos_ += n;
    return n;
  }
  size_t write(const uint8_t* src, size_t n) {
    data_->append(reinterpret_cast<const char*>(src), n);
    return n;
  }
  void flush() {}
  int getWriteError() const { return 0; }
  void close() {}
 private:
  std::string* data_ = nullptr;
  size_t pos_ = 0;
};
}

struct FakeLittleFS {
  std::map<std::string, std::string> files;
  bool directory = false;
  bool failRename = false;
  unsigned writes = 0;
  bool exists(const char*) const { return directory; }
  bool mkdir(const char*) { directory = true; return true; }
  fs::File open(const char* name, const char* mode) {
    if (*mode == 'w') { ++writes; return fs::File(&(files[name] = "")); }
    auto it = files.find(name);
    return it == files.end() ? fs::File() : fs::File(&it->second);
  }
  bool rename(const char* from, const char* to) {
    if (failRename || !files.count(from)) return false;
    files[to] = files[from];
    files.erase(from);
    return true;
  }
  bool remove(const char* name) { return files.erase(name) != 0; }
};
extern FakeLittleFS LittleFS;
