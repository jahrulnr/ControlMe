// File routes :80 (docs/07) on top of IStorage (app.storage) — single SD owner.
//
// IMPORTANT — REGISTRATION ORDER: controlme_register_storage() MUST be called
// BEFORE controlme_register_core() in setup(). The core's static "GET /*"
// fallback is matched by httpd IN registration order — /api/* routes
// registered afterwards would never be reached.
//
// Mutation envelope (requestId/expectedBootId/isRetry — docs/03): read from
// the JSON body for POST/PATCH, from the query string for PUT/DELETE (the PUT
// body is raw file bytes). A foreign expectedBootId -> UnknownExecution
// WITHOUT execution (compared against app.bootId). requestId is parsed and
// echoed in the response; full requestId dedup (32/boot cache) is another
// wave's responsibility — here we only parse+pass (noted, not hidden).
// isRetry is parsed too for the same contract.
//
// SD mounting is NOT done here — orchestrated by SdServoCoordinator from
// main.cpp (initial mount required before servo13 PWM is active, docs/05).
#include <stdio.h>
#include <string.h>
#include <memory>
#include <new>
#include <cJSON.h>
#include <esp_log.h>

#include "app.h"
#include "common/psram_alloc.h"
#include "psram_new.h"
#include "common/event.h"   // copyStr
#include "domain/dedup_cache.h"
#include "storage/storage_owner.h"

namespace {

const char* kTag = "mod-storage";
using controlme::Err;
using controlme::Status;
using transport::ChunkedSink;
using transport::HttpRequest;

// transport::jsonEscape worst case = 6 bytes per char ("\u00xx for byte
// <0x20; '"'/'\\' -> 2). Escaping destination buffers are always 6x source
// + margin: snprintf truncation in a JSON payload = silently broken JSON.
constexpr size_t kEscPathCap = 6 * controlme::cfg::kPathCap + 8;    // ~968
constexpr size_t kEscIdCap = 6 * controlme::cfg::kRequestIdCap + 8; // ~248

// Write segment/literal to sink; single error check point per chain.
bool ws(ChunkedSink& sink, const char* p) {
  return sink.write((const uint8_t*)p, strlen(p)).ok();
}

// ------------------------------------------------------------- envelope

struct Envelope {
  char requestId[controlme::cfg::kRequestIdCap] = {};
  char expectedBootId[controlme::cfg::kBootIdCap] = {};
  bool isRetry = false;   // parsed; full dedup = another wave
};

void envFromQuery(HttpRequest& req, Envelope& e) {
  req.query("requestId", e.requestId, sizeof(e.requestId));
  req.query("expectedBootId", e.expectedBootId, sizeof(e.expectedBootId));
  char t[8];
  e.isRetry = req.query("isRetry", t, sizeof(t)) &&
              (strcmp(t, "true") == 0 || strcmp(t, "1") == 0);
}

// transport::JsonBody cannot be instantiated (its deleted copy ctor
// suppresses the implicit default ctor — a defect in lib/transport, outside
// my write scope). Local parser with semantics identical to parseJsonBody:
// maxBytes bound, truncated body -> ProtocolIncomplete, bad size/parse ->
// InvalidArgument.
struct JsonDoc {
  cJSON* root = nullptr;
  ~JsonDoc() {
    if (root) cJSON_Delete(root);
  }
};

Status readJsonBody(HttpRequest& req, JsonDoc& out, size_t maxBytes = 4096) {
  out.root = nullptr;
  if (req.contentLength() == 0 || req.contentLength() > maxBytes) {
    return Status::Fail(Err::InvalidArgument, "json body size invalid");
  }
  char* buf = (char*)controlme::psramMalloc(req.contentLength() + 1);
  if (!buf) return Status::Fail(Err::Internal, "json alloc");
  Status st = Status::Ok();
  size_t total = 0;
  while (total < req.contentLength()) {
    size_t n = 0;
    st = req.body().read((uint8_t*)buf + total, req.contentLength() - total, n);
    if (!st.ok() || n == 0) break;
    total += n;
  }
  if (st.ok() && total == req.contentLength()) {
    buf[total] = '\0';
    out.root = cJSON_Parse(buf);
    if (!out.root) {
      st = Status::Fail(Err::InvalidArgument, "json parse");
    }
  } else if (st.ok()) {
    st = Status::Fail(Err::ProtocolIncomplete, "json body truncated");
  }
  controlme::psramFree(buf);
  return st;
}

bool jsonStr(cJSON* root, const char* key, char* out, size_t cap) {
  cJSON* it = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!cJSON_IsString(it) || !it->valuestring) return false;
  controlme::copyStr(out, cap, it->valuestring);
  return true;
}

void envFromJson(cJSON* root, Envelope& e) {
  jsonStr(root, "requestId", e.requestId, sizeof(e.requestId));
  jsonStr(root, "expectedBootId", e.expectedBootId, sizeof(e.expectedBootId));
  cJSON* it = cJSON_GetObjectItemCaseSensitive(root, "isRetry");
  e.isRetry = cJSON_IsTrue(it);
}

// Foreign expectedBootId -> UNKNOWN_EXECUTION WITHOUT execution (docs/03).
bool bootAccepted(controlme::App& app, HttpRequest& req, const Envelope& e) {
  if (e.expectedBootId[0] != '\0' &&
      strcmp(e.expectedBootId, app.bootId) != 0) {
    req.sendProblem(Err::UnknownExecution,
                    "expectedBootId does not match current boot",
                    e.requestId);
    return false;
  }
  return true;
}

// ------------------------------------------------------------- helpers

controlme::IStorage* storageOr(controlme::App& app, HttpRequest& req) {
  if (!app.storage) {
    req.sendProblem(Err::Internal, "storage owner not active yet");
    return nullptr;
  }
  return app.storage;
}

// All storage errors go through problem+json with details.path
// (+currentRevision on conflict — matched against the docs/07 catalog).
// ~2 KiB buffers on the HEAP (PSRAM-first): this function is called from the
// bottom of the handler chain on the 8 KiB httpd task — a stack array this
// size on top of the handler frame + sendProblem + the IDF send path broke
// the stack canary (panic->reset). Failed alloc -> problem without details is
// still honest.
Status sendStorageProblem(controlme::IStorage* storage, HttpRequest& req,
                          const Status& s, const Envelope& e,
                          const char* path) {
  auto escPBuf = controlme::psramArr<char>(kEscPathCap);
  auto detailsBuf = controlme::psramArr<char>(kEscPathCap + 96);
  if (!escPBuf || !detailsBuf) {
    return req.sendProblem(s.code, s.detail,
                           e.requestId[0] ? e.requestId : nullptr);
  }
  char* escP = escPBuf.get();
  char* details = detailsBuf.get();
  transport::jsonEscape(path, escP, kEscPathCap);
  uint64_t cur = UINT64_MAX;
  if (s.code == Err::RevisionConflict) {
    controlme::FileStat fs{};
    if (storage->stat(path, fs).ok()) cur = fs.revision;   // best-effort
  }
  if (cur != UINT64_MAX) {
    snprintf(details, kEscPathCap + 96,
             "{\"path\":\"%s\",\"currentRevision\":%llu}", escP,
             (unsigned long long)cur);
  } else {
    snprintf(details, kEscPathCap + 96, "{\"path\":\"%s\"}", escP);
  }
  return req.sendProblem(s.code, s.detail,
                         e.requestId[0] ? e.requestId : nullptr, details);
}

// ",\"requestId\":\"...\"" when present — for echoing in success responses.
size_t reqIdFrag(const Envelope& e, char* out, size_t cap) {
  if (!e.requestId[0]) {
    if (cap) out[0] = '\0';
    return 0;
  }
  char esc[kEscIdCap];
  transport::jsonEscape(e.requestId, esc, sizeof(esc));
  return (size_t)snprintf(out, cap, ",\"requestId\":\"%s\"", esc);
}

// ----------------------------------------------------------- mutation dedup
// requestId dedup 32/boot (docs/03 §Envelope) for file mutations. Admit
// before execution; complete() at EVERY terminal point afterwards with
// outcome "<errNum>|<rev>" — a retry replays the recorded outcome, never
// re-executes. httpd handlers run in one task -> no mutex needed.
controlme::DedupCache* s_mutDedup = nullptr;   // filled at register time (PSRAM)

// true -> response already sent (replay/problem); caller must return.
bool mutDedupGate(HttpRequest& req, const Envelope& env) {
  if (!env.requestId[0]) {
    if (env.isRetry) {
      req.sendProblem(Err::UnknownExecution,
                      "isRetry without requestId cannot be verified");
      return true;
    }
    return false;   // no key: no dedup — processed as-is
  }
  if (!s_mutDedup) return false;   // dedup not allocated -> no dedup
  char outcome[controlme::kDedupOutcomeCap];
  switch (s_mutDedup->admit(env.requestId, env.isRetry, outcome,
                            sizeof(outcome))) {
    case controlme::DedupCache::AdmitResult::New:
      return false;
    case controlme::DedupCache::AdmitResult::Full:
      req.sendProblem(Err::QueueFull, "dedup cache full", env.requestId);
      return true;
    case controlme::DedupCache::AdmitResult::UnknownExecution:
      req.sendProblem(Err::UnknownExecution,
                      "isRetry+miss — never re-executed",
                      env.requestId);
      return true;
    case controlme::DedupCache::AdmitResult::Duplicate: {
      if (!outcome[0]) {
        req.sendProblem(Err::ResourceBusy, "requestId still in flight",
                        env.requestId);
        return true;
      }
      unsigned code = 0;
      unsigned long long rev = 0;
      sscanf(outcome, "%u|%llu", &code, &rev);
      if (code == (unsigned)Err::None) {
        char esc[kEscIdCap];
        transport::jsonEscape(env.requestId, esc, sizeof(esc));
        char body[kEscIdCap + 96];
        snprintf(body, sizeof(body),
                 "{\"duplicated\":true,\"requestId\":\"%s\",\"revision\":%llu}",
                 esc, rev);
        req.sendJson(200, body);
      } else {
        req.sendProblem((Err)code, "this requestId was already recorded",
                        env.requestId);
      }
      return true;
    }
  }
  return false;
}

void mutDedupDone(const Envelope& env, Err code, uint64_t rev) {
  if (!env.requestId[0] || !s_mutDedup) return;
  char outcome[24];
  snprintf(outcome, sizeof(outcome), "%u|%llu", (unsigned)code,
           (unsigned long long)rev);
  s_mutDedup->complete(env.requestId, outcome);
}

void addOrLog(controlme::App& app, httpd_method_t method, const char* uri,
              transport::HttpHandler h) {
  Status st = app.http.addRoute(method, uri, h, &app);
  if (!st.ok()) {
    ESP_LOGE(kTag, "route %s failed: %s", uri, controlme::errName(st.code));
  }
}

// Lazy sink for downloads: the ChunkedSink (status+headers on the wire) is
// only created on the FIRST byte. Without this, a read() that fails before
// the first byte (e.g. FILE_BUSY active lease, or a file gone between stat
// and read) would already have sent "200 + empty body" — a fake response.
// With this deferral, problem+json stays valid as long as no byte has gone
// out; an empty file still gets an honest 200 via finish().
class DeferredContentSink : public controlme::IByteSink {
 public:
  DeferredContentSink(HttpRequest& req, int status, const char* contentType)
      : req_(req), status_(status), ct_(contentType) {}
  ~DeferredContentSink() override { controlme::psramDelete(real_); }
  Status write(const uint8_t* data, size_t len) override {
    if (!real_) real_ = controlme::psramNew<ChunkedSink>(req_, status_, ct_);
    if (!real_) return Status::Fail(Err::Internal, "sink alloc");
    return real_->write(data, len);
  }
  // Called last: ensures a response is sent (even 0 bytes) if no byte has
  // gone out yet.
  Status finish() {
    if (!real_) real_ = controlme::psramNew<ChunkedSink>(req_, status_, ct_);
    if (!real_) return Status::Fail(Err::Internal, "sink alloc");
    return real_->finish();
  }
  bool started() const { return real_ != nullptr; }

 private:
  HttpRequest& req_;
  int status_;
  const char* ct_;
  ChunkedSink* real_ = nullptr;
};

// ------------------------------------------------------------- handlers

// GET /api/v1/files?path=&cursor=&limit= — paginated listing + revision.
Status handleFilesGet(HttpRequest& req, void* ctx) {
  controlme::App& app = *static_cast<controlme::App*>(ctx);
  Envelope env{};
  controlme::IStorage* st = storageOr(app, req);
  if (!st) return Status::Ok();
  char path[controlme::cfg::kPathCap] = "/";
  req.query("path", path, sizeof(path));
  char cursor[controlme::cfg::kIdCap] = {};
  req.query("cursor", cursor, sizeof(cursor));
  uint32_t lim = controlme::cfg::kPageDefault;
  req.queryU32("limit", lim);
  // ListPage.entries max 16 (port) — honest clamp above the default/contract.
  if (lim == 0 || lim > 16) lim = 16;
  controlme::FileStat ds{};
  Status s = st->stat(path, ds);
  if (!s.ok()) return sendStorageProblem(st, req, s, env, path);
  if (!ds.isDir) {
    return req.sendProblem(Err::InvalidArgument, "path is not a directory",
                           nullptr, nullptr);
  }
  // ListPage ~3 KB (16 FileStat) — per-request heap so the 8 KiB httpd
  // stack isn't squeezed together with the escaping buffers.
  auto pgPtr = controlme::psramMake<controlme::ListPage>();
  if (!pgPtr) return req.sendProblem(Err::Internal, "list page alloc");
  controlme::ListPage& pg = *pgPtr;
  s = st->list(path, cursor, (uint8_t)lim, pg);
  if (!s.ok()) return sendStorageProblem(st, req, s, env, path);
  // Stream JSON per segment — a page can be ~5 KB, don't build it whole.
  // esc/seg ~2 KiB on the HEAP (PSRAM-first), not stack: the 8 KiB httpd task
  // and this frame + sendProblem + the IDF send path have broken the canary
  // before. Must be allocated BEFORE ChunkedSink — its ctor locks the
  // response (headers).
  auto escBuf = controlme::psramArr<char>(kEscPathCap);
  auto segBuf = controlme::psramArr<char>(kEscPathCap + 64);
  if (!escBuf || !segBuf) {
    return req.sendProblem(Err::Internal, "response buffer alloc failed");
  }
  char* esc = escBuf.get();
  char* seg = segBuf.get();
  ChunkedSink sink(req, 200, "application/json");
  transport::jsonEscape(path, esc, kEscPathCap);
  snprintf(seg, kEscPathCap + 64,
           "{\"path\":\"%s\",\"revision\":%llu,\"entries\":[", esc,
           (unsigned long long)ds.revision);
  bool okW = ws(sink, seg);
  for (uint8_t i = 0; okW && i < pg.count; ++i) {
    controlme::FileStat& fe = pg.entries[i];
    const char* slash = strrchr(fe.path, '/');
    const char* name = slash ? slash + 1 : fe.path;
    okW = ws(sink, i ? ",{\"name\":\"" : "{\"name\":\"");
    if (okW) {
      transport::jsonEscape(name, esc, kEscPathCap);
      okW = ws(sink, esc) && ws(sink, "\",\"path\":\"");
    }
    if (okW) {
      transport::jsonEscape(fe.path, esc, kEscPathCap);
      okW = ws(sink, esc);
    }
    // Entry numeric tail: starts with the path-closing '"' + comma.
    char nums[112];
    snprintf(nums, sizeof(nums),
             "\",\"size\":%llu,\"isDir\":%s,\"mtimeMs\":%llu,\"revision\":%llu}",
             (unsigned long long)fe.size, fe.isDir ? "true" : "false",
             (unsigned long long)fe.mtimeMs,
             (unsigned long long)fe.revision);
    if (okW) okW = ws(sink, nums);
  }
  if (!okW) return sink.finish();   // dead connection -> send trailer then fail
  transport::jsonEscape(pg.nextCursor, esc, kEscPathCap);
  snprintf(seg, kEscPathCap + 64, "],\"nextCursor\":\"%s\"}", esc);
  ws(sink, seg);
  return sink.finish();
}

// GET /api/v1/files/content?path=&offset=&length= — chunked stream +
// X-Revision header (docs/07). offset defaults to 0; length defaults to EOF.
Status handleContentGet(HttpRequest& req, void* ctx) {
  controlme::App& app = *static_cast<controlme::App*>(ctx);
  Envelope env{};
  controlme::IStorage* st = storageOr(app, req);
  if (!st) return Status::Ok();
  char path[controlme::cfg::kPathCap];
  if (!req.query("path", path, sizeof(path))) {
    return req.sendProblem(Err::InvalidArgument, "path param required");
  }
  uint64_t offset = 0, len = 0;
  req.queryU64("offset", offset);
  const bool hasLen = req.queryU64("length", len);
  controlme::FileStat fs{};
  Status s = st->stat(path, fs);
  if (!s.ok()) return sendStorageProblem(st, req, s, env, path);
  if (fs.isDir) {
    return req.sendProblem(Err::InvalidArgument, "path is a directory");
  }
  if (offset > fs.size) {
    return sendStorageProblem(st, req,
                              Status::Fail(Err::InvalidArgument,
                                          "offset exceeds file size"),
                              env, path);
  }
  uint64_t want = fs.size - offset;
  if (hasLen && len < want) want = len;   // clamp to EOF
  char revBuf[24];
  snprintf(revBuf, sizeof(revBuf), "%llu", (unsigned long long)fs.revision);
  req.setHeader("X-Revision", revBuf);
  DeferredContentSink sink(req, 200, "application/octet-stream");
  s = st->read(path, offset, want, sink);
  if (!s.ok() && !sink.started()) {
    // No byte sent to the client yet -> problem+json still valid (not a
    // fake 200).
    return sendStorageProblem(st, req, s, env, path);
  }
  Status f = sink.finish();
  // Mid-stream failure: response already sent — bridge closes the connection (honest).
  return s.ok() ? f : s;
}

// PUT /api/v1/files/content?path=&mode=create|replace&expectedRevision=
// (+envelope via query) — ONE streaming upload: writeBegin -> body() loop of
// 4 KiB -> writeChunk -> writeCommit; on failure -> writeAbort + problem.
Status handleContentPut(HttpRequest& req, void* ctx) {
  controlme::App& app = *static_cast<controlme::App*>(ctx);
  Envelope env{};
  envFromQuery(req, env);
  if (!bootAccepted(app, req, env)) return Status::Ok();
  controlme::IStorage* st = storageOr(app, req);
  if (!st) return Status::Ok();
  char path[controlme::cfg::kPathCap];
  if (!req.query("path", path, sizeof(path))) {
    return req.sendProblem(Err::InvalidArgument, "path param required",
                           env.requestId);
  }
  bool replace = false;
  char mode[12];
  if (req.query("mode", mode, sizeof(mode))) {
    if (strcmp(mode, "replace") == 0) {
      replace = true;
    } else if (strcmp(mode, "create") != 0) {
      return req.sendProblem(Err::InvalidArgument,
                             "mode must be create|replace", env.requestId);
    }
  }
  uint64_t expectedRev = 0;
  req.queryU64("expectedRevision", expectedRev);
  if (replace && expectedRev == 0) {
    return req.sendProblem(Err::InvalidArgument,
                           "expectedRevision required for mode=replace",
                           env.requestId);
  }
  // A body without Content-Length (chunked) can't be read via BodySource —
  // it would read as empty and a 0-byte file would be silently created.
  // Honest: reject.
  if (req.contentLength() == 0) {
    char te[24];
    if (req.header("Transfer-Encoding", te, sizeof(te))) {
      return req.sendProblem(Err::NotSupported,
                             "chunked body not supported — use "
                             "Content-Length",
                             env.requestId);
    }
  }
  if (mutDedupGate(req, env)) return Status::Ok();
  char uploadId[controlme::cfg::kIdCap];
  Status s = st->writeBegin(path, replace, expectedRev, uploadId);
  if (!s.ok()) {
    mutDedupDone(env, s.code, 0);
    return sendStorageProblem(st, req, s, env, path);
  }
  // Chunk buffer on the heap — the httpd task only has 8 KiB of stack and
  // already uses a 4 KiB buffer on another path; per-request malloc is cheap
  // and bounded.
  auto buf = controlme::psramArr<uint8_t>(controlme::cfg::kIoChunk);
  if (!buf) {
    st->writeAbort(uploadId);
    mutDedupDone(env, Err::Internal, 0);
    return req.sendProblem(Err::Internal, "chunk buffer alloc failed",
                           env.requestId);
  }
  uint64_t off = 0;
  for (;;) {
    size_t n = 0;
    s = req.body().read(buf.get(), controlme::cfg::kIoChunk, n);
    if (!s.ok()) {
      st->writeAbort(uploadId);
      mutDedupDone(env, s.code, 0);
      return req.sendProblem(s.code, s.detail, env.requestId);
    }
    if (n == 0) break;   // body EOF
    uint64_t next = 0;
    s = st->writeChunk(uploadId, off, buf.get(), n, next);
    if (!s.ok()) {
      st->writeAbort(uploadId);
      mutDedupDone(env, s.code, 0);
      return sendStorageProblem(st, req, s, env, path);
    }
    off = next;   // server-authoritative nextOffset (idempotent dedup safe)
  }
  uint64_t rev = 0;
  s = st->writeCommit(uploadId, off, rev);
  // Commit failure: the lease was released by the owner and staging is
  // LEFT as an inspection trail — do not writeAbort (that removes staging).
  if (!s.ok()) {
    mutDedupDone(env, s.code, 0);
    return sendStorageProblem(st, req, s, env, path);
  }
  mutDedupDone(env, Err::None, rev);
  // escP/body ~2.2 KiB on the HEAP (PSRAM-first), not stack — this frame
  // already broke the 8 KiB httpd task canary on the response path (see
  // sendStorageProblem).
  auto escPBuf = controlme::psramArr<char>(kEscPathCap);
  auto bodyBuf = controlme::psramArr<char>(kEscPathCap + kEscIdCap + 96);
  if (!escPBuf || !bodyBuf) {
    return req.sendProblem(Err::Internal, "response buffer alloc failed",
                           env.requestId);
  }
  char* escP = escPBuf.get();
  char* body = bodyBuf.get();
  transport::jsonEscape(path, escP, kEscPathCap);
  char rid[kEscIdCap + 24];
  reqIdFrag(env, rid, sizeof(rid));
  snprintf(body, kEscPathCap + kEscIdCap + 96,
           "{\"path\":\"%s\",\"revision\":%llu,\"bytes\":%llu%s}", escP,
           (unsigned long long)rev, (unsigned long long)off, rid);
  return req.sendJson(200, body);
}

// DELETE /api/v1/files?path=&recursive=true (+envelope via query).
Status handleFilesDelete(HttpRequest& req, void* ctx) {
  controlme::App& app = *static_cast<controlme::App*>(ctx);
  Envelope env{};
  envFromQuery(req, env);
  if (!bootAccepted(app, req, env)) return Status::Ok();
  controlme::IStorage* st = storageOr(app, req);
  if (!st) return Status::Ok();
  char path[controlme::cfg::kPathCap];
  if (!req.query("path", path, sizeof(path))) {
    return req.sendProblem(Err::InvalidArgument, "path param required",
                           env.requestId);
  }
  char rec[8];
  const bool recursive = req.query("recursive", rec, sizeof(rec)) &&
                         (strcmp(rec, "true") == 0 || strcmp(rec, "1") == 0);
  if (mutDedupGate(req, env)) return Status::Ok();
  Status s = st->remove(path, recursive);
  mutDedupDone(env, s.code, 0);
  if (!s.ok()) return sendStorageProblem(st, req, s, env, path);
  auto escPBuf = controlme::psramArr<char>(kEscPathCap);
  auto bodyBuf = controlme::psramArr<char>(kEscPathCap + kEscIdCap + 96);
  if (!escPBuf || !bodyBuf) {
    return req.sendProblem(Err::Internal, "response buffer alloc failed",
                           env.requestId);
  }
  char* escP = escPBuf.get();
  char* body = bodyBuf.get();
  transport::jsonEscape(path, escP, kEscPathCap);
  char rid[kEscIdCap + 24];
  reqIdFrag(env, rid, sizeof(rid));
  snprintf(body, kEscPathCap + kEscIdCap + 96, "{\"path\":\"%s\",\"deleted\":true%s}", escP,
           rid);
  return req.sendJson(200, body);
}

// POST /api/v1/directories {path, +envelope} — mkdir (no auto-parent).
Status handleDirectories(HttpRequest& req, void* ctx) {
  controlme::App& app = *static_cast<controlme::App*>(ctx);
  JsonDoc jd;
  Status s = readJsonBody(req, jd);
  if (!s.ok()) {
    return req.sendProblem(s.code, s.detail ? s.detail : "invalid json body");
  }
  Envelope env{};
  envFromJson(jd.root, env);
  if (!bootAccepted(app, req, env)) return Status::Ok();
  controlme::IStorage* st = storageOr(app, req);
  if (!st) return Status::Ok();
  char path[controlme::cfg::kPathCap];
  if (!jsonStr(jd.root, "path", path, sizeof(path))) {
    return req.sendProblem(Err::InvalidArgument, "path field required",
                           env.requestId);
  }
  if (mutDedupGate(req, env)) return Status::Ok();
  uint64_t rev = 0;
  s = st->mkdir(path, rev);
  mutDedupDone(env, s.code, s.ok() ? rev : 0);
  if (!s.ok()) return sendStorageProblem(st, req, s, env, path);
  auto escPBuf = controlme::psramArr<char>(kEscPathCap);
  auto bodyBuf = controlme::psramArr<char>(kEscPathCap + kEscIdCap + 96);
  if (!escPBuf || !bodyBuf) {
    return req.sendProblem(Err::Internal, "response buffer alloc failed",
                           env.requestId);
  }
  char* escP = escPBuf.get();
  char* body = bodyBuf.get();
  transport::jsonEscape(path, escP, kEscPathCap);
  char rid[kEscIdCap + 24];
  reqIdFrag(env, rid, sizeof(rid));
  snprintf(body, kEscPathCap + kEscIdCap + 96,
           "{\"path\":\"%s\",\"revision\":%llu%s}", escP,
           (unsigned long long)rev, rid);
  return req.sendJson(200, body);
}

// PATCH /api/v1/files/metadata {path, renameTo, expectedRevision, +envelope}.
Status handleMetadataPatch(HttpRequest& req, void* ctx) {
  controlme::App& app = *static_cast<controlme::App*>(ctx);
  JsonDoc jd;
  Status s = readJsonBody(req, jd);
  if (!s.ok()) {
    return req.sendProblem(s.code, s.detail ? s.detail : "invalid json body");
  }
  Envelope env{};
  envFromJson(jd.root, env);
  if (!bootAccepted(app, req, env)) return Status::Ok();
  controlme::IStorage* st = storageOr(app, req);
  if (!st) return Status::Ok();
  char path[controlme::cfg::kPathCap];
  char renameTo[controlme::cfg::kPathCap];
  if (!jsonStr(jd.root, "path", path, sizeof(path)) ||
      !jsonStr(jd.root, "renameTo", renameTo, sizeof(renameTo))) {
    return req.sendProblem(Err::InvalidArgument,
                           "path and renameTo fields required", env.requestId);
  }
  uint64_t expectedRev = 0;
  cJSON* it = cJSON_GetObjectItemCaseSensitive(jd.root, "expectedRevision");
  if (cJSON_IsNumber(it)) expectedRev = (uint64_t)it->valuedouble;
  if (mutDedupGate(req, env)) return Status::Ok();
  uint64_t rev = 0;
  s = st->rename(path, renameTo, expectedRev, rev);
  mutDedupDone(env, s.code, s.ok() ? rev : 0);
  if (!s.ok()) return sendStorageProblem(st, req, s, env, path);
  auto escPBuf = controlme::psramArr<char>(kEscPathCap);
  auto bodyBuf = controlme::psramArr<char>(kEscPathCap + kEscIdCap + 96);
  if (!escPBuf || !bodyBuf) {
    return req.sendProblem(Err::Internal, "response buffer alloc failed",
                           env.requestId);
  }
  char* escP = escPBuf.get();
  char* body = bodyBuf.get();
  transport::jsonEscape(renameTo, escP, kEscPathCap);
  char rid[kEscIdCap + 24];
  reqIdFrag(env, rid, sizeof(rid));
  snprintf(body, kEscPathCap + kEscIdCap + 96,
           "{\"path\":\"%s\",\"revision\":%llu%s}", escP,
           (unsigned long long)rev, rid);
  return req.sendJson(200, body);
}

}  // namespace

// Called from setup() BEFORE controlme_register_core() — see the wildcard
// ordering note above.
void controlme_register_storage(controlme::App& app) {
  // ~24 KiB revision table + ~4 KiB dedup — PSRAM, not .bss/internal heap.
  static controlme::StorageOwner* owner = controlme::psramNew<controlme::StorageOwner>();
  if (!s_mutDedup) s_mutDedup = controlme::psramNew<controlme::DedupCache>();
  if (!s_mutDedup) {
    ESP_LOGW(kTag, "dedup cache alloc failed — file mutation dedup disabled");
  }
  if (!owner) {
    ESP_LOGE(kTag, "storage owner alloc failed — storage module skipped");
    return;
  }
  Status s = owner->start();
  if (!s.ok()) {
    ESP_LOGE(kTag, "storage owner start failed: %s", controlme::errName(s.code));
  } else {
    app.storage = owner;
  }
  addOrLog(app, HTTP_GET, "/api/v1/files", &handleFilesGet);
  addOrLog(app, HTTP_DELETE, "/api/v1/files", &handleFilesDelete);
  addOrLog(app, HTTP_GET, "/api/v1/files/content", &handleContentGet);
  addOrLog(app, HTTP_PUT, "/api/v1/files/content", &handleContentPut);
  addOrLog(app, HTTP_POST, "/api/v1/directories", &handleDirectories);
  addOrLog(app, HTTP_PATCH, "/api/v1/files/metadata", &handleMetadataPatch);
  app.addFeature("{\"sd\":true}");
}
