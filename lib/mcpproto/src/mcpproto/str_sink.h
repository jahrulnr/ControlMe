#pragma once
// StrSink — IByteSink over a caller-owned buffer (all-or-nothing per
// write). Always NUL-terminated; cap INCLUDES NUL. Overflow: writes nothing,
// overflow_=true, returns Err::Internal.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include "common/error.h"
#include "ports/writer.h"

namespace controlme::mcp {

class StrSink : public IByteSink {
 public:
  StrSink(char* buf, size_t cap) : buf_(buf), cap_(cap) {
    if (buf_ && cap_ > 0) buf_[0] = '\0';
  }

  Status write(const uint8_t* data, size_t len) override {
    if (len_ + len + 1 > cap_) {
      overflow_ = true;
      return Status::Fail(Err::Internal, "result overflow");
    }
    memcpy(buf_ + len_, data, len);
    len_ += len;
    buf_[len_] = '\0';
    return Status::Ok();
  }

  bool append(const char* s) { return append(s, strlen(s)); }
  bool append(const char* s, size_t n) {
    return write(reinterpret_cast<const uint8_t*>(s), n).ok();
  }
  bool appendf(const char* fmt, ...) {
    if (len_ + 1 >= cap_) {
      overflow_ = true;
      return false;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf_ + len_, cap_ - len_, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap_ - len_) {
      overflow_ = true;
      return false;
    }
    len_ += (size_t)n;
    return true;
  }

  size_t len() const { return len_; }
  const char* data() const { return buf_; }
  bool overflow() const { return overflow_; }
  void reset() {
    len_ = 0;
    overflow_ = false;
    if (buf_ && cap_ > 0) buf_[0] = '\0';
  }

 private:
  char* buf_;
  size_t cap_;
  size_t len_ = 0;
  bool overflow_ = false;
};

}  // namespace controlme::mcp
