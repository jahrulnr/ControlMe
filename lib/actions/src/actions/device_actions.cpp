// DeviceActions — implementation of the 14 docs/03 tools on top of the App
// ports.
//
// Deliberate limits (honest, not hidden):
//  * Tool results are produced <= ~kResultCap bytes to fit the ActionService
//    record (outcome <=1 KiB). Lists are emitted per element with a closing
//    reserve; elements that don't fit -> "truncated":true (+internal
//    nextCursor for sd_list) — the client resumes with a smaller limit.
//  * Echo fields (path/name/url/snippet) are escaped into bounded buffers —
//    per-field clipping is possible on huge names; JSON stays valid.
//  * sd_read: STREAMING base64 encode from storage chunks (B64Sink) — not a
//    file-sized buffer. length defaults to 512, clamped to kSdReadMax so the
//    result JSON stays <=1 KiB (b64 856 + head/tail).
//  * sd_write CHUNK: dataBase64 is decoded into a <=4 KiB heap buffer
//    (cfg::kIoChunk) per the docs/03 phase contract.
//  * image_read = metadata + downloadUrl; when the caller passes a
//    ToolAttach the staged JPEG is also streamed inline as base64 (the MCP
//    image content block) if it fits the attach budget — bigger frames stay
//    reachable via downloadUrl.
//
// Null port mapping (feature not installed): web_* -> ProviderUnavailable,
// others -> NotSupported. admissionOpen=false -> DeviceRebooting.
#include "actions/device_actions.h"
#include "common/psram_alloc.h"

#include <cJSON.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <memory>
#include <new>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>

#include "common/event.h"           // copyStr
#include "common/json_escape.h"     // jsonEscapeBounded
#include "transport/http_server.h"  // transport::jsonEscape (shared helper)

// app.h lives in src/ (composition root), not a lib — same resolution
// convention as owner_task.h in the storage/servo/camera libs.
#if __has_include("app.h")
#include "app.h"
#else
#include "../../../../src/app.h"
#endif

namespace {

using controlme::App;
using controlme::Err;
using controlme::FileStat;
using controlme::IByteSink;
using controlme::ImageMeta;
using controlme::IStorage;
using controlme::ListPage;
using controlme::Status;
using controlme::ToolAttach;
using controlme::WebSearchResult;
using controlme::copyStr;
namespace cfg = controlme::cfg;

// ---------------------------------------------------------------- budget

// Tool results target <=960 B (the 1 KiB ActionService record).
constexpr size_t kResultCap = 960;
constexpr size_t kTailReserve = 72;   // JSON list closing reserve
constexpr uint8_t kListLimitMax = 8;  // sd_list limit in the tool context
constexpr uint64_t kSdReadDefaultLen = 512;
constexpr uint64_t kSdReadMaxLen = 640;   // b64 ~856 -> total ~920 B
constexpr size_t kSdWriteChunkMax = cfg::kIoChunk;  // decoded <=4 KiB
constexpr size_t kFetchTextCap = 512;
constexpr size_t kEscCap = 200;   // echo field escape (path/url/dst)
constexpr size_t kEscNameCap = 120;
constexpr size_t kEscTitleCap = 96;
constexpr size_t kEscUrlCap = 200;
constexpr size_t kEscSnipCap = 200;
constexpr size_t kElCap = 512;    // max list element (names/urls clipped first)

struct JsonDoc {
  cJSON* root = nullptr;
  ~JsonDoc() {
    if (root) cJSON_Delete(root);
  }
};

Status parseArgs(const char* argsJson, JsonDoc& out) {
  if (!argsJson || !*argsJson) argsJson = "{}";
  out.root = cJSON_Parse(argsJson);
  if (!out.root) {
    return Status::Fail(Err::InvalidArgument, "arguments are not valid JSON");
  }
  return Status::Ok();
}

bool argStr(cJSON* r, const char* key, char* out, size_t cap) {
  cJSON* it = cJSON_GetObjectItemCaseSensitive(r, key);
  if (!cJSON_IsString(it) || !it->valuestring) return false;
  copyStr(out, cap, it->valuestring);
  return true;
}

bool argU64(cJSON* r, const char* key, uint64_t& out) {
  cJSON* it = cJSON_GetObjectItemCaseSensitive(r, key);
  if (!cJSON_IsNumber(it) || it->valuedouble < 0) return false;
  out = (uint64_t)it->valuedouble;
  return true;
}

bool argNum(cJSON* r, const char* key, double& out) {
  cJSON* it = cJSON_GetObjectItemCaseSensitive(r, key);
  if (!cJSON_IsNumber(it)) return false;
  out = it->valuedouble;
  return true;
}

// Bounded JSON builder: every append is verified; overflow -> fit=false
// and the caller decides (honest Internal error). Result sent in ONE write.
struct JsonBuf {
  char buf[kResultCap];
  size_t len = 0;
  bool fit = true;
  void raw(const char* s, size_t n) {
    if (!fit) return;
    if (len + n > sizeof(buf)) {
      fit = false;
      return;
    }
    memcpy(buf + len, s, n);
    len += n;
  }
  void raw(const char* s) { raw(s, strlen(s)); }
  void fmt(const char* f, ...) {
    if (!fit) return;
    va_list ap;
    va_start(ap, f);
    const int n = vsnprintf(buf + len, sizeof(buf) - len, f, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(buf) - len) {
      fit = false;
      return;
    }
    len += (size_t)n;
  }
  // A list element is only added when the `reserve` closing still fits —
  // JSON always ends up valid even when truncated.
  bool elem(const char* s, size_t n) {
    if (len + n + kTailReserve > sizeof(buf)) return false;
    raw(s, n);
    return fit;
  }
};

Status flushBuf(IByteSink& out, JsonBuf& jb) {
  if (!jb.fit) {
    return Status::Fail(Err::Internal, "tool result exceeds record budget");
  }
  return out.write((const uint8_t*)jb.buf, jb.len);
}

// ------------------------------------------------------------- base64

const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Incremental encoder 3 bytes -> 4 chars; flushes 48-char blocks to the
// downstream sink. Never holds more than 3 input bytes + 48 output chars.
class B64Sink : public IByteSink {
 public:
  explicit B64Sink(IByteSink& dst) : dst_(dst) {}
  Status write(const uint8_t* d, size_t n) override {
    raw_ += n;
    for (size_t i = 0; i < n; ++i) {
      trio_[trioLen_++] = d[i];
      if (trioLen_ == 3) {
        Status st = put4(kB64[trio_[0] >> 2],
                         kB64[((trio_[0] & 3) << 4) | (trio_[1] >> 4)],
                         kB64[((trio_[1] & 15) << 2) | (trio_[2] >> 6)],
                         kB64[trio_[2] & 63]);
        if (!st.ok()) return st;
        trioLen_ = 0;
      }
    }
    return Status::Ok();
  }
  Status finish() {
    if (trioLen_ == 1) {
      Status st = put4(kB64[trio_[0] >> 2], kB64[(trio_[0] & 3) << 4], '=', '=');
      if (!st.ok()) return st;
    } else if (trioLen_ == 2) {
      Status st = put4(kB64[trio_[0] >> 2],
                       kB64[((trio_[0] & 3) << 4) | (trio_[1] >> 4)],
                       kB64[(trio_[1] & 15) << 2], '=');
      if (!st.ok()) return st;
    }
    trioLen_ = 0;
    if (blkLen_) {
      Status st = dst_.write((const uint8_t*)blk_, blkLen_);
      blkLen_ = 0;
      if (!st.ok()) return st;
    }
    return Status::Ok();
  }
  uint64_t rawBytes() const { return raw_; }

 private:
  Status put4(char a, char b, char c, char d) {
    blk_[blkLen_++] = a;
    blk_[blkLen_++] = b;
    blk_[blkLen_++] = c;
    blk_[blkLen_++] = d;
    if (blkLen_ == sizeof(blk_)) {
      Status st = dst_.write((const uint8_t*)blk_, blkLen_);
      blkLen_ = 0;
      return st;
    }
    return Status::Ok();
  }
  IByteSink& dst_;
  uint64_t raw_ = 0;
  uint8_t trio_[3] = {};
  uint8_t trioLen_ = 0;
  char blk_[48] = {};
  uint8_t blkLen_ = 0;
};

// Decoder for sd_write CHUNK; '=' terminates; whitespace ignored.
Status b64Decode(const char* s, uint8_t* out, size_t cap, size_t& outLen) {
  outLen = 0;
  uint32_t acc = 0;
  uint8_t bits = 0;
  for (const char* p = s; *p; ++p) {
    const char c = *p;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    if (c == '=') break;
    int v;
    if (c >= 'A' && c <= 'Z') {
      v = c - 'A';
    } else if (c >= 'a' && c <= 'z') {
      v = c - 'a' + 26;
    } else if (c >= '0' && c <= '9') {
      v = c - '0' + 52;
    } else if (c == '+') {
      v = 62;
    } else if (c == '/') {
      v = 63;
    } else {
      return Status::Fail(Err::InvalidArgument, "invalid dataBase64 character");
    }
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (outLen >= cap) {
        return Status::Fail(Err::InvalidArgument,
                            "decoded chunk exceeds 4 KiB");
      }
      out[outLen++] = (uint8_t)((acc >> bits) & 0xFF);
    }
  }
  return Status::Ok();
}

// Text sink -> bounded JSON escape: excess bytes are dropped and marked
// truncated (final result stays valid JSON); raw byte total still counted.
class EscTextSink : public IByteSink {
 public:
  EscTextSink(char* dst, size_t cap) : dst_(dst), cap_(cap) {}
  Status write(const uint8_t* d, size_t n) override {
    total_ += n;
    for (size_t i = 0; i < n; ++i) {
      const char c = (char)d[i];
      char rep[8];
      size_t rn = 0;
      switch (c) {
        case '"': rep[rn++] = '\\'; rep[rn++] = '"'; break;
        case '\\': rep[rn++] = '\\'; rep[rn++] = '\\'; break;
        case '\n': rep[rn++] = '\\'; rep[rn++] = 'n'; break;
        case '\r': rep[rn++] = '\\'; rep[rn++] = 'r'; break;
        case '\t': rep[rn++] = '\\'; rep[rn++] = 't'; break;
        default:
          if ((uint8_t)c < 0x20) {
            rn = (size_t)snprintf(rep, sizeof(rep), "\\u%04x", c);
          } else {
            rep[rn++] = c;
          }
      }
      if (len_ + rn >= cap_) {
        truncated_ = true;
        continue;  // absorb the rest so meta stays complete; marked truncated
      }
      memcpy(dst_ + len_, rep, rn);
      len_ += rn;
    }
    return Status::Ok();
  }
  bool truncated() const { return truncated_; }
  uint64_t total() const { return total_; }
  size_t len() const { return len_; }

 private:
  char* dst_;
  size_t cap_;
  size_t len_ = 0;
  uint64_t total_ = 0;
  bool truncated_ = false;
};

// ------------------------------------------------------------- guards

IStorage* storageOr(App& app, Status& st) {
  if (!app.storage) {
    st = Status::Fail(Err::NotSupported, "storage not installed");
    return nullptr;
  }
  return app.storage;
}

// Absolute staging URL when wifi is up (httpBaseUrl set), else the relative
// path — same self-describing pattern as log_read's downloadUrl.
void fmtImageUrl(App& app, const char* imageId, char* out, size_t cap) {
  snprintf(out, cap, "%s/api/v1/images/%s", app.httpBaseUrl, imageId);
}

// Fill att link fields and stream the staged JPEG as inline base64 when it
// fits the caller's budget — shared by camera_capture and image_read. The
// link fields are set even when the payload is skipped (the URL still works).
// pin holds the slot across the stream (TTL/concurrent-capture race) — same
// pattern as GET /api/v1/images/*.
void attachImage(App& app, const char* url, ImageMeta& meta, ToolAttach* att) {
  copyStr(att->uri, sizeof(att->uri), url);
  copyStr(att->name, sizeof(att->name), meta.imageId);
  copyStr(att->mime, sizeof(att->mime), meta.mime);
  const size_t b64 = ((size_t)meta.bytes + 2) / 3 * 4;
  if (att->data && b64 > 0 && b64 <= att->dataCap) {
    Status pst = app.camera->pin(meta.imageId);
    if (pst.ok()) {
      B64Sink enc(*att->data);
      Status rst = app.camera->readImage(meta.imageId, enc, meta);
      app.camera->release(meta.imageId);
      if (rst.ok()) rst = enc.finish();
      att->hasData = rst.ok();
    }
  }
}

const char* imageMimeOf(const char* path) {
  const char* dot = strrchr(path, '.');
  if (!dot) return nullptr;
  ++dot;
  if (!strcasecmp(dot, "jpg") || !strcasecmp(dot, "jpeg")) return "image/jpeg";
  if (!strcasecmp(dot, "png")) return "image/png";
  if (!strcasecmp(dot, "gif")) return "image/gif";
  if (!strcasecmp(dot, "bmp")) return "image/bmp";
  if (!strcasecmp(dot, "webp")) return "image/webp";
  return nullptr;
}

// ------------------------------------------------------------- tools

using ToolFn = Status (*)(App&, cJSON*, IByteSink&, ToolAttach*);

Status tDeviceStatus(App& app, cJSON*, IByteSink& out, ToolAttach*) {
  JsonBuf jb;
  jb.fmt(
      "{\"status\":\"ok\",\"bootId\":\"%s\",\"uptimeMs\":%llu,"
      "\"heapFree\":%u,\"heapMinFree\":%u,\"heapLargestBlock\":%u,"
      "\"lastEventSequence\":%lu,\"sdMounted\":%s,\"wifiConnected\":%s,"
      "\"wallSynced\":%s}",
      app.bootId, (unsigned long long)(app.clock ? app.clock->monoMs() : 0),
      (unsigned)esp_get_free_heap_size(),
      (unsigned)esp_get_minimum_free_heap_size(),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                 MALLOC_CAP_8BIT),
      (unsigned long)app.lastEventSequence,
      (app.storage && app.storage->isMounted()) ? "true" : "false",
      app.wifiConnected ? "true" : "false",
      (app.clock && app.clock->wallSynced()) ? "true" : "false");
  return flushBuf(out, jb);
}

Status tDeviceReboot(App& app, cJSON* a, IByteSink& out, ToolAttach*) {
  if (!app.rebootCtl) {
    return Status::Fail(Err::NotSupported, "reboot ctl not installed");
  }
  if (app.rebootCtl->isRebooting()) {
    return Status::Fail(Err::DeviceRebooting, "reboot already in progress");
  }
  char reason[64] = {};
  argStr(a, "reason", reason, sizeof(reason));
  // ACCEPTED then reboot (docs/03) — the supervisor runs the shutdown
  // sequence; this is NOT a claim that reboot already happened.
  Status st = app.rebootCtl->requestReboot(reason[0] ? reason : nullptr);
  if (!st.ok()) return st;
  JsonBuf jb;
  jb.raw("{\"status\":\"ACCEPTED\",\"rebooting\":true}");
  return flushBuf(out, jb);
}

Status tSdList(App& app, cJSON* a, IByteSink& out, ToolAttach*) {
  Status st;
  IStorage* s = storageOr(app, st);
  if (!s) return st;
  char path[cfg::kPathCap] = {};
  if (!argStr(a, "path", path, sizeof(path)) || !path[0]) {
    return Status::Fail(Err::InvalidArgument, "path argument required");
  }
  char cursor[cfg::kIdCap] = {};
  argStr(a, "cursor", cursor, sizeof(cursor));
  uint64_t lim = kListLimitMax;
  argU64(a, "limit", lim);
  if (lim == 0 || lim > kListLimitMax) lim = kListLimitMax;
  // ListPage ~3 KiB — heap so the 6 KiB worker stack isn't squeezed.
  auto pg = controlme::psramMake<ListPage>();
  if (!pg) return Status::Fail(Err::Internal, "list page alloc");
  st = s->list(path, cursor, (uint8_t)lim, *pg);
  if (!st.ok()) return st;

  JsonBuf jb;
  char esc[kEscCap];
  transport::jsonEscape(path, esc, sizeof(esc));
  jb.fmt("{\"path\":\"%s\",\"entries\":[", esc);
  uint8_t emitted = 0;
  bool truncated = false;
  for (uint8_t i = 0; i < pg->count; ++i) {
    FileStat& fe = pg->entries[i];
    const char* slash = strrchr(fe.path, '/');
    const char* name = slash ? slash + 1 : fe.path;
    char en[kEscNameCap];
    transport::jsonEscape(name, en, sizeof(en));
    char el[kElCap];
    const int en2 = snprintf(el, sizeof(el),
                             "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%llu,"
                             "\"rev\":%llu}",
                             emitted ? "," : "", en,
                             fe.isDir ? "true" : "false",
                             (unsigned long long)fe.size,
                             (unsigned long long)fe.revision);
    if (en2 < 0 || (size_t)en2 >= sizeof(el) || !jb.elem(el, (size_t)en2)) {
      truncated = true;  // element doesn't fit whole — no partial elements
      break;
    }
    ++emitted;
  }
  if (truncated) {
    // Storage cursor = internal decimal index (storage_owner.h): resume
    // right after the last emitted entry — honest, no skips.
    const uint64_t resume = (uint64_t)strtoull(cursor, nullptr, 10) + emitted;
    jb.fmt("],\"nextCursor\":\"%llu\",\"truncated\":true,\"count\":%u}",
           (unsigned long long)resume, (unsigned)emitted);
  } else {
    char ec[cfg::kIdCap * 2];
    transport::jsonEscape(pg->nextCursor, ec, sizeof(ec));
    jb.fmt("],\"nextCursor\":\"%s\",\"truncated\":false,\"count\":%u}", ec,
           (unsigned)emitted);
  }
  return flushBuf(out, jb);
}

Status tSdStat(App& app, cJSON* a, IByteSink& out, ToolAttach*) {
  Status st;
  IStorage* s = storageOr(app, st);
  if (!s) return st;
  char path[cfg::kPathCap] = {};
  if (!argStr(a, "path", path, sizeof(path)) || !path[0]) {
    return Status::Fail(Err::InvalidArgument, "path argument required");
  }
  FileStat fs{};
  st = s->stat(path, fs);
  if (!st.ok()) return st;
  JsonBuf jb;
  char esc[kEscCap];
  transport::jsonEscape(path, esc, sizeof(esc));
  jb.fmt("{\"path\":\"%s\",\"size\":%llu,\"isDir\":%s,\"mtimeMs\":%llu,"
         "\"revision\":%llu}",
         esc, (unsigned long long)fs.size, fs.isDir ? "true" : "false",
         (unsigned long long)fs.mtimeMs, (unsigned long long)fs.revision);
  return flushBuf(out, jb);
}

Status tSdRead(App& app, cJSON* a, IByteSink& out, ToolAttach*) {
  Status st;
  IStorage* s = storageOr(app, st);
  if (!s) return st;
  char path[cfg::kPathCap] = {};
  if (!argStr(a, "path", path, sizeof(path)) || !path[0]) {
    return Status::Fail(Err::InvalidArgument, "path argument required");
  }
  uint64_t offset = 0;
  uint64_t len = kSdReadDefaultLen;
  argU64(a, "offset", offset);
  argU64(a, "length", len);
  if (len == 0) len = kSdReadDefaultLen;
  if (len > kSdReadMaxLen) len = kSdReadMaxLen;  // clamp so JSON stays <=1 KiB
  FileStat fs{};
  st = s->stat(path, fs);
  if (!st.ok()) return st;
  if (fs.isDir) {
    return Status::Fail(Err::InvalidArgument, "path is a directory");
  }
  if (offset > fs.size) {
    return Status::Fail(Err::InvalidArgument, "offset exceeds file size");
  }
  uint64_t want = fs.size - offset;
  if (len < want) want = len;
  char head[96];
  const int hn = snprintf(head, sizeof(head),
                          "{\"offset\":%llu,\"dataBase64\":\"",
                          (unsigned long long)offset);
  st = out.write((const uint8_t*)head, (size_t)hn);
  if (!st.ok()) return st;
  B64Sink enc(out);
  st = s->read(path, offset, want, enc);
  if (st.ok()) st = enc.finish();
  if (!st.ok()) return st;
  const uint64_t got = enc.rawBytes();
  char tail[112];
  const int tn = snprintf(tail, sizeof(tail),
                          "\",\"bytes\":%llu,\"nextOffset\":%llu,\"eof\":%s}",
                          (unsigned long long)got,
                          (unsigned long long)(offset + got),
                          (offset + got) >= fs.size ? "true" : "false");
  return out.write((const uint8_t*)tail, (size_t)tn);
}

Status tSdMkdir(App& app, cJSON* a, IByteSink& out, ToolAttach*) {
  Status st;
  IStorage* s = storageOr(app, st);
  if (!s) return st;
  char path[cfg::kPathCap] = {};
  if (!argStr(a, "path", path, sizeof(path)) || !path[0]) {
    return Status::Fail(Err::InvalidArgument, "path argument required");
  }
  uint64_t rev = 0;
  st = s->mkdir(path, rev);
  if (!st.ok()) return st;
  JsonBuf jb;
  char esc[kEscCap];
  transport::jsonEscape(path, esc, sizeof(esc));
  jb.fmt("{\"path\":\"%s\",\"revision\":%llu}", esc,
         (unsigned long long)rev);
  return flushBuf(out, jb);
}

Status tSdRename(App& app, cJSON* a, IByteSink& out, ToolAttach*) {
  Status st;
  IStorage* s = storageOr(app, st);
  if (!s) return st;
  char src[cfg::kPathCap] = {};
  char dst[cfg::kPathCap] = {};
  if (!argStr(a, "src", src, sizeof(src)) || !src[0] ||
      !argStr(a, "dst", dst, sizeof(dst)) || !dst[0]) {
    return Status::Fail(Err::InvalidArgument, "src and dst arguments required");
  }
  uint64_t expectedRev = 0;
  argU64(a, "expectedRevision", expectedRev);
  uint64_t rev = 0;
  st = s->rename(src, dst, expectedRev, rev);
  if (!st.ok()) return st;
  JsonBuf jb;
  char escS[kEscCap], escD[kEscCap];
  transport::jsonEscape(src, escS, sizeof(escS));
  transport::jsonEscape(dst, escD, sizeof(escD));
  jb.fmt("{\"src\":\"%s\",\"dst\":\"%s\",\"revision\":%llu}", escS, escD,
         (unsigned long long)rev);
  return flushBuf(out, jb);
}

Status tSdDelete(App& app, cJSON* a, IByteSink& out, ToolAttach*) {
  Status st;
  IStorage* s = storageOr(app, st);
  if (!s) return st;
  char path[cfg::kPathCap] = {};
  if (!argStr(a, "path", path, sizeof(path)) || !path[0]) {
    return Status::Fail(Err::InvalidArgument, "path argument required");
  }
  bool recursive = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a, "recursive"));
  st = s->remove(path, recursive);
  if (!st.ok()) return st;
  JsonBuf jb;
  char esc[kEscCap];
  transport::jsonEscape(path, esc, sizeof(esc));
  jb.fmt("{\"path\":\"%s\",\"deleted\":true}", esc);
  return flushBuf(out, jb);
}

// sd_write — four phases (docs/03): BEGIN/CHUNK/COMMIT/ABORT via IStorage.
Status tSdWrite(App& app, cJSON* a, IByteSink& out, ToolAttach*) {
  Status st;
  IStorage* s = storageOr(app, st);
  if (!s) return st;
  char phase[8] = {};
  if (!argStr(a, "phase", phase, sizeof(phase))) {
    return Status::Fail(Err::InvalidArgument,
                        "phase required: BEGIN|CHUNK|COMMIT|ABORT");
  }
  if (strcmp(phase, "BEGIN") == 0) {
    char path[cfg::kPathCap] = {};
    if (!argStr(a, "path", path, sizeof(path)) || !path[0]) {
      return Status::Fail(Err::InvalidArgument, "path argument required");
    }
    char mode[12] = {};
    bool replace = false;
    if (argStr(a, "mode", mode, sizeof(mode))) {
      if (strcmp(mode, "replace") == 0) {
        replace = true;
      } else if (strcmp(mode, "create") != 0) {
        return Status::Fail(Err::InvalidArgument,
                            "mode must be create|replace");
      }
    }
    uint64_t expectedRev = 0;
    const bool hasRev = argU64(a, "expectedRevision", expectedRev);
    if (replace && !hasRev) {
      return Status::Fail(Err::InvalidArgument,
                          "expectedRevision required for mode=replace");
    }
    char uploadId[cfg::kIdCap] = {};
    st = s->writeBegin(path, replace, expectedRev, uploadId);
    if (!st.ok()) return st;
    JsonBuf jb;
    char esc[cfg::kIdCap * 2];
    transport::jsonEscape(uploadId, esc, sizeof(esc));
    jb.fmt("{\"uploadId\":\"%s\",\"nextOffset\":0}", esc);
    return flushBuf(out, jb);
  }
  if (strcmp(phase, "CHUNK") == 0) {
    char uploadId[cfg::kIdCap] = {};
    if (!argStr(a, "uploadId", uploadId, sizeof(uploadId)) || !uploadId[0]) {
      return Status::Fail(Err::InvalidArgument, "uploadId argument required");
    }
    uint64_t offset = 0;
    if (!argU64(a, "offset", offset)) {
      return Status::Fail(Err::InvalidArgument, "offset argument required");
    }
    cJSON* data = cJSON_GetObjectItemCaseSensitive(a, "dataBase64");
    if (!cJSON_IsString(data) || !data->valuestring) {
      return Status::Fail(Err::InvalidArgument, "dataBase64 argument required");
    }
    // 4 KiB decode buffer on the heap — the 6 KiB worker stack isn't roomy enough.
    auto buf = controlme::psramArr<uint8_t>(kSdWriteChunkMax);
    if (!buf) return Status::Fail(Err::Internal, "chunk buffer alloc");
    size_t n = 0;
    st = b64Decode(data->valuestring, buf.get(), kSdWriteChunkMax, n);
    if (!st.ok()) return st;
    uint64_t next = 0;
    st = s->writeChunk(uploadId, offset, buf.get(), n, next);
    if (!st.ok()) return st;
    JsonBuf jb;
    jb.fmt("{\"nextOffset\":%llu}", (unsigned long long)next);
    return flushBuf(out, jb);
  }
  if (strcmp(phase, "COMMIT") == 0) {
    char uploadId[cfg::kIdCap] = {};
    if (!argStr(a, "uploadId", uploadId, sizeof(uploadId)) || !uploadId[0]) {
      return Status::Fail(Err::InvalidArgument, "uploadId argument required");
    }
    uint64_t total = 0;
    if (!argU64(a, "totalBytes", total)) {
      return Status::Fail(Err::InvalidArgument, "totalBytes argument required");
    }
    uint64_t rev = 0;
    st = s->writeCommit(uploadId, total, rev);
    if (!st.ok()) return st;
    JsonBuf jb;
    jb.fmt("{\"revision\":%llu}", (unsigned long long)rev);
    return flushBuf(out, jb);
  }
  if (strcmp(phase, "ABORT") == 0) {
    char uploadId[cfg::kIdCap] = {};
    if (!argStr(a, "uploadId", uploadId, sizeof(uploadId)) || !uploadId[0]) {
      return Status::Fail(Err::InvalidArgument, "uploadId argument required");
    }
    st = s->writeAbort(uploadId);
    if (!st.ok()) return st;
    JsonBuf jb;
    jb.raw("{\"aborted\":true}");
    return flushBuf(out, jb);
  }
  return Status::Fail(Err::InvalidArgument,
                      "phase must be BEGIN|CHUNK|COMMIT|ABORT");
}

Status tServoSet(App& app, cJSON* a, IByteSink& out, ToolAttach*) {
  if (!app.servo) {
    return Status::Fail(Err::NotSupported, "servo not installed");
  }
  char id[16] = {};
  double deg = 0;
  if (!argStr(a, "servoId", id, sizeof(id)) || !id[0] ||
      !argNum(a, "angleDeg", deg)) {
    return Status::Fail(Err::InvalidArgument,
                        "servoId and angleDeg arguments required");
  }
  Status st = app.servo->setAngle(id, (float)deg);
  if (!st.ok()) return st;
  // COMMAND_APPLIED = signal applied, NOT a claim the position was
  // reached (docs/03).
  JsonBuf jb;
  char esc[64];
  transport::jsonEscape(id, esc, sizeof(esc));
  jb.fmt("{\"servoId\":\"%s\",\"angleDeg\":%g,\"status\":\"COMMAND_APPLIED\"}",
         esc, deg);
  return flushBuf(out, jb);
}

Status tCameraCapture(App& app, cJSON*, IByteSink& out, ToolAttach* att) {
  if (!app.camera) {
    return Status::Fail(Err::NotSupported, "camera not installed");
  }
  if (!app.camera->isReady()) {
    return Status::Fail(Err::Internal, "camera not ready");
  }
  ImageMeta meta{};
  Status st = app.camera->capture(meta);
  if (!st.ok()) return st;
  char url[192];
  fmtImageUrl(app, meta.imageId, url, sizeof(url));
  JsonBuf jb;
  char escI[cfg::kIdCap * 2], escM[64], escU[200];
  transport::jsonEscape(meta.imageId, escI, sizeof(escI));
  transport::jsonEscape(meta.mime, escM, sizeof(escM));
  transport::jsonEscape(url, escU, sizeof(escU));
  jb.fmt("{\"imageId\":\"%s\",\"mime\":\"%s\",\"bytes\":%lu,\"width\":%u,"
         "\"height\":%u,\"downloadUrl\":\"%s\"}",
         escI, escM, (unsigned long)meta.bytes, (unsigned)meta.width,
         (unsigned)meta.height, escU);
  // "Capture & look" in one call: the fresh frame also comes back inline
  // when it fits — no second image_read needed.
  if (att) attachImage(app, url, meta, att);
  return flushBuf(out, jb);
}

// image_read — small metadata/ref only (docs/03): staging imageId OR an
// SD path; NOT image bytes (bytes = multimodal content part via another
// adapter).
Status tImageRead(App& app, cJSON* a, IByteSink& out, ToolAttach* att) {
  char imageId[cfg::kIdCap] = {};
  char path[cfg::kPathCap] = {};
  argStr(a, "imageId", imageId, sizeof(imageId));
  argStr(a, "path", path, sizeof(path));
  if (imageId[0]) {
    if (!app.camera) {
      return Status::Fail(Err::NotSupported, "camera not installed");
    }
    ImageMeta meta{};
    Status st = app.camera->stat(imageId, meta);
    if (!st.ok()) return st;
    if (strncmp(meta.mime, "image/", 6) != 0) {
      return Status::Fail(Err::NotSupported, "unknown image mime");
    }
    char url[192];
    fmtImageUrl(app, meta.imageId, url, sizeof(url));
    JsonBuf jb;
    char escI[cfg::kIdCap * 2], escM[64], escU[200];
    transport::jsonEscape(meta.imageId, escI, sizeof(escI));
    transport::jsonEscape(meta.mime, escM, sizeof(escM));
    transport::jsonEscape(url, escU, sizeof(escU));
    jb.fmt("{\"imageId\":\"%s\",\"mime\":\"%s\",\"bytes\":%lu,\"width\":%u,"
           "\"height\":%u,\"pinned\":%s,\"source\":\"staging\","
           "\"downloadUrl\":\"%s\"}",
           escI, escM, (unsigned long)meta.bytes, (unsigned)meta.width,
           (unsigned)meta.height, meta.pinned ? "true" : "false", escU);
    if (att) attachImage(app, url, meta, att);
    return flushBuf(out, jb);
  }
  if (!path[0]) {
    return Status::Fail(Err::InvalidArgument, "imageId or path required");
  }
  Status st;
  IStorage* s = storageOr(app, st);
  if (!s) return st;
  FileStat fs{};
  st = s->stat(path, fs);
  if (!st.ok()) return st;
  if (fs.isDir) {
    return Status::Fail(Err::InvalidArgument, "path is a directory");
  }
  const char* mime = imageMimeOf(path);
  if (!mime) {
    return Status::Fail(Err::NotSupported, "unknown image mime");
  }
  // width/height unavailable from FS metadata — honestly omitted.
  JsonBuf jb;
  char esc[kEscCap];
  transport::jsonEscape(path, esc, sizeof(esc));
  jb.fmt("{\"path\":\"%s\",\"mime\":\"%s\",\"bytes\":%llu,\"revision\":%llu,"
         "\"source\":\"sd\"}",
         esc, mime, (unsigned long long)fs.size,
         (unsigned long long)fs.revision);
  return flushBuf(out, jb);
}

// log_read — DeviceLog LittleFS (current/previous boot). The result
// carries downloadUrl for a full fetch outside MCP (a ~<=1 KiB tool result
// doesn't fit a ~100 KiB file).
Status tLogRead(App& app, cJSON* a, IByteSink& out, ToolAttach*) {
  if (!app.logs) {
    return Status::Fail(Err::NotSupported, "log not installed");
  }
  char which[16] = {};
  if (!argStr(a, "which", which, sizeof(which))) {
    return Status::Fail(Err::InvalidArgument, "which required (current|previous)");
  }
  controlme::LogFile wf;
  if (strcmp(which, "current") == 0) wf = controlme::LogFile::Current;
  else if (strcmp(which, "previous") == 0) wf = controlme::LogFile::Previous;
  else return Status::Fail(Err::InvalidArgument, "which must be current|previous");
  uint64_t off = 0;
  argU64(a, "offset", off);
  uint64_t len = cfg::kIoChunk;
  argU64(a, "length", len);
  if (len == 0) len = 1;
  if (len > cfg::kIoChunk) len = cfg::kIoChunk;

  // Raw PSRAM buffer (Timeout rule: dst may leak — don't free now since
  // the owner may still be writing).
  auto raw = controlme::psramArr<char>((size_t)len);
  if (!raw) return Status::Fail(Err::Internal, "log_read alloc");
  controlme::LogReadResult res{};
  Status st = app.logs->read(wf, (size_t)off, raw.get(), (size_t)len,
                             cfg::kToolTimeoutMs, res);
  if (!st.ok()) {
    if (st.code == Err::Timeout) {
      ESP_LOGW("actions", "log_read timeout — %u B buffer left leaked",
               (unsigned)len);
      raw.release();
    }
    return st;
  }

  // Result in a PSRAM kMcpToolResultCap buffer (NOT the 960 B JsonBuf —
  // this tool carries a larger log text payload).
  auto jb = controlme::psramArr<char>(cfg::kMcpToolResultCap);
  if (!jb) return Status::Fail(Err::Internal, "log_read result alloc");
  char url[96];
  if (app.httpBaseUrl[0]) {
    snprintf(url, sizeof(url), "%s/api/v1/logs/%s", app.httpBaseUrl, which);
  } else {
    snprintf(url, sizeof(url), "/api/v1/logs/%s", which);
  }
  char* buf = jb.get();
  const size_t cap = cfg::kMcpToolResultCap;
  int hn = snprintf(buf, cap,
                    "{\"which\":\"%s\",\"size\":%u,\"offset\":%u,"
                    "\"text\":\"",
                    which, (unsigned)res.size, (unsigned)off);
  if (hn <= 0 || (size_t)hn >= cap) {
    return Status::Fail(Err::Internal, "log_read head overflow");
  }
  constexpr size_t kTail = 160;  // closing ",nextOffset..downloadUrl..} + margin
  // McpServer escapes the tool result ONCE MORE into the kMcpRespCap
  // envelope — quote-dense logs can grow ~2x; bound the budget so it always
  // fits.
  size_t escCap = cap - (size_t)hn - kTail;
  const size_t envCap = (cfg::kMcpRespCap - 512) / 2;
  if (escCap > envCap) escCap = envCap;
  size_t escLen = 0;
  const size_t consumed =
      controlme::jsonEscapeBounded(raw.get(), res.outLen, buf + hn, escCap,
                                   &escLen, res.eof);
  const size_t nextOff = off + consumed;
  const bool eof = res.eof && consumed == res.outLen;
  const size_t el = strlen(buf);
  int tn = snprintf(buf + el, cap - el,
                    "\",\"nextOffset\":%u,\"eof\":%s,\"generation\":%lu,"
                    "\"downloadUrl\":\"%s\"}",
                    (unsigned)nextOff, eof ? "true" : "false",
                    (unsigned long)res.generation, url);
  if (tn <= 0 || el + (size_t)tn >= cap) {
    return Status::Fail(Err::Internal, "log_read tail overflow");
  }
  return out.write((const uint8_t*)buf, el + (size_t)tn);
}

Status tWebSearch(App& app, cJSON* a, IByteSink& out, ToolAttach*) {
  if (!app.webSearch || !app.webSearch->isConfigured()) {
    return Status::Fail(Err::ProviderUnavailable,
                        "web_search not configured");
  }
  char query[192] = {};
  if (!argStr(a, "query", query, sizeof(query)) || !query[0]) {
    return Status::Fail(Err::InvalidArgument, "query argument required");
  }
  uint64_t lim = 4;
  argU64(a, "limit", lim);
  if (lim == 0) lim = 4;
  if (lim > 8) lim = 8;  // WebSearchResult.items max 8 (port)
  // ~5.4 KiB on the heap — too big for the worker stack.
  auto res = controlme::psramMake<WebSearchResult>();
  if (!res) return Status::Fail(Err::Internal, "search result alloc");
  Status st = app.webSearch->search(query, (uint8_t)lim, *res);
  if (!st.ok()) return st;
  JsonBuf jb;
  jb.raw("{\"results\":[");
  uint8_t emitted = 0;
  bool truncated = false;
  for (uint8_t i = 0; i < res->count; ++i) {
    char et[kEscTitleCap], eu[kEscUrlCap], es[kEscSnipCap];
    transport::jsonEscape(res->items[i].title, et, sizeof(et));
    transport::jsonEscape(res->items[i].url, eu, sizeof(eu));
    transport::jsonEscape(res->items[i].snippet, es, sizeof(es));
    char el[kElCap];
    const int en = snprintf(el, sizeof(el),
                            "%s{\"t\":\"%s\",\"u\":\"%s\",\"s\":\"%s\"}",
                            emitted ? "," : "", et, eu, es);
    if (en < 0 || (size_t)en >= sizeof(el) || !jb.elem(el, (size_t)en)) {
      truncated = true;
      break;
    }
    ++emitted;
  }
  jb.fmt("],\"count\":%u,\"truncated\":%s}", (unsigned)emitted,
         (truncated || emitted < res->count) ? "true" : "false");
  return flushBuf(out, jb);
}

Status tWebFetch(App& app, cJSON* a, IByteSink& out, ToolAttach*) {
  if (!app.webFetch) {
    return Status::Fail(Err::ProviderUnavailable,
                        "web_fetch not installed");
  }
  char url[256] = {};
  if (!argStr(a, "url", url, sizeof(url)) || !url[0]) {
    return Status::Fail(Err::InvalidArgument, "url argument required");
  }
  // Text body accumulated escaped <=kFetchTextCap; meta is filled up
  // front (port) so the final result stays complete even when the body is
  // cut -> truncated.
  char text[kFetchTextCap];
  EscTextSink sink(text, sizeof(text));
  controlme::WebFetchMeta meta{};
  Status st = app.webFetch->fetch(url, meta, sink);
  if (!st.ok()) return st;
  text[sink.len()] = '\0';
  JsonBuf jb;
  char escU[kEscCap], escM[80];
  transport::jsonEscape(meta.finalUrl, escU, sizeof(escU));
  transport::jsonEscape(meta.mime, escM, sizeof(escM));
  jb.fmt("{\"status\":%d,\"url\":\"%s\",\"mime\":\"%s\",\"text\":\"%s\","
         "\"textBytes\":%llu,\"truncated\":%s}",
         meta.httpStatus, escU, escM, text,
         (unsigned long long)sink.total(),
         sink.truncated() ? "true" : "false");
  return flushBuf(out, jb);
}

// ------------------------------------------------------------- katalog

struct ToolEntry {
  const char* name;
  const char* description;
  const char* schema;
  ToolFn fn;
};

// Compact JSON schemas per the docs/03 Arguments column (one line per
// tool). description is required on the OpenAI/OpenRouter wire
// (function.description).
constexpr ToolEntry kTools[] = {
    {"device_status", "Read device heap/wifi/uptime status",
     "{\"type\":\"object\",\"properties\":{}}", &tDeviceStatus},
    {"device_reboot", "Reboot the ESP32 device",
     "{\"type\":\"object\",\"properties\":{\"reason\":{\"type\":\"string\"}}}",
     &tDeviceReboot},
    {"sd_list", "List files on the SD card path",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
     "\"cursor\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\","
     "\"minimum\":1,\"maximum\":8}},\"required\":[\"path\"]}",
     &tSdList},
    {"sd_stat", "Stat a path on the SD card",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
     "\"required\":[\"path\"]}",
     &tSdStat},
    {"sd_read", "Read a byte range from an SD file",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
     "\"offset\":{\"type\":\"integer\",\"minimum\":0},\"length\":{\"type\":"
     "\"integer\",\"minimum\":1,\"maximum\":640}},\"required\":[\"path\"]}",
     &tSdRead},
    {"sd_mkdir", "Create a directory on the SD card",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
     "\"required\":[\"path\"]}",
     &tSdMkdir},
    {"sd_rename", "Rename or move an SD path",
     "{\"type\":\"object\",\"properties\":{\"src\":{\"type\":\"string\"},"
     "\"dst\":{\"type\":\"string\"},\"expectedRevision\":{\"type\":\"integer\""
     "}},\"required\":[\"src\",\"dst\"]}",
     &tSdRename},
    {"sd_delete", "Delete an SD file or directory",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
     "\"recursive\":{\"type\":\"boolean\"}},\"required\":[\"path\"]}",
     &tSdDelete},
    {"sd_write", "Write a file to SD via BEGIN/CHUNK/COMMIT/ABORT",
     "{\"type\":\"object\",\"properties\":{\"phase\":{\"type\":\"string\","
     "\"enum\":[\"BEGIN\",\"CHUNK\",\"COMMIT\",\"ABORT\"]},\"path\":{\"type\":"
     "\"string\"},\"mode\":{\"type\":\"string\",\"enum\":[\"create\","
     "\"replace\"]},\"expectedRevision\":{\"type\":\"integer\"},\"uploadId\":"
     "{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},\"dataBase64\":"
     "{\"type\":\"string\"},\"totalBytes\":{\"type\":\"integer\"}},"
     "\"required\":[\"phase\"]}",
     &tSdWrite},
    {"servo_set", "Set a servo channel angle in degrees",
     "{\"type\":\"object\",\"properties\":{\"servoId\":{\"type\":\"string\"},"
     "\"angleDeg\":{\"type\":\"number\"}},\"required\":[\"servoId\","
     "\"angleDeg\"]}",
     &tServoSet},
    {"camera_capture", "Capture a still JPEG into staging; returns metadata + downloadUrl + the image inline when it fits",
     "{\"type\":\"object\",\"properties\":{}}", &tCameraCapture},
    {"image_read", "Read a staged image by id or SD path; returns metadata + downloadUrl; staged images also come back inline when they fit",
     "{\"type\":\"object\",\"properties\":{\"imageId\":{\"type\":\"string\"},"
     "\"path\":{\"type\":\"string\"}}}",
     &tImageRead},
    {"log_read", "Read the device log stored in LittleFS (current boot or previous boot). Paginate with offset/nextOffset; downloadUrl serves the whole file over HTTP",
     "{\"type\":\"object\",\"properties\":{\"which\":{\"type\":\"string\",\"enum\":[\"current\",\"previous\"]},\"offset\":{\"type\":\"integer\",\"minimum\":0},\"length\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4096}},\"required\":[\"which\"]}",
     &tLogRead},
    {"web_search", "Search the web for a query",
     "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},"
     "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8}},"
     "\"required\":[\"query\"]}",
     &tWebSearch},
    {"web_fetch", "Fetch a URL and return text content",
     "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},"
     "\"required\":[\"url\"]}",
     &tWebFetch},
};

constexpr uint8_t kToolCount = sizeof(kTools) / sizeof(kTools[0]);

const ToolEntry* findTool(const char* name) {
  for (const ToolEntry& t : kTools) {
    if (strcmp(t.name, name) == 0) return &t;
  }
  return nullptr;
}

// Honest catalog: web tools are only advertised when their adapter is
// actually installed — the LLM must not waste turns calling a tool that is
// certain to fail.
bool toolWired(const App& app, const ToolEntry& t) {
  if (t.fn == &tWebSearch) return app.webSearch != nullptr;
  if (t.fn == &tWebFetch) return app.webFetch != nullptr;
  return true;
}

}  // namespace

namespace controlme {

Status DeviceActions::dispatch(const char* toolName, const char* argsJson,
                               const ToolCallCtx& ctx, IByteSink& resultJson,
                               ToolAttach* attach) {
  (void)ctx;  // call identity belongs to ActionService/runner — not a tool argument
  if (!toolName || !*toolName) {
    return Status::Fail(Err::InvalidArgument, "empty toolName");
  }
  const ToolEntry* t = findTool(toolName);
  if (!t) {
    return Status::Fail(Err::InvalidArgument, "unknown tool");
  }
  if (!app_.admissionOpen) {
    return Status::Fail(Err::DeviceRebooting, "admission closed (reboot)");
  }
  JsonDoc jd;
  Status st = parseArgs(argsJson, jd);
  if (!st.ok()) return st;
  return t->fn(app_, jd.root, resultJson, attach);
}

uint8_t DeviceActions::toolCount() {
  uint8_t n = 0;
  for (const ToolEntry& t : kTools) {
    if (toolWired(app_, t)) ++n;
  }
  return n;
}

bool DeviceActions::toolSpec(uint8_t idx, char* nameOut, size_t nameCap,
                             char* descOut, size_t descCap, char* schemaOut,
                             size_t schemaCap) {
  if (!nameOut || !schemaOut) return false;
  for (const ToolEntry& t : kTools) {
    if (!toolWired(app_, t)) continue;
    if (idx-- == 0) {
      copyStr(nameOut, nameCap, t.name);
      if (descOut && descCap) copyStr(descOut, descCap, t.description);
      copyStr(schemaOut, schemaCap, t.schema);
      return strlen(t.name) < nameCap && strlen(t.schema) < schemaCap;
    }
  }
  return false;
}

}  // namespace controlme
