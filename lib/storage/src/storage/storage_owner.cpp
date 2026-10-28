// StorageOwner — see header for contract/known limits.
// This file is the only place that touches the SD filesystem:
// all op*/helper functions run in the owner task via OwnerTask<StorageReq>.
#include "storage/storage_owner.h"

#include <dirent.h>
#include <errno.h>
#include <new>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SD_MMC.h>
#include <esp_log.h>

#include "common/psram_alloc.h"
#include <esp_random.h>
#include <esp_rom_crc.h>
#include <esp_timer.h>

#include "common/config.h"
#include "common/event.h"   // copyStr

namespace controlme {

namespace {
const char* kTag = "storage";
constexpr char kMountPoint[] = "/sdcard";   // VFS mount SD_MMC
constexpr size_t kMountLen = sizeof(kMountPoint) - 1;
}  // namespace

// ------------------------------------------------------------------- utils

uint64_t StorageOwner::nowMs() {
  return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

// Path API "/a/b" -> path fs "/sdcard/a/b"; "/" -> "/sdcard".
// Rejects: not starting with '/', containing "..", or too long.
bool StorageOwner::toFsPath(const char* api, char* out, size_t cap) {
  if (!api || api[0] != '/' || strstr(api, "..")) return false;
  if (strlen(api) >= cfg::kPathCap) return false;
  int n = snprintf(out, cap, "%s%s", kMountPoint, api);
  if (n <= 0 || n >= (int)cap) return false;
  // Tidy trailing slash (unless the result is exactly "/sdcard").
  while (n > (int)kMountLen && out[n - 1] == '/') out[--n] = '\0';
  return true;
}

// errno -> shared taxonomy (adapter boundary; docs/04).
Status StorageOwner::fsErr(int e, const char* what) {
  switch (e) {
    case ENOENT:
    case ENOTDIR:
      return Status::Fail(Err::NotFound, what ? what : "path not found");
    case ENODEV:
    case EIO:
    case EBADF:
      return Status::Fail(Err::SdUnavailable, "sd media inaccessible");
    case ENOSPC:
      return Status::Fail(Err::Internal, "sd card full");
    case EEXIST:
      return Status::Fail(Err::InvalidArgument, "path already exists");
    case ENOTEMPTY:
      return Status::Fail(Err::InvalidArgument, "directory not empty");
    case EISDIR:
      return Status::Fail(Err::InvalidArgument, "path is a directory");
    default:
      return Status::Fail(Err::Internal, what ? what : "fs operation failed");
  }
}

// ------------------------------------------------- revision table (owner only)

int StorageOwner::revFind(State& s, const char* path) {
  for (uint16_t i = 0; i < kRevEntries; ++i) {
    if (s.revs[i].used && strcmp(s.revs[i].path, path) == 0) return (int)i;
  }
  return -1;
}

uint64_t StorageOwner::revPeek(State& s, const char* path) {
  int i = revFind(s, path);
  return i >= 0 ? s.revs[i].rev : 0;
}

// Free slot; when full, evict the entry with the oldest seq (bounded ~128).
int StorageOwner::revAlloc(State& s) {
  int freeIdx = -1, oldestIdx = 0;
  uint32_t minSeq = UINT32_MAX;
  for (uint16_t i = 0; i < kRevEntries; ++i) {
    if (!s.revs[i].used) {
      freeIdx = (int)i;
      break;
    }
    if (s.revs[i].seq < minSeq) {
      minSeq = s.revs[i].seq;
      oldestIdx = (int)i;
    }
  }
  return freeIdx >= 0 ? freeIdx : oldestIdx;
}

uint64_t StorageOwner::revTouch(State& s, const char* path) {
  int i = revFind(s, path);
  if (i >= 0) return s.revs[i].rev;
  i = revAlloc(s);
  copyStr(s.revs[i].path, sizeof(s.revs[i].path), path);
  s.revs[i].rev = 1;             // starts at 1 on first sight
  s.revs[i].seq = ++s.revSeq;
  s.revs[i].used = true;
  return 1;
}

uint64_t StorageOwner::revBump(State& s, const char* path) {
  int i = revFind(s, path);
  if (i < 0) {
    i = revAlloc(s);
    copyStr(s.revs[i].path, sizeof(s.revs[i].path), path);
    s.revs[i].rev = 0;
    s.revs[i].used = true;
  }
  s.revs[i].seq = ++s.revSeq;    // refresh age
  return ++s.revs[i].rev;        // +1 per successful mutation
}

// Drop the path entry + all entries below it ("<path>/..."); path "/" =
// root means dropping the ENTIRE table (all paths are below root).
void StorageOwner::revDropPath(State& s, const char* path) {
  const size_t n = strlen(path);
  for (uint16_t i = 0; i < kRevEntries; ++i) {
    if (!s.revs[i].used) continue;
    const char* p = s.revs[i].path;
    if (strcmp(p, path) == 0 || n == 1 ||
        (strncmp(p, path, n) == 0 && p[n] == '/')) {
      s.revs[i].used = false;
    }
  }
}

// Rename moves an object: the "src" entry and its children are rewritten
// to dst; the moved root's revision = src rev + 1 (successful mutation). Old
// dst entries (overwritten targets) are dropped first.
void StorageOwner::revMove(State& s, const char* src, const char* dst) {
  const uint64_t srcRev = revPeek(s, src);
  revDropPath(s, dst);
  const size_t sl = strlen(src);
  for (uint16_t i = 0; i < kRevEntries; ++i) {
    if (!s.revs[i].used) continue;
    const char* p = s.revs[i].path;
    if (strcmp(p, src) != 0 && !(strncmp(p, src, sl) == 0 && p[sl] == '/')) {
      continue;
    }
    char np[cfg::kPathCap];
    int n = snprintf(np, sizeof(np), "%s%s", dst, p + sl);
    if (n <= 0 || n >= (int)sizeof(np)) {
      s.revs[i].used = false;    // new path too long -> entry dropped (honest)
      continue;
    }
    copyStr(s.revs[i].path, sizeof(s.revs[i].path), np);
  }
  int i = revFind(s, dst);
  if (i < 0) {
    i = revAlloc(s);
    copyStr(s.revs[i].path, sizeof(s.revs[i].path), dst);
    s.revs[i].rev = srcRev;
    s.revs[i].used = true;
  }
  s.revs[i].rev = srcRev + 1;
  s.revs[i].seq = ++s.revSeq;
}

// ------------------------------------------------------- leases (owner only)

StorageOwner::Lease* StorageOwner::leaseByUploadId(State& s,
                                                  const char* uploadId) {
  if (!uploadId || !uploadId[0]) return nullptr;
  for (auto& l : s.leases) {
    if (l.used && strcmp(l.uploadId, uploadId) == 0) return &l;
  }
  return nullptr;
}

StorageOwner::Lease* StorageOwner::leaseByPath(State& s, const char* apiPath) {
  for (auto& l : s.leases) {
    if (l.used && strcmp(l.path, apiPath) == 0) return &l;
  }
  return nullptr;
}

// A lease blocks delete/rename ops on a path when the target == path or
// lies BELOW the path dir (e.g. removing "/a" while upload "/a/f.txt" is
// active). The "/.tmp-*" staging path is checked too so staging files can't
// be removed by outside hands.
bool StorageOwner::leaseBlocking(State& s, const char* apiPath) {
  const size_t n = strlen(apiPath);
  for (auto& l : s.leases) {
    if (!l.used) continue;
    const char* stagingApi = l.staging + (int)kMountLen;   // "/sdcard/X" -> "/X"
    const char* cand[2] = {l.path, stagingApi};
    for (const char* c : cand) {
      if (strcmp(c, apiPath) == 0) return true;
      // path "/" = root -> all lease targets are below it.
      if (n == 1 || (strncmp(c, apiPath, n) == 0 && c[n] == '/')) return true;
    }
  }
  return false;
}

// Abort: close + remove staging + release lease + log (not silently).
void StorageOwner::abortLease(State& s, Lease& l, const char* why) {
  (void)s;
  if (l.f) {
    fclose(l.f);
    l.f = nullptr;
  }
  if (l.staging[0]) ::remove(l.staging);
  ESP_LOGW(kTag, "upload %s aborted (%s); staging %s removed, target %s intact",
           l.uploadId, why, l.staging, l.path);
  l.used = false;
}

// Commit failure after data was written: staging is LEFT on disk
// (inspection trail, docs/03) — only close handle + release lease + log.
void StorageOwner::closeKeepStaging(Lease& l, const char* why) {
  if (l.f) {
    fclose(l.f);
    l.f = nullptr;
  }
  ESP_LOGW(kTag, "upload %s failed (%s); staging %s LEFT for inspection",
           l.uploadId, why, l.staging);
  l.used = false;
}

// LAZY sweep: checked on incoming requests — leases only matter when a
// competitor exists, so no separate timer needed (docs/03 idle 15 s).
void StorageOwner::sweepLeases(State& s, uint64_t now) {
  for (auto& l : s.leases) {
    if (l.used && now - l.lastActiveMs > cfg::kWriteLeaseIdleMs) {
      abortLease(s, l, "idle timeout");
    }
  }
}

void StorageOwner::fillStat(FileStat& fe, const char* apiPath, uint64_t size,
                            bool isDir, uint64_t mtimeMs) {
  copyStr(fe.path, sizeof(fe.path), apiPath);
  fe.size = size;
  fe.isDir = isDir;
  fe.mtimeMs = mtimeMs;
}

// ------------------------------------------------------------------- ops

void StorageOwner::opMount(State& s, StorageReq& r) {
  if (s.mounted) {
    r.ctrl.status = Status::Ok();
    return;
  }
  // DAT3/GPIO13 already HIGH — guaranteed by coordinator before call (docs/05).
  // mode1bit=true -> slot1 width1 (CLK14/CMD15/DAT0=GPIO2).
  if (!SD_MMC.begin(kMountPoint, /*mode1bit=*/true)) {
    r.ctrl.status = Status::Fail(Err::SdUnavailable, "sd mount failed");
    return;
  }
  s.mounted = true;
  ESP_LOGI(kTag, "sd mounted slot1 width1");
  r.ctrl.status = Status::Ok();
}

void StorageOwner::opUnmount(State& s, StorageReq& r) {
  // Active uploads are aborted cleanly (staging removed, logged) — no
  // handle leaks into removed media.
  for (auto& l : s.leases) {
    if (l.used) abortLease(s, l, "unmount");
  }
  if (s.mounted) SD_MMC.end();
  s.mounted = false;
  r.ctrl.status = Status::Ok();
}

void StorageOwner::opList(State& s, StorageReq& r) {
  if (!s.mounted) {
    r.ctrl.status = Status::Fail(Err::SdUnavailable, "sd not mounted");
    return;
  }
  char fs[kFsPathCap];
  if (!toFsPath(r.path, fs, sizeof(fs))) {
    r.ctrl.status = Status::Fail(Err::InvalidArgument, "invalid path");
    return;
  }
  // Opaque cursor = decimal readdir position index (best-effort: dirent
  // order may shift if the dir changes between pages — honestly noted).
  uint32_t skip = 0;
  if (r.cursor[0]) {
    char* end = nullptr;
    unsigned long v = strtoul(r.cursor, &end, 10);
    if (!end || *end != '\0') {
      r.ctrl.status = Status::Fail(Err::InvalidArgument, "invalid cursor");
      return;
    }
    skip = (uint32_t)v;
  }
  DIR* d = opendir(fs);
  if (!d) {
    r.ctrl.status = fsErr(errno, "directory not found");
    return;
  }
  const uint8_t lim = (r.limit == 0 || r.limit > kPageEntries)
                          ? kPageEntries
                          : r.limit;
  r.page.count = 0;
  r.page.nextCursor[0] = '\0';
  uint32_t idx = 0;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    if (e->d_name[0] == '.' &&
        (e->d_name[1] == '\0' ||
         (e->d_name[1] == '.' && e->d_name[2] == '\0'))) {
      continue;   // "." and ".."
    }
    const uint32_t myIdx = idx++;
    if (myIdx < skip) continue;
    if (r.page.count >= lim) {
      // More entries remain -> next page starts at this index.
      snprintf(r.page.nextCursor, sizeof(r.page.nextCursor), "%lu",
               (unsigned long)myIdx);
      break;
    }
    char apiChild[cfg::kPathCap];
    char fsChild[kFsPathCap];
    int n = snprintf(apiChild, sizeof(apiChild), "%s%s%s", r.path,
                     (r.path[0] == '/' && r.path[1] == '\0') ? "" : "/",
                     e->d_name);
    if (n <= 0 || n >= (int)sizeof(apiChild) ||
        !toFsPath(apiChild, fsChild, sizeof(fsChild))) {
      continue;   // name too long -> skip (index still consumed)
    }
    struct stat stt;
    if (::stat(fsChild, &stt) != 0) continue;   // gone mid-list -> skip
    FileStat& fe = r.page.entries[r.page.count];
    fillStat(fe, apiChild, (uint64_t)stt.st_size, S_ISDIR(stt.st_mode),
             (uint64_t)stt.st_mtime * 1000ULL);
    fe.revision = revTouch(s, apiChild);   // first seen -> 1
    ++r.page.count;
  }
  closedir(d);
  r.ctrl.status = Status::Ok();
}

void StorageOwner::opStat(State& s, StorageReq& r) {
  if (!s.mounted) {
    r.ctrl.status = Status::Fail(Err::SdUnavailable, "sd not mounted");
    return;
  }
  char fs[kFsPathCap];
  if (!toFsPath(r.path, fs, sizeof(fs))) {
    r.ctrl.status = Status::Fail(Err::InvalidArgument, "invalid path");
    return;
  }
  struct stat stt;
  if (::stat(fs, &stt) != 0) {
    r.ctrl.status = fsErr(errno, "path not found");
    return;
  }
  fillStat(r.stat, r.path, (uint64_t)stt.st_size, S_ISDIR(stt.st_mode),
           (uint64_t)stt.st_mtime * 1000ULL);
  r.stat.revision = revTouch(s, r.path);
  r.revision = r.stat.revision;
  r.ctrl.status = Status::Ok();
}

void StorageOwner::opRead(State& s, StorageReq& r) {
  if (!s.mounted) {
    r.ctrl.status = Status::Fail(Err::SdUnavailable, "sd not mounted");
    return;
  }
  char fs[kFsPathCap];
  if (!toFsPath(r.path, fs, sizeof(fs))) {
    r.ctrl.status = Status::Fail(Err::InvalidArgument, "invalid path");
    return;
  }
  // sd_read -> FILE_BUSY when the path has an active upload lease (docs/03).
  if (leaseByPath(s, r.path)) {
    r.ctrl.status = Status::Fail(Err::FileBusy, "upload in progress on path");
    return;
  }
  struct stat stt;
  if (::stat(fs, &stt) != 0) {
    r.ctrl.status = fsErr(errno, "path not found");
    return;
  }
  if (S_ISDIR(stt.st_mode)) {
    r.ctrl.status = Status::Fail(Err::InvalidArgument, "path is a directory");
    return;
  }
  r.fileSize = (uint64_t)stt.st_size;
  if (r.offset > r.fileSize) {
    r.ctrl.status =
        Status::Fail(Err::InvalidArgument, "offset exceeds file size");
    return;
  }
  if (r.offset > (uint64_t)LONG_MAX) {
    // 32-bit POSIX fseek — known honest limit (see header).
    r.ctrl.status =
        Status::Fail(Err::InvalidArgument, "offset >2GiB not supported");
    return;
  }
  FILE* f = fopen(fs, "rb");
  if (!f) {
    r.ctrl.status = fsErr(errno, "file open failed");
    return;
  }
  Status st = Status::Ok();
  if (fseek(f, (long)r.offset, SEEK_SET) != 0) {
    st = fsErr(errno, "seek failed");
  } else {
    size_t want = (size_t)(r.length > cfg::kIoChunk ? cfg::kIoChunk : r.length);
    r.readLen = (uint32_t)fread(r.data, 1, want, f);
    if (ferror(f)) st = fsErr(errno ? errno : EIO, "file read failed");
  }
  fclose(f);
  if (st.ok()) {
    r.revision = revTouch(s, r.path);   // used by facade for consistency
  }
  r.ctrl.status = st;
}

void StorageOwner::opWriteBegin(State& s, StorageReq& r) {
  if (!s.mounted) {
    r.ctrl.status = Status::Fail(Err::SdUnavailable, "sd not mounted");
    return;
  }
  char fs[kFsPathCap];
  if (!toFsPath(r.path, fs, sizeof(fs)) || r.path[1] == '\0') {
    r.ctrl.status = Status::Fail(Err::InvalidArgument, "invalid path");
    return;
  }
  // One write lease per path.
  if (leaseByPath(s, r.path)) {
    r.ctrl.status = Status::Fail(Err::ResourceBusy, "write lease in use");
    return;
  }
  // Staging namespace is reserved — writing "/.tmp-*" directly could
  // overwrite an active upload's staging file and ruin the inspection trail.
  if (strncmp(r.path, "/.tmp-", 6) == 0) {
    r.ctrl.status = Status::Fail(Err::InvalidArgument,
                               "path /.tmp-* reserved for staging");
    return;
  }
  struct stat stt;
  const bool exists = (::stat(fs, &stt) == 0);
  if (exists && S_ISDIR(stt.st_mode)) {
    r.ctrl.status = Status::Fail(Err::InvalidArgument, "path is a directory");
    return;
  }
  const uint64_t cur = exists ? revTouch(s, r.path) : 0;
  if (r.replace) {
    if (!exists) {
      r.ctrl.status =
          Status::Fail(Err::NotFound, "replace target does not exist");
      return;
    }
    if (r.expectedRevision == 0) {
      r.ctrl.status = Status::Fail(Err::InvalidArgument,
                                  "expectedRevision required for replace");
      return;
    }
    if (cur != r.expectedRevision) {
      r.currentRevision = cur;
      r.ctrl.status =
          Status::Fail(Err::RevisionConflict, "expectedRevision mismatch");
      return;
    }
  } else if (r.expectedRevision != 0 && cur != r.expectedRevision) {
    // create with expected!=0 -> still checked (missing path = rev 0).
    r.currentRevision = cur;
    r.ctrl.status =
        Status::Fail(Err::RevisionConflict, "expectedRevision mismatch");
    return;
  }
  Lease* l = nullptr;
  for (auto& c : s.leases) {
    if (!c.used) {
      l = &c;
      break;
    }
  }
  if (!l) {
    r.ctrl.status =
        Status::Fail(Err::ResourceBusy, "upload slots full");
    return;
  }
  const uint32_t cnt = ++s.stagingCounter;
  const uint32_t rnd = esp_random();
  snprintf(l->staging, sizeof(l->staging), "%s/.tmp-%08lx-%08lx", kMountPoint,
           (unsigned long)cnt, (unsigned long)rnd);
  snprintf(l->uploadId, sizeof(l->uploadId), "w-%08lx-%08lx",
           (unsigned long)cnt, (unsigned long)rnd);
  l->f = fopen(l->staging, "wb");
  if (!l->f) {
    r.ctrl.status = fsErr(errno, "staging create failed");
    return;
  }
  copyStr(l->path, sizeof(l->path), r.path);
  l->nextOffset = 0;
  l->expectedRevision = r.expectedRevision;
  l->lastCrc = 0;
  l->lastLen = 0;
  l->hasLast = false;
  l->lastActiveMs = nowMs();
  l->used = true;
  copyStr(r.uploadId, sizeof(r.uploadId), l->uploadId);
  r.nextOffset = 0;
  r.ctrl.status = Status::Ok();
}

void StorageOwner::opWriteChunk(State& s, StorageReq& r) {
  Lease* l = leaseByUploadId(s, r.uploadId);
  if (!l) {
    // No lease: unknown, already committed/aborted, or expired and swept.
    r.ctrl.status =
        Status::Fail(Err::InvalidArgument, "unknown uploadId");
    return;
  }
  if (r.dataLen > cfg::kIoChunk) {
    r.ctrl.status = Status::Fail(Err::InvalidArgument, "chunk >4KiB");
    return;
  }
  if (r.offset == l->nextOffset) {
    if (r.dataLen > 0) {
      size_t w = fwrite(r.data, 1, r.dataLen, l->f);
      if (w != r.dataLen) {
        r.ctrl.status = fsErr(errno ? errno : EIO, "staging write failed");
        return;
      }
      l->lastCrc = esp_rom_crc32_le(0, r.data, r.dataLen);
      l->lastLen = r.dataLen;
      l->hasLast = true;
      l->nextOffset += r.dataLen;
    }
    l->lastActiveMs = nowMs();
    r.nextOffset = l->nextOffset;
    r.ctrl.status = Status::Ok();
    return;
  }
  // Duplicate of last chunk: same offset + same len+crc -> idempotent.
  if (l->hasLast && r.offset == l->nextOffset - l->lastLen) {
    if (r.dataLen == l->lastLen &&
        esp_rom_crc32_le(0, r.data, r.dataLen) == l->lastCrc) {
      l->lastActiveMs = nowMs();
      r.nextOffset = l->nextOffset;   // return without rewriting
      r.ctrl.status = Status::Ok();
      return;
    }
    r.ctrl.status = Status::Fail(Err::RevisionConflict,
                               "same offset, different bytes");
    return;
  }
  r.ctrl.status =
      Status::Fail(Err::RevisionConflict, "non-contiguous offset");
}

void StorageOwner::opWriteCommit(State& s, StorageReq& r) {
  Lease* l = leaseByUploadId(s, r.uploadId);
  if (!l) {
    r.ctrl.status =
        Status::Fail(Err::InvalidArgument, "unknown uploadId");
    return;
  }
  char apiPath[cfg::kPathCap];
  copyStr(apiPath, sizeof(apiPath), l->path);
  // Contiguity: totalBytes must equal the bytes received.
  if (r.totalBytes != l->nextOffset) {
    closeKeepStaging(*l, "totalBytes mismatch");
    r.ctrl.status = Status::Fail(Err::RevisionConflict,
                                "totalBytes does not match received data");
    return;
  }
  // expectedRevision re-checked at commit (FTP: initial revision snapshot).
  if (l->expectedRevision != 0) {
    const uint64_t cur = revPeek(s, apiPath);
    if (cur != l->expectedRevision) {
      r.currentRevision = cur;
      closeKeepStaging(*l, "revision changed");
      r.ctrl.status = Status::Fail(Err::RevisionConflict,
                                  "revision changed during upload");
      return;
    }
  }
  int ioerr = 0;
  if (fflush(l->f) != 0 && !ioerr) ioerr = errno;
  if (fclose(l->f) != 0 && !ioerr) ioerr = errno;
  l->f = nullptr;
  if (ioerr) {
    closeKeepStaging(*l, "staging flush failed");
    r.ctrl.status = fsErr(ioerr, "staging flush failed");
    return;
  }
  char fsTarget[kFsPathCap];
  toFsPath(apiPath, fsTarget, sizeof(fsTarget));
  // FAT rename does not replace -> remove old target first.
  struct stat stt;
  if (::stat(fsTarget, &stt) == 0 && ::remove(fsTarget) != 0) {
    closeKeepStaging(*l, "old target removal failed");
    r.ctrl.status = fsErr(errno, "old target removal failed");
    return;
  }
  if (::rename(l->staging, fsTarget) != 0) {
    closeKeepStaging(*l, "staging rename failed");
    r.ctrl.status = fsErr(errno, "commit rename failed");
    return;
  }
  const uint64_t rev = revBump(s, apiPath);
  l->used = false;   // staging already renamed -> lease ends cleanly
  r.revision = rev;
  r.ctrl.status = Status::Ok();
}

void StorageOwner::opWriteAbort(State& s, StorageReq& r) {
  Lease* l = leaseByUploadId(s, r.uploadId);
  if (!l) {
    // Idempotent: aborting an unknown uploadId = nothing is aborted.
    r.ctrl.status = Status::Ok();
    return;
  }
  abortLease(s, *l, "abort by caller");
  r.ctrl.status = Status::Ok();
}

void StorageOwner::opMkdir(State& s, StorageReq& r) {
  if (!s.mounted) {
    r.ctrl.status = Status::Fail(Err::SdUnavailable, "sd not mounted");
    return;
  }
  char fs[kFsPathCap];
  if (!toFsPath(r.path, fs, sizeof(fs)) || r.path[1] == '\0') {
    r.ctrl.status = Status::Fail(Err::InvalidArgument, "invalid path");
    return;
  }
  if (::mkdir(fs, 0777) != 0) {
    r.ctrl.status = fsErr(errno, "mkdir failed");
    return;
  }
  r.revision = revTouch(s, r.path);   // first seen -> 1
  r.ctrl.status = Status::Ok();
}

void StorageOwner::opRename(State& s, StorageReq& r) {
  if (!s.mounted) {
    r.ctrl.status = Status::Fail(Err::SdUnavailable, "sd not mounted");
    return;
  }
  char fsSrc[kFsPathCap], fsDst[kFsPathCap];
  if (!toFsPath(r.path, fsSrc, sizeof(fsSrc)) ||
      !toFsPath(r.path2, fsDst, sizeof(fsDst)) || r.path[1] == '\0' ||
      r.path2[1] == '\0') {
    // "/" is the mountpoint itself — cannot be renamed or used as a target.
    r.ctrl.status = Status::Fail(Err::InvalidArgument, "invalid path");
    return;
  }
  if (leaseBlocking(s, r.path) || leaseBlocking(s, r.path2)) {
    r.ctrl.status =
        Status::Fail(Err::FileBusy, "path has active upload lease");
    return;
  }
  // src == dst: without this guard, "remove dst then rename" deletes the file.
  if (strcmp(fsSrc, fsDst) == 0) {
    struct stat stSelf;
    if (::stat(fsSrc, &stSelf) != 0) {
      r.ctrl.status = fsErr(errno, "source not found");
      return;
    }
    r.revision = revTouch(s, r.path);
    r.ctrl.status = Status::Ok();
    return;
  }
  struct stat stt;
  if (::stat(fsSrc, &stt) != 0) {
    r.ctrl.status = fsErr(errno, "source not found");
    return;
  }
  const uint64_t cur = revTouch(s, r.path);
  if (r.expectedRevision != 0 && cur != r.expectedRevision) {
    r.currentRevision = cur;
    r.ctrl.status =
        Status::Fail(Err::RevisionConflict, "expectedRevision mismatch");
    return;
  }
  // POSIX rename replaces; FAT does not -> remove dst first (dir: only when
  // empty — non-empty contents -> honest error).
  struct stat sd2;
  if (::stat(fsDst, &sd2) == 0) {
    int rc = S_ISDIR(sd2.st_mode) ? ::rmdir(fsDst) : ::remove(fsDst);
    if (rc != 0) {
      r.ctrl.status = fsErr(errno, "dst exists and cannot be replaced");
      return;
    }
  }
  if (::rename(fsSrc, fsDst) != 0) {
    r.ctrl.status = fsErr(errno, "rename failed");
    return;
  }
  revMove(s, r.path, r.path2);
  r.revision = revPeek(s, r.path2);
  r.ctrl.status = Status::Ok();
}

// Remove dir contents then rmdir; called recursively with bounded depth.
Status StorageOwner::removeTree(char* fsPathBuf, size_t fsCap,
                                const char* apiPath, uint8_t depth) {
  (void)apiPath;
  if (depth >= kRemoveMaxDepth) {
    return Status::Fail(Err::Internal, "directory tree too deep");
  }
  DIR* d = opendir(fsPathBuf);
  if (!d) return fsErr(errno, "directory open failed");
  Status st = Status::Ok();
  struct dirent* e;
  char child[kFsPathCap];
  while (st.ok() && (e = readdir(d)) != nullptr) {
    if (e->d_name[0] == '.' &&
        (e->d_name[1] == '\0' ||
         (e->d_name[1] == '.' && e->d_name[2] == '\0'))) {
      continue;
    }
    int n = snprintf(child, sizeof(child), "%s/%s", fsPathBuf, e->d_name);
    if (n <= 0 || n >= (int)sizeof(child)) {
      st = Status::Fail(Err::Internal, "path too long");
      break;
    }
    struct stat stt;
    if (::stat(child, &stt) != 0) {
      st = fsErr(errno, "stat failed during recursion");
      break;
    }
    if (S_ISDIR(stt.st_mode)) {
      st = removeTree(child, fsCap, nullptr, depth + 1);
    } else if (::remove(child) != 0) {
      st = fsErr(errno, "file remove failed");
    }
  }
  closedir(d);
  if (st.ok() && ::rmdir(fsPathBuf) != 0) {
    st = fsErr(errno, "rmdir failed");
  }
  return st;
}

void StorageOwner::opRemove(State& s, StorageReq& r) {
  if (!s.mounted) {
    r.ctrl.status = Status::Fail(Err::SdUnavailable, "sd not mounted");
    return;
  }
  char fs[kFsPathCap];
  if (!toFsPath(r.path, fs, sizeof(fs)) || r.path[1] == '\0') {
    r.ctrl.status = Status::Fail(Err::InvalidArgument, "invalid path");
    return;
  }
  // Delete/rename on a leased path -> FILE_BUSY (docs/03); including leases
  // whose target lies BELOW this dir (recursive).
  if (leaseBlocking(s, r.path)) {
    r.ctrl.status =
        Status::Fail(Err::FileBusy, "path has active upload lease");
    return;
  }
  struct stat stt;
  if (::stat(fs, &stt) != 0) {
    r.ctrl.status = fsErr(errno, "path not found");
    return;
  }
  Status st;
  if (!S_ISDIR(stt.st_mode)) {
    st = (::remove(fs) == 0) ? Status::Ok()
                            : fsErr(errno, "file remove failed");
  } else if (!r.recursive) {
    st = (::rmdir(fs) == 0) ? Status::Ok()
                            : fsErr(errno, "rmdir failed");
  } else {
    char buf[kFsPathCap];
    copyStr(buf, sizeof(buf), fs);
    st = removeTree(buf, sizeof(buf), r.path, 0);
  }
  if (st.ok()) revDropPath(s, r.path);   // path entry + children dropped too
  r.ctrl.status = st;
}

void StorageOwner::opCapacity(State& s, StorageReq& r) {
  if (!s.mounted) {
    r.ctrl.status = Status::Fail(Err::SdUnavailable, "sd not mounted");
    return;
  }
  r.totalOut = SD_MMC.totalBytes();
  uint64_t used = SD_MMC.usedBytes();
  r.freeOut = (r.totalOut > used) ? (r.totalOut - used) : 0;
  r.ctrl.status = Status::Ok();
}

// ---------------------------------------------------------------- dispatch

void StorageOwner::handle(StorageReq& r, void* ctx) {
  State& s = *static_cast<State*>(ctx);
  sweepLeases(s, nowMs());   // lazy idle-15s check on incoming request
  switch (r.op) {
    case StorageOp::Mount:       opMount(s, r); break;
    case StorageOp::Unmount:     opUnmount(s, r); break;
    case StorageOp::List:        opList(s, r); break;
    case StorageOp::Stat:        opStat(s, r); break;
    case StorageOp::Read:        opRead(s, r); break;
    case StorageOp::WriteBegin:  opWriteBegin(s, r); break;
    case StorageOp::WriteChunk:  opWriteChunk(s, r); break;
    case StorageOp::WriteCommit: opWriteCommit(s, r); break;
    case StorageOp::WriteAbort:  opWriteAbort(s, r); break;
    case StorageOp::Mkdir:       opMkdir(s, r); break;
    case StorageOp::Rename:      opRename(s, r); break;
    case StorageOp::Remove:      opRemove(s, r); break;
    case StorageOp::Capacity:    opCapacity(s, r); break;
    default:
      r.ctrl.status = Status::Fail(Err::InvalidArgument, "unknown op");
      break;
  }
  // Media error (ENODEV/EIO/EBADF) while mounted -> honest mount status:
  // drop mount + abort leases, so the next mount() ACTUALLY retries — not a
  // fake idempotent-Ok on top of a removed card.
  if (s.mounted && r.ctrl.status.code == Err::SdUnavailable &&
      r.op != StorageOp::Mount && r.op != StorageOp::Unmount) {
    for (auto& l : s.leases) {
      if (l.used) abortLease(s, l, "media error");
    }
    SD_MMC.end();
    s.mounted = false;
    ESP_LOGW(kTag, "sd media error -> forced unmount; mount() retries");
  }
}

// ------------------------------------------------------------------ facade

Status StorageOwner::start() {
  return task_.start(OwnerTaskConfig<StorageReq>{
      .name = "storage",
      .stackBytes = cfg::kStackStorage,
      .priority = cfg::kPrioStorage,
      .queueDepth = cfg::kStorageQueueDepth,
      .handle = &StorageOwner::handle,
      .ctx = &state_,
      .core = cfg::kCoreApp});
}

// StorageReq ~8 KiB — two on the caller's stack (in+out) would overflow an
// 8 KiB task (httpd). All facades allocate the pair on the heap, fill, free.
struct StorageReqPair { StorageReq in, o; };

Status StorageOwner::callReq(StorageReq& in, StorageReq& out) {
  return task_.call(in, out, cfg::kToolTimeoutMs);
}

StorageReqPair* StorageOwner::allocReq() {
  auto* rp = controlme::psramNew<StorageReqPair>();
  return rp;  // nullptr -> Internal at facade
}

bool StorageOwner::isMounted() {
  return state_.mounted;   // volatile byte — written only by the owner task
}

Status StorageOwner::mount() {
  StorageReqPair* rp = allocReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  rp->in.op = StorageOp::Mount;
  Status st = callReq(rp->in, rp->o);
  controlme::psramDelete(rp);
  return st;
}

Status StorageOwner::unmount() {
  StorageReqPair* rp = allocReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  rp->in.op = StorageOp::Unmount;
  Status st = callReq(rp->in, rp->o);
  controlme::psramDelete(rp);
  return st;
}

Status StorageOwner::list(const char* path, const char* cursor, uint8_t limit,
                          ListPage& out) {
  StorageReqPair* rp = allocReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  StorageReq& in = rp->in;
  in.op = StorageOp::List;
  copyStr(in.path, sizeof(in.path), path);
  copyStr(in.cursor, sizeof(in.cursor), cursor);
  in.limit = limit;
  Status st = callReq(in, rp->o);
  if (st.ok()) out = rp->o.page;
  controlme::psramDelete(rp);
  return st;
}

Status StorageOwner::stat(const char* path, FileStat& out) {
  StorageReqPair* rp = allocReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  StorageReq& in = rp->in;
  in.op = StorageOp::Stat;
  copyStr(in.path, sizeof(in.path), path);
  Status st = callReq(in, rp->o);
  if (st.ok()) out = rp->o.stat;
  controlme::psramDelete(rp);
  return st;
}

// Per-chunk pump: each iteration = one Read request (reopen+fseek, no
// lingering handle). sink.write runs in the CALLER's task — network sends
// don't hold the owner. Revision checked between chunks -> mixed content is
// never silently sent.
Status StorageOwner::read(const char* path, uint64_t offset, uint64_t length,
                          IByteSink& sink) {
  StorageReqPair* rp = allocReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  uint64_t pos = offset;
  uint64_t sent = 0;
  uint64_t baseRev = UINT64_MAX;
  Status st = Status::Ok();
  while (sent < length) {
    StorageReq& in = rp->in;
    StorageReq& o = rp->o;
    in = StorageReq{};   // reset input; output overwritten by handler
    in.op = StorageOp::Read;
    copyStr(in.path, sizeof(in.path), path);
    in.offset = pos;
    uint64_t remain = length - sent;
    in.length = remain > cfg::kIoChunk ? cfg::kIoChunk : remain;
    st = callReq(in, o);
    if (!st.ok()) break;
    if (baseRev == UINT64_MAX) {
      baseRev = o.revision;
    } else if (o.revision != baseRev) {
      st = Status::Fail(Err::RevisionConflict, "file changed during read");
      break;
    }
    if (o.readLen == 0) { st = Status::Ok(); break; }  // EOF
    st = sink.write(o.data, o.readLen);
    if (!st.ok()) break;
    pos += o.readLen;
    sent += o.readLen;
    if (o.readLen < in.length) { st = Status::Ok(); break; }
  }
  controlme::psramDelete(rp);
  return st;
}

Status StorageOwner::writeBegin(const char* path, bool replace,
                                uint64_t expectedRevision,
                                char uploadIdOut[cfg::kIdCap]) {
  StorageReqPair* rp = allocReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  StorageReq& in = rp->in;
  in.op = StorageOp::WriteBegin;
  copyStr(in.path, sizeof(in.path), path);
  in.replace = replace;
  in.expectedRevision = expectedRevision;
  Status st = callReq(in, rp->o);
  if (st.ok() && uploadIdOut) copyStr(uploadIdOut, cfg::kIdCap, rp->o.uploadId);
  controlme::psramDelete(rp);
  return st;
}

Status StorageOwner::writeChunk(const char* uploadId, uint64_t offset,
                                const uint8_t* data, size_t len,
                                uint64_t& nextOffsetOut) {
  if (len > cfg::kIoChunk) {
    return Status::Fail(Err::InvalidArgument, "chunk >4KiB");
  }
  StorageReqPair* rp = allocReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  StorageReq& in = rp->in;
  in.op = StorageOp::WriteChunk;
  copyStr(in.uploadId, sizeof(in.uploadId), uploadId);
  in.offset = offset;
  in.dataLen = (uint32_t)len;
  if (len) memcpy(in.data, data, len);
  Status st = callReq(in, rp->o);
  if (st.ok()) nextOffsetOut = rp->o.nextOffset;
  controlme::psramDelete(rp);
  return st;
}

Status StorageOwner::writeCommit(const char* uploadId, uint64_t totalBytes,
                                 uint64_t& revisionOut) {
  StorageReqPair* rp = allocReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  StorageReq& in = rp->in;
  in.op = StorageOp::WriteCommit;
  copyStr(in.uploadId, sizeof(in.uploadId), uploadId);
  in.totalBytes = totalBytes;
  Status st = callReq(in, rp->o);
  if (st.ok()) revisionOut = rp->o.revision;
  controlme::psramDelete(rp);
  return st;
}

Status StorageOwner::writeAbort(const char* uploadId) {
  StorageReqPair* rp = allocReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  rp->in.op = StorageOp::WriteAbort;
  copyStr(rp->in.uploadId, sizeof(rp->in.uploadId), uploadId);
  Status st = callReq(rp->in, rp->o);
  controlme::psramDelete(rp);
  return st;
}

Status StorageOwner::mkdir(const char* path, uint64_t& revisionOut) {
  StorageReqPair* rp = allocReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  rp->in.op = StorageOp::Mkdir;
  copyStr(rp->in.path, sizeof(rp->in.path), path);
  Status st = callReq(rp->in, rp->o);
  if (st.ok()) revisionOut = rp->o.revision;
  controlme::psramDelete(rp);
  return st;
}

Status StorageOwner::rename(const char* src, const char* dst,
                            uint64_t expectedRevision, uint64_t& revisionOut) {
  StorageReqPair* rp = allocReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  StorageReq& in = rp->in;
  in.op = StorageOp::Rename;
  copyStr(in.path, sizeof(in.path), src);
  copyStr(in.path2, sizeof(in.path2), dst);
  in.expectedRevision = expectedRevision;
  Status st = callReq(in, rp->o);
  if (st.ok()) revisionOut = rp->o.revision;
  controlme::psramDelete(rp);
  return st;
}

Status StorageOwner::remove(const char* path, bool recursive) {
  StorageReqPair* rp = allocReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  rp->in.op = StorageOp::Remove;
  copyStr(rp->in.path, sizeof(rp->in.path), path);
  rp->in.recursive = recursive;
  Status st = callReq(rp->in, rp->o);
  controlme::psramDelete(rp);
  return st;
}

Status StorageOwner::capacity(uint64_t& totalOut, uint64_t& freeOut) {
  StorageReqPair* rp = allocReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  rp->in.op = StorageOp::Capacity;
  Status st = callReq(rp->in, rp->o);
  if (st.ok()) {
    totalOut = rp->o.totalOut;
    freeOut = rp->o.freeOut;
  }
  controlme::psramDelete(rp);
  return st;
}

}  // namespace controlme
