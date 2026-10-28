#pragma once
// HTTP adapter :80 — thin esp_http_server wrapper (docs/02, docs/07).
// Handlers run IN the httpd task; access to resources owned by other owners
// MUST go through the owner_task.h facade (queue + semaphore + deadline) —
// not shared locks. Static files served from LittleFS via POSIX VFS (mounted
// in app, not here).

#include <stddef.h>
#include <stdint.h>
#include <esp_http_server.h>
#include <cJSON.h>
#include "common/config.h"
#include "common/error.h"
#include "ports/writer.h"

namespace transport {

// Request context handed to handlers. Created per-request on the httpd
// task stack — don't store or use from other tasks.
class HttpRequest {
 public:
  explicit HttpRequest(httpd_req_t* r);

  httpd_req_t* raw() const { return r_; }
  int method() const { return r_->method; }
  const char* uri() const { return r_->uri; }   // raw URI — STILL contains the query
  size_t contentLength() const { return r_->content_len; }

  // Query string helpers (cursor/limit/path/offset/length etc).
  bool queryString(char* out, size_t cap) const;  // raw query (without '?')
  bool query(const char* key, char* out, size_t cap) const;
  bool queryU32(const char* key, uint32_t& out) const;
  bool queryU64(const char* key, uint64_t& out) const;
  // Pointer to the URI remainder after `prefix` (for wildcard ".../*"
  // routes). nullptr when uri doesn't start with prefix. NOTE: the tail
  // still carries the query string (r_->uri isn't cut at '?') — to compare
  // id/suffix use tailCopy, not strcmp/strchr on this pointer.
  const char* uriTail(const char* prefix) const;
  // Copy the tail after `prefix` into `out` with the query cut at '?'.
  // false when the prefix doesn't match, cap==0, or the tail (sans query)
  // >= cap.
  bool tailCopy(const char* prefix, char* out, size_t cap) const;
  bool header(const char* name, char* out, size_t cap) const;

  // Body as a chunked byte source (httpd_req_recv). Single consumption.
  controlme::IByteSource& body() { return bodySrc_; }

  // Extra response header; call BEFORE the first send*.
  void setHeader(const char* name, const char* value);

  // Responses — call exactly one per request.
  controlme::Status sendBuf(int status, const char* contentType, const void* data,
                          size_t len);
  controlme::Status sendJson(int status, const char* json);  // application/json
  controlme::Status sendStream(int status, const char* contentType,
                             controlme::IByteSource& src);   // chunked 4 KiB
  // application/problem+json per docs/07 (type=urn:controlme:error:CODE).
  // detailsJson = RAW JSON object fragment (optional, already validated).
  controlme::Status sendProblem(controlme::Err e, const char* detail = nullptr,
                              const char* requestId = nullptr,
                              const char* detailsJson = nullptr);
  bool responded() const { return responded_; }

 private:
  friend class ChunkedSink;
  class BodySource : public controlme::IByteSource {
   public:
    explicit BodySource(httpd_req_t* r) : r_(r), remaining_(r->content_len) {}
    controlme::Status read(uint8_t* buf, size_t cap, size_t& outLen) override;
   private:
    httpd_req_t* r_;
    size_t remaining_;
  };

  httpd_req_t* r_;
  BodySource bodySrc_;
  bool responded_ = false;
};

using HttpHandler = controlme::Status (*)(HttpRequest& req, void* ctx);

// Chunked response sink — used by modules streaming bytes to clients.
// Pattern: req.setHeader(...); ChunkedSink s(req, 200, "text/plain");
//          src.write to s...; s.finish();
class ChunkedSink : public controlme::IByteSink {
 public:
  ChunkedSink(HttpRequest& req, int status, const char* contentType);
  controlme::Status write(const uint8_t* data, size_t len) override;
  controlme::Status finish();   // send the closing chunk; must be called last
 private:
  HttpRequest& req_;
  bool ok_ = true;
};

// Parse the whole JSON body (<= maxBytes). RAII: root freed automatically.
struct JsonBody {
  cJSON* root = nullptr;
  JsonBody() = default;
  ~JsonBody();
  JsonBody(const JsonBody&) = delete;
  JsonBody& operator=(const JsonBody&) = delete;
  bool ok() const { return root != nullptr; }
  cJSON* operator->() const { return root; }
  cJSON* get() const { return root; }
};
controlme::Status parseJsonBody(HttpRequest& req, JsonBody& out,
                              size_t maxBytes = 4096);

// Escape a string into a JSON value (no outer quotes). Used by
// sendProblem and the event serializer — open to other modules.
size_t jsonEscape(const char* src, char* dst, size_t cap);

class HttpServer {
 public:
  // All modules currently use 30 routes (storage 6, camera 2, agent 8,
  // actions 2, rules 8, supervisor 1, core 2 + static fallback). Adding a
  // module? Raise this constant — an addRoute failure is only a WARN + 404
  // endpoint.
  static constexpr uint16_t kMaxRoutes = 32;

  controlme::Status begin(uint16_t port = 80);
  void stop();
  bool running() const { return hd_ != nullptr; }

  // `uri` must be static storage (a literal); ".../*" wildcards allowed —
  // matching uses httpd_uri_match_wildcard and follows registration order.
  controlme::Status addRoute(httpd_method_t method, const char* uri,
                           HttpHandler handler, void* ctx);
  // GET /* -> static files under fsRoot (e.g. "/littlefs"). Register LAST
  // after all API routes. Fallback "/" -> "/index.html"; ".gz" is picked
  // automatically when present and the client accepts gzip; 404 /api/* ->
  // problem+json.
  controlme::Status serveStatic(const char* fsRoot);

 private:
  struct Route {
    httpd_method_t method;
    const char* uri;
    HttpHandler handler;
    void* ctx;
  };
  static esp_err_t bridge(httpd_req_t* r);
  static esp_err_t onError404(httpd_req_t* r, httpd_err_code_t err);
  static controlme::Status staticGet(HttpRequest& req, void* ctx);

  httpd_handle_t hd_ = nullptr;
  Route routes_[kMaxRoutes] = {};
  uint16_t routeCount_ = 0;
  char fsRoot_[48] = {};
};

}  // namespace transport
