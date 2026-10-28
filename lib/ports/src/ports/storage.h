#pragma once
// Storage port — single owner of the SD card (SDMMC slot1 width1).
// All SD access (HTTP/AI/FTP) goes through here; no bypass.
// Facade: called from the caller's task, implementations enqueue to the
// owner task and wait for completion with a bounded deadline — not locks,
// not re-entrant callbacks. The SD<->GPIO13 handoff is orchestrated by
// SdServoCoordinator via mount()/unmount() + readiness flag — Storage never
// waits on Servo directly (no deadlock).
#include "common/error.h"
#include "common/config.h"
#include "ports/writer.h"

namespace controlme {

struct FileStat {
  char path[cfg::kPathCap] = {};
  uint64_t size = 0;
  uint64_t revision = 0;       // optimistic concurrency
  bool isDir = false;
  uint64_t mtimeMs = 0;
};

struct ListPage {
  FileStat entries[16];
  uint8_t count = 0;
  char nextCursor[cfg::kIdCap] = {};   // empty = exhausted
};

class IStorage {
 public:
  virtual ~IStorage() = default;

  // --- lifecycle (used by the coordinator on boot/remount/handoff) ---
  virtual bool isMounted() = 0;
  virtual Status mount() = 0;     // SD_UNAVAILABLE when absent/failed
  virtual Status unmount() = 0;

  // --- read path ---
  virtual Status list(const char* path, const char* cursor, uint8_t limit,
                      ListPage& out) = 0;
  virtual Status stat(const char* path, FileStat& out) = 0;
  // Stream file contents to the sink in 4 KiB chunks (no file-sized buffer).
  virtual Status read(const char* path, uint64_t offset, uint64_t length,
                      IByteSink& sink) = 0;

  // --- 4-phase write transaction; one write lease per path (docs/03
  // sd_write) --- mode: "create" | "replace"; create requires a missing
  // target (existing -> InvalidArgument); replace requires matching
  // expectedRevision (-> RevisionConflict).
  virtual Status writeBegin(const char* path, bool replace, uint64_t expectedRevision,
                            char uploadIdOut[cfg::kIdCap]) = 0;
  // data already base64-decoded; offset must == server nextOffset.
  virtual Status writeChunk(const char* uploadId, uint64_t offset,
                            const uint8_t* data, size_t len,
                            uint64_t& nextOffsetOut) = 0;
  virtual Status writeCommit(const char* uploadId, uint64_t totalBytes,
                             uint64_t& revisionOut) = 0;
  virtual Status writeAbort(const char* uploadId) = 0;

  // --- other mutations ---
  virtual Status mkdir(const char* path, uint64_t& revisionOut) = 0;
  virtual Status rename(const char* src, const char* dst, uint64_t expectedRevision,
                        uint64_t& revisionOut) = 0;
  virtual Status remove(const char* path, bool recursive) = 0;

  virtual Status capacity(uint64_t& totalOut, uint64_t& freeOut) = 0;
};

}  // namespace controlme
