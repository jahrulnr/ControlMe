#pragma once
// Chunked byte sink for streaming (file reads, images, request bodies).
// Caller always on the same task; no re-entrant cross-task callbacks.
#include <stddef.h>
#include <stdint.h>
#include "common/error.h"

namespace controlme {

class IByteSink {
 public:
  virtual ~IByteSink() = default;
  virtual Status write(const uint8_t* data, size_t len) = 0;
};

// Chunked byte source (used by upload adapters: HTTP PUT / FTP STOR -> SD lease).
class IByteSource {
 public:
  virtual ~IByteSource() = default;
  // Fills buf; returns the byte count (0 = EOF). Failed Status -> error.
  virtual Status read(uint8_t* buf, size_t cap, size_t& outLen) = 0;
};

}  // namespace controlme
