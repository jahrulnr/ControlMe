#pragma once
// StorageOwner — IStorage implementation on top of SD_MMC (SDMMC slot1
// width1) with the single-owner pattern (src/owner_task.h): the facade copies
// the POD request to the heap, enqueues it to the storage task, and waits for
// completion with a bounded deadline. ALL filesystem operations
// (fopen/fread/fwrite/stat/rename/rmdir/opendir) run only in the owner task —
// full confinement; no resource mutexes, no synchronous cross-task callbacks.
//
// Concrete decisions & known (honest) limits:
//  * Media: SD_MMC.begin("/sdcard", true) — slot 1, mode1bit=true (docs/05).
//    DAT3/GPIO13 must already be HIGH before mount is called — guaranteed by
//    the caller (SdServoCoordinator), NOT managed here.
//  * Revision: in-memory path->u64 table (kRevEntries; full -> evict oldest).
//    Entry starts at 1 when a path is first SEEN (stat/list/read/begin/commit),
//    +1 per successful mutation. RAM only -> RESET every boot; clients must
//    re-stat after a fresh boot (consistent with bootId reconciliation
//    docs/07).
//  * expectedRevision: 0 = "do not check" — only valid for create mode.
//    create REQUIRES a missing target (existing path -> InvalidArgument;
//    expected!=0 vs missing rev 0 -> RevisionConflict); replace REQUIRES
//    matching expectedRevision -> else RevisionConflict.
//  * Write transactions (docs/03 sd_write): unique staging per transaction
//    "/sdcard/.tmp-<counter>-<rand>"; ONE write lease per path -> other
//    writers get ResourceBusy; read/rename/remove on a leased path ->
//    FileBusy. Leases idle for kWriteLeaseIdleMs are swept LAZY on incoming
//    requests -> auto-abort (staging removed) + log; original file stays
//    intact — sufficient, because a lease only matters when a competitor
//    exists (the owner FIFO queue stays fair across uploads).
//  * Idempotent CHUNK: offset==nextOffset -> normal write; offset==LAST
//    chunk's offset with identical len+crc32 -> return nextOffset without
//    rewriting; different bytes at the same offset -> RevisionConflict; other
//    offsets -> RevisionConflict (non-contiguous stream). Duplicates are
//    tracked only for the last chunk (bounded state) — per spec.
//  * COMMIT checks contiguity (totalBytes==nextOffset) + re-checks
//    expectedRevision -> atomic staging->target rename (old target removed
//    first if present — FAT rename does not replace). On failure -> honest
//    error and the staging file is LEFT on disk as an inspection trail
//    (docs/03).
//  * read(): the facade pumps kIoChunk chunks through the queue — each chunk
//    is one Read request (reopen+fseek, no lingering read handle) and
//    sink.write is called from the CALLER's task so network sends never hold
//    the owner task ("network chunks don't hold the owner busy"). If the
//    path's revision changes mid-read -> RevisionConflict, not silently mixed
//    content.
//  * writeAbort is idempotent: unknown uploadId -> Ok (nothing aborted).
//  * File offset bounded by LONG_MAX via 32-bit POSIX fseek (~2 GiB) ->
//    beyond that InvalidArgument. Recursive remove bounded by kRemoveMaxDepth
//    -> Internal when deeper.
//  * Case sensitivity: rev-table and lease keys are case-SENSITIVE API
//    strings over a case-INSENSITIVE FAT — "/a.txt" and "/A.TXT" are the
//    same file but different keys (lease protection bypassable by
//    respelling; one file can hold two rev entries -> spurious conflict).
//    Accepted limit — canonical casing is the caller's job. The FS ops
//    themselves stay correct: f_rename detects same-entry renames
//    (opRename), staging/target names can never alias (opWriteCommit).
//  * isMounted(): volatile byte written only by the owner task — atomic
//    1-byte read from other tasks; reflects the last mount/unmount, silently
//    removed media is reported by the next op via errno mapping.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>   // FILE* (lease-owned staging handle, owner task only)
#include "common/config.h"
#include "common/error.h"
#include "ports/storage.h"
#include "ports/writer.h"
// owner_task.h lives in src/ (not a lib): resolved via __has_include
// (same convention used by the servo/camera modules); relative-path fallback
// so the shared owner pattern is used without duplication. This class remains
// a port adapter.
#if __has_include("owner_task.h")
#include "owner_task.h"
#else
#include "../../../../src/owner_task.h"
#endif

namespace controlme {

// Owner request op — one POD through the queue (see owner_task.h).
enum class StorageOp : uint8_t {
  Mount = 0, Unmount, List, Stat, Read,
  WriteBegin, WriteChunk, WriteCommit, WriteAbort,
  Mkdir, Rename, Remove, Capacity,
};

// Request POD: OwnerCtrl MUST be the first member (static_assert in
// OwnerTask). Large buffers (data/page) live in the heap request — NOT on the
// owner task stack.
struct StorageReq {
  OwnerCtrl ctrl;
  StorageOp op = StorageOp::Stat;
  // ---- input ----
  char path[cfg::kPathCap] = {};       // API path ("/a/b"), not "/sdcard/.."
  char path2[cfg::kPathCap] = {};      // dst for Rename
  char cursor[cfg::kIdCap] = {};       // opaque cursor = internal decimal index
  char uploadId[cfg::kIdCap] = {};
  uint64_t offset = 0;                 // Read/WriteChunk
  uint64_t length = 0;                 // Read: bytes requested for this chunk
  uint64_t expectedRevision = 0;
  uint64_t totalBytes = 0;             // WriteCommit
  uint32_t dataLen = 0;                // WriteChunk: valid bytes in data[]
  uint8_t limit = 0;
  bool replace = false;
  bool recursive = false;
  uint8_t data[cfg::kIoChunk] = {};    // incoming chunk payload / read bounce
  // ---- output ----
  uint64_t revision = 0;               // path revision / new revision
  uint64_t currentRevision = 0;        // actual rev on conflict (for details)
  uint64_t nextOffset = 0;             // WriteChunk
  uint64_t fileSize = 0;               // Read/Stat
  uint64_t totalOut = 0;               // Capacity
  uint64_t freeOut = 0;                // Capacity
  uint32_t readLen = 0;                // Read: valid bytes in data[]
  FileStat stat;
  ListPage page;
};

class StorageOwner : public IStorage {
 public:
  StorageOwner() = default;
  StorageOwner(const StorageOwner&) = delete;
  StorageOwner& operator=(const StorageOwner&) = delete;

  // Create queue + owner task (stack cfg::kStackStorage, queue
  // cfg::kStorageQueueDepth). Called once from controlme_register_storage —
  // does NOT mount (mount is orchestrated by the coordinator from main.cpp).
  Status start();

  // ---- IStorage facade — thread-safe via queue (may be called from other tasks) ----
  bool isMounted() override;
  Status mount() override;
  Status unmount() override;
  Status list(const char* path, const char* cursor, uint8_t limit,
              ListPage& out) override;
  Status stat(const char* path, FileStat& out) override;
  Status read(const char* path, uint64_t offset, uint64_t length,
              IByteSink& sink) override;
  Status writeBegin(const char* path, bool replace, uint64_t expectedRevision,
                    char uploadIdOut[cfg::kIdCap]) override;
  Status writeChunk(const char* uploadId, uint64_t offset,
                    const uint8_t* data, size_t len,
                    uint64_t& nextOffsetOut) override;
  Status writeCommit(const char* uploadId, uint64_t totalBytes,
                     uint64_t& revisionOut) override;
  Status writeAbort(const char* uploadId) override;
  Status mkdir(const char* path, uint64_t& revisionOut) override;
  Status rename(const char* src, const char* dst, uint64_t expectedRevision,
                uint64_t& revisionOut) override;
  Status remove(const char* path, bool recursive) override;
  Status capacity(uint64_t& totalOut, uint64_t& freeOut) override;

 private:
  static constexpr uint8_t kMaxWriteLeases = 4;    // concurrent active uploads
  static constexpr uint16_t kRevEntries = 128;     // docs: bounded ~128
  static constexpr uint8_t kRemoveMaxDepth = 8;
  static constexpr uint16_t kFsPathCap = cfg::kPathCap + 16;  // + "/sdcard"
  static constexpr uint8_t kPageEntries =
      sizeof(ListPage::entries) / sizeof(ListPage::entries[0]);

  struct RevEntry {
    char path[cfg::kPathCap];
    uint64_t rev;
    uint32_t seq;                    // insert order -> evict oldest
    bool used;
  };
  struct Lease {
    bool used = false;
    char path[cfg::kPathCap] = {};   // target API path ("/a/b.txt")
    char staging[kFsPathCap] = {};   // "/sdcard/.tmp-<counter>-<rand>"
    char uploadId[cfg::kIdCap] = {};
    FILE* f = nullptr;
    uint64_t nextOffset = 0;
    uint64_t lastActiveMs = 0;
    uint64_t expectedRevision = 0;   // re-checked at commit
    uint32_t lastCrc = 0;
    uint32_t lastLen = 0;
    bool hasLast = false;            // last chunk present for dedup
  };
  // State touched only by the owner task (handler ctx) — confinement.
  struct State {
    volatile bool mounted = false;   // written by owner only; read by isMounted()
    RevEntry revs[kRevEntries];
    uint32_t revSeq = 0;
    Lease leases[kMaxWriteLeases];
    uint32_t stagingCounter = 0;
  };

  Status callReq(StorageReq& in, StorageReq& out);
  // Heap-allocated request pair (StorageReq ~8 KiB — two on the caller's
  // stack would overflow the 8 KiB httpd task).
  static struct StorageReqPair* allocReq();
  static void handle(StorageReq& r, void* ctx);

  // ---- helpers & ops (all static members -> private state access; run
  //      only in the owner task via handle()) ----
  static uint64_t nowMs();
  static bool toFsPath(const char* api, char* out, size_t cap);
  static Status fsErr(int e, const char* what);
  static int revFind(State& s, const char* path);
  static int revAlloc(State& s);                          // free/oldest slot
  static uint64_t revPeek(State& s, const char* path);
  static uint64_t revTouch(State& s, const char* path);   // assign 1 when new
  static uint64_t revBump(State& s, const char* path);    // +1 per successful mutation
  static void revDropPath(State& s, const char* path);    // path + those below it
  static void revMove(State& s, const char* src, const char* dst);
  static Lease* leaseByUploadId(State& s, const char* uploadId);
  static Lease* leaseByPath(State& s, const char* apiPath);
  // true when lease.path/staging == path or lies BELOW the path dir.
  static bool leaseBlocking(State& s, const char* apiPath);
  static void abortLease(State& s, Lease& l, const char* why);
  // Post-data commit failure: staging LEFT (inspection trail docs/03).
  static void closeKeepStaging(Lease& l, const char* why);
  static void sweepLeases(State& s, uint64_t nowMs);
  static void fillStat(FileStat& fe, const char* apiPath, uint64_t size,
                       bool isDir, uint64_t mtimeMs);
  static Status removeTree(char* fsPathBuf, size_t fsCap, const char* apiPath,
                           uint8_t depth);

  static void opMount(State& s, StorageReq& r);
  static void opUnmount(State& s, StorageReq& r);
  static void opList(State& s, StorageReq& r);
  static void opStat(State& s, StorageReq& r);
  static void opRead(State& s, StorageReq& r);
  static void opWriteBegin(State& s, StorageReq& r);
  static void opWriteChunk(State& s, StorageReq& r);
  static void opWriteCommit(State& s, StorageReq& r);
  static void opWriteAbort(State& s, StorageReq& r);
  static void opMkdir(State& s, StorageReq& r);
  static void opRename(State& s, StorageReq& r);
  static void opRemove(State& s, StorageReq& r);
  static void opCapacity(State& s, StorageReq& r);

  State state_;   // ~24 KB (rev table) — static instance, not on a stack
  OwnerTask<StorageReq> task_;
};

}  // namespace controlme
