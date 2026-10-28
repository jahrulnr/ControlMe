#pragma once
// DeviceLog port — persistent log in internal LittleFS (/logs/current.log
// + prev.log, rotated per boot). Facade: all file I/O is executed by the
// FileLog owner task; callers block with a deadline — never touching the
// FS.
#include <stddef.h>
#include <stdint.h>
#include "common/error.h"

namespace controlme {

enum class LogFile : uint8_t { Current = 0, Previous = 1 };

struct LogReadResult {
  size_t outLen = 0;   // bytes copied to dst
  size_t size = 0;     // full file size
  bool eof = false;    // offset+outLen reached end of file
  uint32_t generation = 0;  // bumped per keep-tail truncate; streaming
                            // readers may abort if it changes mid-read
};

class IDeviceLog {
 public:
  virtual ~IDeviceLog() = default;
  // Copy up to cap bytes starting at offset into dst (the file read runs
  // in the log owner task; the caller blocks with a deadline). cap is
  // clamped to cfg::kIoChunk. dst is written by the facade on success
  // only — the owner never sees it, so a Timeout leaves caller memory
  // untouched and freeable.
  virtual Status read(LogFile which, size_t offset, char* dst, size_t cap, uint32_t timeoutMs,
                      LogReadResult& out) = 0;
  virtual Status flush(uint32_t timeoutMs) = 0;
};

}  // namespace controlme
