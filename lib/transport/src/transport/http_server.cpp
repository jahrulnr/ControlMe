#include "transport/http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include "common/psram_alloc.h"

namespace transport {

static const char* kTag = "http";

// ---------------------------------------------------------------- util

static const char* statusText(int status) {
  switch (status) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 202: return "202 Accepted";
    case 204: return "204 No Content";
    case 400: return "400 Bad Request";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 422: return "422 Unprocessable Content";
    case 429: return "429 Too Many Requests";
    case 500: return "500 Internal Server Error";
    case 502: return "502 Bad Gateway";
    case 503: return "503 Service Unavailable";
    case 504: return "504 Gateway Timeout";
  }
  static thread_local char buf[8];  // httpd task may recur across requests
  snprintf(buf, sizeof(buf), "%d", status);
  return buf;
}

size_t jsonEscape(const char* src, char* dst, size_t cap) {
  size_t n = 0;
  if (!src) src = "";
  for (const char* p = src; *p && n + 1 < cap; ++p) {
    const unsigned char c = (unsigned char)*p;
    const char* rep = nullptr;
    switch (c) {
      case '"': rep = "\\\""; break;
      case '\\': rep = "\\\\"; break;
      case '\n': rep = "\\n"; break;
      case '\r': rep = "\\r"; break;
      case '\t': rep = "\\t"; break;
      default: break;
    }
    if (rep) {
      if (n + 2 >= cap) break;
      dst[n++] = rep[0];
      dst[n++] = rep[1];
    } else if (c < 0x20) {
      if (n + 6 >= cap) break;
      n += snprintf(dst + n, cap - n, "\\u%04x", c);
    } else {
      dst[n++] = (char)c;
    }
  }
  if (cap) dst[n < cap ? n : cap - 1] = '\0';
  return n;
}

// retryable per the docs/07 catalog meaning: transient only -> true.
static bool errRetryable(controlme::Err e) {
  using controlme::Err;
  switch (e) {
    case Err::QueueFull:
    case Err::ResourceBusy:
    case Err::Timeout:
    case Err::ProviderUnavailable:
    case Err::SdUnavailable:
    case Err::DeviceRebooting:
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------- HttpRequest

HttpRequest::HttpRequest(httpd_req_t* r) : r_(r), bodySrc_(r) {}

controlme::Status HttpRequest::BodySource::read(uint8_t* buf, size_t cap,
                                              size_t& outLen) {
  outLen = 0;
  if (remaining_ == 0) return controlme::Status::Ok();  // EOF
  size_t want = cap < remaining_ ? cap : remaining_;
  int n = httpd_req_recv(r_, (char*)buf, want);
  if (n == HTTPD_SOCK_ERR_TIMEOUT) {
    return controlme::Status::Fail(controlme::Err::Timeout, "body recv timeout");
  }
  if (n <= 0) {
    return controlme::Status::Fail(controlme::Err::ProtocolIncomplete,
                                 "body recv failed");
  }
  remaining_ -= (size_t)n;
  outLen = (size_t)n;
  return controlme::Status::Ok();
}

bool HttpRequest::queryString(char* out, size_t cap) const {
  if (cap == 0) return false;
  out[0] = '\0';
  if (httpd_req_get_url_query_len(r_) >= cap) return false;
  return httpd_req_get_url_query_str(r_, out, cap) == ESP_OK;
}

bool HttpRequest::query(const char* key, char* out, size_t cap) const {
  if (cap == 0) return false;
  out[0] = '\0';
  char qbuf[512];
  if (!queryString(qbuf, sizeof(qbuf)) || qbuf[0] == '\0') return false;
  if (httpd_query_key_value(qbuf, key, out, cap) != ESP_OK) return false;
  // httpd_query_key_value doesn't decode percent-encoding in this build:
  // `path=%2F` reaches the handler as a literal "%2F" -> fake 422. Decode
  // in-place (%XX -> byte, '+' -> ' ' form-urlencoded style) so all
  // endpoints accept standard client encoding.
  char* r = out;
  char* w = out;
  auto hexv = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  while (*r) {
    if (*r == '%' && hexv(r[1]) >= 0 && hexv(r[2]) >= 0) {
      *w++ = (char)((hexv(r[1]) << 4) | hexv(r[2]));
      r += 3;
    } else {
      *w++ = (*r == '+') ? ' ' : *r;
      ++r;
    }
  }
  *w = '\0';
  return true;
}

bool HttpRequest::queryU32(const char* key, uint32_t& out) const {
  char tmp[16];
  if (!query(key, tmp, sizeof(tmp))) return false;
  char* end = nullptr;
  unsigned long v = strtoul(tmp, &end, 10);
  if (!end || *end != '\0') return false;
  out = (uint32_t)v;
  return true;
}

bool HttpRequest::queryU64(const char* key, uint64_t& out) const {
  char tmp[24];
  if (!query(key, tmp, sizeof(tmp))) return false;
  char* end = nullptr;
  unsigned long long v = strtoull(tmp, &end, 10);
  if (!end || *end != '\0') return false;
  out = (uint64_t)v;
  return true;
}

const char* HttpRequest::uriTail(const char* prefix) const {
  size_t n = strlen(prefix);
  if (strncmp(r_->uri, prefix, n) != 0) return nullptr;
  return r_->uri + n;
}

bool HttpRequest::tailCopy(const char* prefix, char* out, size_t cap) const {
  const char* t = uriTail(prefix);
  if (!t || cap == 0) return false;
  size_t n = strcspn(t, "?");
  if (n >= cap) return false;
  memcpy(out, t, n);
  out[n] = '\0';
  return true;
}

bool HttpRequest::header(const char* name, char* out, size_t cap) const {
  if (cap == 0) return false;
  out[0] = '\0';
  return httpd_req_get_hdr_value_str(r_, name, out, cap) == ESP_OK;
}

void HttpRequest::setHeader(const char* name, const char* value) {
  httpd_resp_set_hdr(r_, name, value);
}

controlme::Status HttpRequest::sendBuf(int status, const char* contentType,
                                     const void* data, size_t len) {
  httpd_resp_set_status(r_, statusText(status));
  httpd_resp_set_type(r_, contentType);
  responded_ = true;
  if (httpd_resp_send(r_, (const char*)data, (ssize_t)len) != ESP_OK) {
    return controlme::Status::Fail(controlme::Err::Internal, "resp send failed");
  }
  return controlme::Status::Ok();
}

controlme::Status HttpRequest::sendJson(int status, const char* json) {
  return sendBuf(status, "application/json", json, json ? strlen(json) : 0);
}

controlme::Status HttpRequest::sendStream(int status, const char* contentType,
                                        controlme::IByteSource& src) {
  httpd_resp_set_status(r_, statusText(status));
  httpd_resp_set_type(r_, contentType);
  responded_ = true;
  // 4 KiB pump on the HEAP (PSRAM-first) — on the stack this broke the
  // 8 KiB httpd task canary when called from the bottom of the handler
  // chain.
  auto buf = controlme::psramArr<uint8_t>(controlme::cfg::kIoChunk);
  if (!buf) {
    return controlme::Status::Fail(controlme::Err::Internal, "stream buf alloc");
  }
  controlme::Status st = controlme::Status::Ok();
  for (;;) {
    size_t n = 0;
    st = src.read(buf.get(), controlme::cfg::kIoChunk, n);
    if (!st.ok() || n == 0) break;
    if (httpd_resp_send_chunk(r_, (const char*)buf.get(), (ssize_t)n) !=
        ESP_OK) {
      return controlme::Status::Fail(controlme::Err::Internal, "chunk send failed");
    }
  }
  // Close the chunked encoding so the client doesn't hang; errors still reported.
  httpd_resp_send_chunk(r_, nullptr, 0);
  return st;
}

controlme::Status HttpRequest::sendProblem(controlme::Err e, const char* detail,
                                         const char* requestId,
                                         const char* detailsJson) {
  int status = controlme::errHttpStatus(e);
  const char* code = controlme::errName(e);
  char escDetail[256];
  jsonEscape(detail && *detail ? detail : controlme::errTitle(e), escDetail,
             sizeof(escDetail));
  // Body on the HEAP (PSRAM-first), not stack: this function is called
  // from the bottom of the handler chain on the 8 KiB httpd task — the old
  // ~1.2 KiB frame + the caller's escaping buffers already broke the stack
  // canary (panic -> device reset). Size covers the caller's worst-case
  // detailsJson (~kEscPathCap+96 in mod_storage) so the response isn't cut
  // into invalid JSON.
  constexpr size_t kProblemBodyCap = 2 * 1024;
  auto bodyBuf = controlme::psramArr<char>(kProblemBodyCap);
  if (!bodyBuf) {
    static const char kMinimal[] =
        "{\"type\":\"urn:controlme:error:INTERNAL\",\"title\":\"Internal\","
        "\"status\":500,\"detail\":\"resp alloc\",\"code\":\"INTERNAL\","
        "\"retryable\":false}";
    return sendBuf(500, "application/problem+json", kMinimal,
                   sizeof(kMinimal) - 1);
  }
  char* body = bodyBuf.get();
  size_t n = (size_t)snprintf(
      body, kProblemBodyCap,
      "{\"type\":\"urn:controlme:error:%s\",\"title\":\"%s\",\"status\":%d,"
      "\"detail\":\"%s\",\"code\":\"%s\",\"retryable\":%s",
      code, controlme::errTitle(e), status, escDetail, code,
      errRetryable(e) ? "true" : "false");
  if (n >= kProblemBodyCap) n = kProblemBodyCap - 1;
  if (requestId && *requestId) {
    char escReq[2 * controlme::cfg::kRequestIdCap];
    jsonEscape(requestId, escReq, sizeof(escReq));
    n += (size_t)snprintf(body + n, kProblemBodyCap - n, ",\"requestId\":\"%s\"",
                          escReq);
    if (n >= kProblemBodyCap) n = kProblemBodyCap - 1;
  }
  if (detailsJson && *detailsJson) {
    n += (size_t)snprintf(body + n, kProblemBodyCap - n, ",\"details\":%s",
                          detailsJson);
    if (n >= kProblemBodyCap) n = kProblemBodyCap - 1;
  }
  snprintf(body + n, kProblemBodyCap - n, "}");
  return sendBuf(status, "application/problem+json", body, strlen(body));
}

// ---------------------------------------------------------------- ChunkedSink

ChunkedSink::ChunkedSink(HttpRequest& req, int status, const char* contentType)
    : req_(req) {
  httpd_resp_set_status(req_.r_, statusText(status));
  httpd_resp_set_type(req_.r_, contentType);
  req_.responded_ = true;
}

controlme::Status ChunkedSink::write(const uint8_t* data, size_t len) {
  if (!ok_) return controlme::Status::Fail(controlme::Err::Internal, "sink closed");
  // len=0 in httpd_resp_send_chunk = response TERMINATOR — a caller
  // writing an empty string (empty JSON field) used to cut the response
  // mid-way.
  if (len == 0) return controlme::Status::Ok();
  if (httpd_resp_send_chunk(req_.r_, (const char*)data, (ssize_t)len) !=
      ESP_OK) {
    ok_ = false;
    return controlme::Status::Fail(controlme::Err::Internal, "chunk send failed");
  }
  return controlme::Status::Ok();
}

controlme::Status ChunkedSink::finish() {
  if (!ok_) return controlme::Status::Fail(controlme::Err::Internal, "sink closed");
  ok_ = false;
  httpd_resp_send_chunk(req_.r_, nullptr, 0);
  return controlme::Status::Ok();
}

// ---------------------------------------------------------------- JsonBody

JsonBody::~JsonBody() {
  if (root) cJSON_Delete(root);
}

controlme::Status parseJsonBody(HttpRequest& req, JsonBody& out,
                              size_t maxBytes) {
  out.root = nullptr;
  if (req.contentLength() == 0 || req.contentLength() > maxBytes) {
    return controlme::Status::Fail(controlme::Err::InvalidArgument,
                                 "json body size invalid");
  }
  char* buf = (char*)controlme::psramMalloc(req.contentLength() + 1);
  if (!buf) return controlme::Status::Fail(controlme::Err::Internal, "json alloc");
  controlme::Status st = controlme::Status::Ok();
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
      st = controlme::Status::Fail(controlme::Err::InvalidArgument, "json parse");
    }
  } else if (st.ok()) {
    st = controlme::Status::Fail(controlme::Err::ProtocolIncomplete, "json body truncated");
  }
  controlme::psramFree(buf);
  return st;
}

// ---------------------------------------------------------------- static files

static const char* mimeOf(const char* path) {
  const char* dot = strrchr(path, '.');
  if (!dot) return "application/octet-stream";
  ++dot;
  struct Map { const char* ext; const char* mime; };
  static const Map kMap[] = {
      {"html", "text/html"},         {"htm", "text/html"},
      {"css", "text/css"},           {"js", "text/javascript"},
      {"json", "application/json"},  {"svg", "image/svg+xml"},
      {"png", "image/png"},          {"jpg", "image/jpeg"},
      {"jpeg", "image/jpeg"},        {"ico", "image/x-icon"},
      {"txt", "text/plain"},         {"map", "application/json"},
  };
  for (const auto& m : kMap) {
    if (strcasecmp(dot, m.ext) == 0) return m.mime;
  }
  return "application/octet-stream";
}

static bool fsFileExists(const char* path) {
  struct stat st;
  return ::stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// File byte source (RAII over FILE*).
class FileSource : public controlme::IByteSource {
 public:
  explicit FileSource(FILE* f) : f_(f) {}
  ~FileSource() override { if (f_) fclose(f_); }
  controlme::Status read(uint8_t* buf, size_t cap, size_t& outLen) override {
    outLen = fread(buf, 1, cap, f_);
    if (outLen == 0 && ferror(f_)) {
      return controlme::Status::Fail(controlme::Err::Internal, "file read failed");
    }
    return controlme::Status::Ok();
  }
 private:
  FILE* f_;
};

controlme::Status HttpServer::staticGet(HttpRequest& req, void* ctx) {
  HttpServer* self = static_cast<HttpServer*>(ctx);
  const char* uri = req.uri();
  // Reject traversal; uri is already httpd-decoded. r_->uri still carries
  // the query string — cut at '?' before treating it as a file path.
  const size_t uriLen = strcspn(uri, "?");
  if (uri[0] != '/' || strstr(uri, "..")) {
    return controlme::Status::Fail(controlme::Err::InvalidArgument, "bad path");
  }
  const bool root = uriLen <= 1;
  const char* rel = root ? "/index.html" : uri;
  const size_t relLen = root ? strlen(rel) : uriLen;
  char path[288];
  char gzPath[304];
  int pn = snprintf(path, sizeof(path), "%s%.*s", self->fsRoot_, (int)relLen,
                    rel);
  if (pn <= 0 || pn >= (int)sizeof(path)) {
    return controlme::Status::Fail(controlme::Err::InvalidArgument, "path too long");
  }
  const char* serve = path;
  bool gzip = false;
  char acc[80];
  if (req.header("Accept-Encoding", acc, sizeof(acc)) && strstr(acc, "gzip")) {
    int gn = snprintf(gzPath, sizeof(gzPath), "%s.gz", path);
    if (gn > 0 && gn < (int)sizeof(gzPath) && fsFileExists(gzPath)) {
      serve = gzPath;
      gzip = true;
    }
  }
  if (!gzip && !fsFileExists(path)) {
    if (strncmp(uri, "/api/", 5) == 0) {
      return req.sendProblem(controlme::Err::NotFound, "route not found");
    }
    return req.sendBuf(404, "text/plain", "404 Not Found\n", 14);
  }
  FILE* f = fopen(serve, "rb");
  if (!f) {
    return req.sendProblem(controlme::Err::Internal, "file open failed");
  }
  if (gzip) req.setHeader("Content-Encoding", "gzip");
  // Small UI asset — read it whole (PSRAM-first) then one
  // Content-Length send: fewer syscalls & the connection is released quickly
  // when internal heap is tight.
  constexpr long kWholeFileMax = 128 * 1024;
  if (fseek(f, 0, SEEK_END) == 0) {
    long sz = ftell(f);
    if (sz >= 0 && sz <= kWholeFileMax && fseek(f, 0, SEEK_SET) == 0) {
      uint8_t* buf = static_cast<uint8_t*>(
          controlme::psramMalloc((size_t)(sz > 0 ? sz : 1)));
      if (buf) {
        size_t got = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        if (got != (size_t)sz) {
          controlme::psramFree(buf);
          return req.sendProblem(controlme::Err::Internal, "file read failed");
        }
        controlme::Status st = req.sendBuf(200, mimeOf(path), buf, got);
        controlme::psramFree(buf);
        return st;
      }
    }
    fseek(f, 0, SEEK_SET);  // fallback: chunked stream
  }
  FileSource src(f);
  return req.sendStream(200, mimeOf(path), src);
}

// ---------------------------------------------------------------- HttpServer

esp_err_t HttpServer::bridge(httpd_req_t* r) {
  Route* route = static_cast<Route*>(r->user_ctx);
  HttpRequest req(r);
  controlme::Status st = route->handler(req, route->ctx);
  if (!st.ok()) {
    if (!req.responded()) {
      req.sendProblem(st.code, st.detail);
    } else {
      // Response already sent: can't send a problem — close the connection.
      ESP_LOGW(kTag, "handler failed after response: %s",
               controlme::errName(st.code));
      return ESP_FAIL;
    }
  }
  return ESP_OK;
}

esp_err_t HttpServer::onError404(httpd_req_t* r, httpd_err_code_t err) {
  (void)err;
  HttpRequest req(r);
  if (strncmp(r->uri, "/api/", 5) == 0) {
    req.sendProblem(controlme::Err::NotFound, "route not found");
    return ESP_OK;
  }
  httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, nullptr);
  return ESP_OK;
}

controlme::Status HttpServer::begin(uint16_t port) {
  if (hd_) return controlme::Status::Ok();
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = port;
  cfg.core_id = controlme::cfg::kCoreNet;            // wire plane — next to tcpip
  cfg.task_priority = controlme::cfg::kPrioHttpd;
  cfg.stack_size = controlme::cfg::kStackTransport;  // 4 KiB file pump on the stack
  cfg.max_uri_handlers = kMaxRoutes;
  // ~11.5 KiB TCP buffer per socket vs ~10-15 KiB internal free — bound
  // concurrency so demand fits; excess connections are refused fast (clients
  // retry) instead of accepted then starved waiting for buffers.
  // LWIP_MAX_SOCKETS=16: listen×4 (:80/:81/:82/:21) + ctrl×2 + httpd + sse +
  // mjpeg-client + ftp + https-out. Used to be listen×3 + httpd 5 + sse 3;
  // adding :82 without shrinking the pool → ENFILE (errno 23) on accept/TLS.
  // 4 here + sse 2 leaves slots for outbound. Chrome overflow → lru_purge.
  cfg.max_open_sockets = 4;
  // keep-alive ON: a server closing every response piles up TIME_WAIT in
  // the lwIP PCB pool — browser+SPA churn once drained the pool (accept
  // EMFILE, outbound HTTPS clients failed too, HTTP dead until reboot).
  // Reusing connections is far cheaper. Slots squatted by idle connections
  // are bounded by lru_purge (new connections evict the longest-idle) — not
  // by keep_alive_*: those three are TCP_KEEPIDLE/INTVL/CNT socket options,
  // dead-peer probes at the TCP level, NOT app-level idle timeouts.
  cfg.keep_alive_enable = true;
  cfg.keep_alive_idle = 4;       // TCP_KEEPIDLE — start dead-peer probes
  cfg.keep_alive_interval = 1;   // TCP_KEEPINTVL
  cfg.keep_alive_count = 2;      // TCP_KEEPCNT
  cfg.lru_purge_enable = true;   // evict idle so new clients still get in
  cfg.recv_wait_timeout = 5;     // seconds
  cfg.send_wait_timeout = 5;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  esp_err_t err = httpd_start(&hd_, &cfg);
  if (err != ESP_OK) {
    hd_ = nullptr;
    return controlme::Status::Fail(controlme::Err::Internal, "httpd start failed");
  }
  httpd_register_err_handler(hd_, HTTPD_404_NOT_FOUND, &onError404);
  return controlme::Status::Ok();
}

void HttpServer::stop() {
  if (!hd_) return;
  httpd_stop(hd_);
  hd_ = nullptr;
}

controlme::Status HttpServer::addRoute(httpd_method_t method, const char* uri,
                                     HttpHandler handler, void* ctx) {
  if (!hd_ || !uri || !handler) {
    return controlme::Status::Fail(controlme::Err::Internal, "http not started");
  }
  if (routeCount_ >= kMaxRoutes) {
    return controlme::Status::Fail(controlme::Err::QueueFull, "routes full");
  }
  Route& slot = routes_[routeCount_];
  slot = {method, uri, handler, ctx};
  httpd_uri_t u = {};
  u.uri = uri;
  u.method = method;
  u.handler = &bridge;
  u.user_ctx = &slot;
  esp_err_t err = httpd_register_uri_handler(hd_, &u);
  if (err != ESP_OK) {
    return controlme::Status::Fail(
        err == ESP_ERR_HTTPD_HANDLERS_FULL ? controlme::Err::QueueFull
                                           : controlme::Err::Internal,
        "register uri failed");
  }
  ++routeCount_;
  return controlme::Status::Ok();
}

controlme::Status HttpServer::serveStatic(const char* fsRoot) {
  if (!fsRoot || strlen(fsRoot) >= sizeof(fsRoot_)) {
    return controlme::Status::Fail(controlme::Err::InvalidArgument, "fsRoot invalid");
  }
  strncpy(fsRoot_, fsRoot, sizeof(fsRoot_) - 1);
  fsRoot_[sizeof(fsRoot_) - 1] = '\0';
  return addRoute(HTTP_GET, "/*", &staticGet, this);
}

}  // namespace transport
