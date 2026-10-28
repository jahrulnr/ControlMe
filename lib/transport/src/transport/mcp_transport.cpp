#include "transport/mcp_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include "common/psram_alloc.h"
#include "mcpproto/str_sink.h"
#include "transport/http_server.h"

namespace transport {

using controlme::Err;
using controlme::Status;
namespace cfg = controlme::cfg;
using controlme::mcp::StrSink;

static const char* kTag = "mcp";

// Stack low-water on the deepest path; print only on a new minimum.
// MIN LATCH PER CALL SITE — a single shared static hides tasks with a higher
// hwm (finding: "stack mcpw" never printed because mcpd had already lowered
// the shared min).
static void stackHwmNote(uint32_t& min, const char* name) {
  const uint32_t h = uxTaskGetStackHighWaterMark(nullptr);
  if (h < min) {
    min = h;
    ESP_LOGI(kTag, "stack %s hwm=%u", name, (unsigned)h);
  }
}
static uint32_t s_hwmMcpd = UINT32_MAX;
static uint32_t s_hwmMcpw = UINT32_MAX;
static uint32_t s_hwmMcpka = UINT32_MAX;
static inline void noteMcpd() { stackHwmNote(s_hwmMcpd, "mcpd"); }
static inline void noteMcpw() { stackHwmNote(s_hwmMcpw, "mcpw"); }
static inline void noteMcpka() { stackHwmNote(s_hwmMcpka, "mcpka"); }

// ---------------------------------------------------------------- util

// Origin -> host (strip scheme+port); empty host -> false.
static bool hostOf(const char* s, char* out, size_t cap) {
  const char* scheme = strstr(s, "://");
  if (scheme) s = scheme + 3;
  size_t i = 0;
  while (s[i] && s[i] != ':' && s[i] != '/' && i + 1 < cap) {
    out[i] = s[i];
    ++i;
  }
  out[i] = '\0';
  return i > 0;
}

// MUST spec: Origin (when present) must have the same host as the Host header.
bool McpTransport::originOk(httpd_req_t* r) {
  HttpRequest req(r);
  char origin[96];
  if (!req.header("Origin", origin, sizeof(origin))) return true;
  char hostHdr[96], oh[64], hh[64];
  if (!req.header("Host", hostHdr, sizeof(hostHdr)) ||
      !hostOf(origin, oh, sizeof(oh)) ||
      !hostOf(hostHdr, hh, sizeof(hh)) ||
      strcasecmp(oh, hh) != 0) {
    req.sendBuf(403, "text/plain", "origin not allowed", 18);
    return false;
  }
  return true;
}

// Whole body -> PSRAM (len+1, NUL). Small retry on SOCK_ERR_TIMEOUT.
char* McpTransport::readBody(httpd_req_t* r, size_t len) {
  char* buf = static_cast<char*>(controlme::psramMalloc(len + 1));
  if (!buf) return nullptr;
  size_t got = 0;
  int retries = 3;
  while (got < len) {
    int n = httpd_req_recv(r, buf + got, len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) {
      if (--retries <= 0) break;
      continue;
    }
    if (n <= 0) {
      controlme::psramFree(buf);
      return nullptr;
    }
    got += (size_t)n;
  }
  if (got < len) {
    controlme::psramFree(buf);
    return nullptr;
  }
  buf[len] = '\0';
  return buf;
}

bool McpTransport::tryReserveAsync() {
  bool ok = false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (asyncInUse_ < cfg::kMcpAsyncMax) {
    ++asyncInUse_;
    ok = true;
  }
  xSemaphoreGive(mutex_);
  return ok;
}

void McpTransport::releaseAsync() {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (asyncInUse_ > 0) --asyncInUse_;
  xSemaphoreGive(mutex_);
}

// ---------------------------------------------------------------- begin

Status McpTransport::begin(uint16_t port, controlme::mcp::McpServer& server) {
  if (hd_) return Status::Ok();
  server_ = &server;
  respBuf_ = static_cast<char*>(controlme::psramMalloc(cfg::kMcpRespCap));
  mutex_ = xSemaphoreCreateMutex();
  sendMux_ = xSemaphoreCreateMutex();
  jobQ_ = xQueueCreate(cfg::kMcpJobQueue, sizeof(Job));
  if (!respBuf_ || !mutex_ || !sendMux_ || !jobQ_) {
    if (mutex_) vSemaphoreDelete(mutex_);
    if (sendMux_) vSemaphoreDelete(sendMux_);
    if (jobQ_) vQueueDelete(jobQ_);
    controlme::psramFree(respBuf_);
    respBuf_ = nullptr;
    mutex_ = nullptr;
    sendMux_ = nullptr;
    jobQ_ = nullptr;
    return Status::Fail(Err::Internal, "mcp alloc");
  }

  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.server_port = port;
  c.core_id = cfg::kCoreNet;
  c.task_priority = cfg::kPrioHttpd;
  // httpd ctrl_port default 32768 is already used by the :80 server — must differ.
  c.ctrl_port = cfg::kMcpdCtrlPort;
  c.stack_size = cfg::kStackMcpd;         // light handlers: body->PSRAM+enqueue
  c.max_open_sockets = cfg::kMcpMaxOpenSockets;
  // LRU is safe in IDF 5.5: sessions with an async req (SSE, in-flight
  // POST) aren't considered idle -> won't be purged when sockets fill. BUT
  // precisely because of that, httpd_accept_conn can fail outright when ALL
  // slots are async-held: close_lru finds no candidate -> accept never runs
  // -> listen fd stays readable -> select hot-loop (livelock). The
  // kMcpAsyncMax budget guarantees >=1 non-async session is always
  // evictable.
  c.lru_purge_enable = true;
  c.max_uri_handlers = 6;
  c.recv_wait_timeout = 5;
  c.send_wait_timeout = 2;
  c.global_user_ctx = this;
  // Without free_fn, httpd_stop() calls free(global_user_ctx) — `this`
  // lives in .bss (an App member); free would panic on any failure path.
  c.global_user_ctx_free_fn = [](void*) {};
  c.close_fn = &closeHook;

  esp_err_t err = httpd_start(&hd_, &c);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "httpd_start failed err=%d — internal free=%u largest=%u",
             (int)err,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                               MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT));
    hd_ = nullptr;
    vSemaphoreDelete(mutex_);
    vSemaphoreDelete(sendMux_);
    vQueueDelete(jobQ_);
    controlme::psramFree(respBuf_);
    mutex_ = nullptr;
    sendMux_ = nullptr;
    jobQ_ = nullptr;
    respBuf_ = nullptr;
    return Status::Fail(Err::Internal, "mcp httpd start");
  }

  static const struct {
    const char* uri;
    httpd_method_t m;
    esp_err_t (*h)(httpd_req_t*);
  } routes[] = {
      {"/mcp", HTTP_POST, &hPostMcp},     {"/mcp", HTTP_GET, &hGetMcp},
      {"/mcp", HTTP_DELETE, &hDeleteMcp}, {"/sse", HTTP_GET, &hGetSse},
      {"/messages", HTTP_POST, &hPostMessages},
  };
  for (const auto& rt : routes) {
    httpd_uri_t u = {};
    u.uri = rt.uri;
    u.method = rt.m;
    u.handler = rt.h;
    u.user_ctx = this;
    if (httpd_register_uri_handler(hd_, &u) != ESP_OK) {
      ESP_LOGE(kTag, "route %s failed", rt.uri);
      goto fail_started;
    }
  }
  if (xTaskCreatePinnedToCore(&workerEntry, "mcpw", cfg::kStackMcpWorker,
                              this, cfg::kPrioMcpWorker, &worker_,
                              cfg::kCoreNet) != pdPASS) {
    goto fail_started;
  }
  // Keepalive/reap on their own task — a long tool call in the worker must
  // not starve SSE liveness or hold dead session slots.
  if (xTaskCreatePinnedToCore(&kaEntry, "mcpka", cfg::kStackMcpKeepalive,
                              this, cfg::kPrioMcpWorker, &kaTask_,
                              cfg::kCoreNet) != pdPASS) {
    goto fail_started;
  }
  ESP_LOGI(kTag, "mcp :%u up (streamable /mcp + legacy /sse,/messages)",
           (unsigned)port);
  return Status::Ok();

fail_started:
  // vTaskDelete(nullptr) deletes the CALLER — guard. Tasks die before the
  // queue/mutexes they may be blocked on; mutex_ must outlive httpd_stop
  // (closeHook fires inside it).
  if (kaTask_) { vTaskDelete(kaTask_); kaTask_ = nullptr; }
  if (worker_) { vTaskDelete(worker_); worker_ = nullptr; }
  httpd_stop(hd_);
  hd_ = nullptr;
  vSemaphoreDelete(mutex_);
  vSemaphoreDelete(sendMux_);
  vQueueDelete(jobQ_);
  controlme::psramFree(respBuf_);
  mutex_ = nullptr;
  sendMux_ = nullptr;
  jobQ_ = nullptr;
  respBuf_ = nullptr;
  return Status::Fail(Err::Internal, "mcp route/worker");
}

// ---------------------------------------------------------------- POST /mcp

esp_err_t McpTransport::hPostMcp(httpd_req_t* r) {
  McpTransport* self = static_cast<McpTransport*>(r->user_ctx);
  if (!self->originOk(r)) return ESP_OK;
  HttpRequest req(r);
  if (r->content_len > cfg::kMcpBodyMax) {
    req.sendJson(413,
                 "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,"
                 "\"message\":\"body too large\"}}");
    return ESP_OK;
  }
  // The httpd task is the sole producer -> check-then-send is race-free.
  if (uxQueueSpacesAvailable(self->jobQ_) == 0) {
    req.sendProblem(Err::QueueFull, "mcp job queue full");
    return ESP_OK;
  }
  char* body = readBody(r, r->content_len);
  if (!body) {
    req.sendProblem(Err::Internal, "body read failed");
    return ESP_OK;
  }
  if (!self->tryReserveAsync()) {
    controlme::psramFree(body);
    req.sendProblem(Err::QueueFull, "mcp busy");
    return ESP_OK;
  }
  httpd_req_t* async = nullptr;
  if (httpd_req_async_handler_begin(r, &async) != ESP_OK) {
    self->releaseAsync();
    controlme::psramFree(body);
    req.sendProblem(Err::Internal, "async begin failed");
    return ESP_OK;
  }
  Job j{Job::Kind::Http, async, {}, body, r->content_len};
  if (xQueueSend(self->jobQ_, &j, 0) != pdTRUE) {
    httpd_req_async_handler_complete(async);
    self->releaseAsync();
    controlme::psramFree(body);
    req.sendProblem(Err::QueueFull, "mcp job queue full");
    return ESP_OK;
  }
  noteMcpd();
  return ESP_OK;
}

esp_err_t McpTransport::hGetMcp(httpd_req_t* r) {
  HttpRequest req(r);
  req.setHeader("Allow", "POST");
  req.sendBuf(405, "text/plain", "method not allowed", 18);
  return ESP_OK;
}

esp_err_t McpTransport::hDeleteMcp(httpd_req_t* r) {
  return hGetMcp(r);
}

// ------------------------------------------------------------------ GET /sse

esp_err_t McpTransport::hGetSse(httpd_req_t* r) {
  McpTransport* self = static_cast<McpTransport*>(r->user_ctx);
  if (!self->originOk(r)) return ESP_OK;
  HttpRequest req(r);

  int idx = -1;
  xSemaphoreTake(self->mutex_, portMAX_DELAY);
  for (int i = 0; i < cfg::kMcpSessions; ++i) {
    if (self->sessions_[i].state == SlotState::Free) {
      self->sessions_[i].state = SlotState::Reserved;
      idx = i;
      break;
    }
  }
  xSemaphoreGive(self->mutex_);
  if (idx < 0) {
    req.sendProblem(Err::QueueFull, "MCP SSE session full");
    return ESP_OK;
  }
  Session& s = self->sessions_[idx];
  if (!self->tryReserveAsync()) {
    xSemaphoreTake(self->mutex_, portMAX_DELAY);
    s.state = SlotState::Free;
    xSemaphoreGive(self->mutex_);
    req.sendProblem(Err::QueueFull, "mcp busy");
    return ESP_OK;
  }

  httpd_req_t* async = nullptr;
  if (httpd_req_async_handler_begin(r, &async) != ESP_OK) {
    self->releaseAsync();
    xSemaphoreTake(self->mutex_, portMAX_DELAY);
    s.state = SlotState::Free;
    xSemaphoreGive(self->mutex_);
    req.sendProblem(Err::Internal, "async begin failed");
    return ESP_OK;
  }
  char sid[12];
  snprintf(sid, sizeof(sid), "s-%08lx", (unsigned long)esp_random());
  char frame[80];
  int n = snprintf(frame, sizeof(frame),
                   "event: endpoint\ndata: /messages?session_id=%s\n\n", sid);
  httpd_resp_set_type(async, "text/event-stream");
  httpd_resp_set_hdr(async, "Cache-Control", "no-cache");
  if (n <= 0 || httpd_resp_send_chunk(async, frame, (ssize_t)n) != ESP_OK) {
    httpd_req_async_handler_complete(async);
    self->releaseAsync();
    xSemaphoreTake(self->mutex_, portMAX_DELAY);
    s.state = SlotState::Free;
    xSemaphoreGive(self->mutex_);
    return ESP_OK;
  }
  const int fd = httpd_req_to_sockfd(r);
  // A silently dead peer would hold one of the 2 slots for minutes
  // without TCP keepalive — idle 10s / interval 5s / 3 probes -> dead
  // ~25 s.
  {
    int one = 1, idle = 10, intvl = 5, cnt = 3;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
  }
  xSemaphoreTake(self->mutex_, portMAX_DELAY);
  s.async = async;
  s.sockfd = fd;
  memcpy(s.sid, sid, sizeof(s.sid));
  s.state = SlotState::Live;
  xSemaphoreGive(self->mutex_);
  noteMcpd();
  return ESP_OK;
}

// ------------------------------------------------------------ POST /messages

esp_err_t McpTransport::hPostMessages(httpd_req_t* r) {
  McpTransport* self = static_cast<McpTransport*>(r->user_ctx);
  if (!self->originOk(r)) return ESP_OK;
  HttpRequest req(r);

  char sid[12] = {};
  if (!req.query("session_id", sid, sizeof(sid)) || sid[0] == '\0') {
    req.sendProblem(Err::NotFound, "unknown session");
    return ESP_OK;
  }
  bool known = false;
  xSemaphoreTake(self->mutex_, portMAX_DELAY);
  for (const auto& s : self->sessions_) {
    if (s.state == SlotState::Live && strcmp(s.sid, sid) == 0) known = true;
  }
  xSemaphoreGive(self->mutex_);
  if (!known) {
    req.sendProblem(Err::NotFound, "unknown session");
    return ESP_OK;
  }
  if (r->content_len > cfg::kMcpBodyMax) {
    req.sendJson(413,
                 "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,"
                 "\"message\":\"body too large\"}}");
    return ESP_OK;
  }
  if (uxQueueSpacesAvailable(self->jobQ_) == 0) {
    req.sendProblem(Err::QueueFull, "mcp job queue full");
    return ESP_OK;
  }
  char* body = readBody(r, r->content_len);
  if (!body) {
    req.sendProblem(Err::Internal, "body read failed");
    return ESP_OK;
  }
  Job j{Job::Kind::Sse, nullptr, {}, body, r->content_len};
  memcpy(j.sid, sid, sizeof(j.sid));
  if (xQueueSend(self->jobQ_, &j, 0) != pdTRUE) {
    controlme::psramFree(body);
    req.sendProblem(Err::QueueFull, "mcp job queue full");
    return ESP_OK;
  }
  req.sendBuf(202, "text/plain", "Accepted", 8);
  noteMcpd();
  return ESP_OK;
}

// ------------------------------------------------------- lifecycle & worker

void McpTransport::closeHook(httpd_handle_t hd, int sockfd) {
  McpTransport* self =
      static_cast<McpTransport*>(httpd_get_global_user_ctx(hd));
  if (!self) return;
  xSemaphoreTake(self->mutex_, portMAX_DELAY);
  for (auto& s : self->sessions_) {
    if (s.sockfd == sockfd && s.state == SlotState::Live) {
      s.state = SlotState::DeadByPeer;
    }
  }
  xSemaphoreGive(self->mutex_);
  // IDF 5.5 httpd_sess.c: a configured close_fn REPLACES close() —
  // without this the socket leaks (a bug not to be copied from SseServer).
  close(sockfd);
}

void McpTransport::reapDead() {
  for (auto& s : sessions_) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const SlotState st = s.state;
    httpd_req_t* async = s.async;
    const int fd = s.sockfd;
    if (st == SlotState::DeadByPeer || st == SlotState::DeadByUs) {
      s.async = nullptr;
      s.sockfd = -1;
      s.sid[0] = '\0';
      s.state = SlotState::Free;
    }
    xSemaphoreGive(mutex_);
    if (st != SlotState::DeadByPeer && st != SlotState::DeadByUs) continue;
    // sendMux_: a sender may be mid-send on this async — completing it
    // must wait for that send (senders re-check state inside the hold, so
    // a slot already Free is skipped and the complete can't interleave).
    xSemaphoreTake(sendMux_, portMAX_DELAY);
    if (st == SlotState::DeadByUs) {
      // shutdown first: httpd sees EOF then closes via close_fn — an fd
      // reused by a new connection isn't killed along (trigger_close
      // per-fd-number can't tell them apart).
      if (fd >= 0) shutdown(fd, SHUT_RDWR);
    }
    if (async) {
      httpd_req_async_handler_complete(async);
      releaseAsync();
    }
    xSemaphoreGive(sendMux_);
  }
}

void McpTransport::processJob(const Job& j) {
  StrSink out(respBuf_, cfg::kMcpRespCap);
  const bool responded = server_->handle(j.body, j.len, out);
  controlme::psramFree(j.body);

  if (j.kind == Job::Kind::Http) {
    if (responded) {
      httpd_resp_set_type(j.async, "application/json");
      httpd_resp_send(j.async, out.data(), (ssize_t)out.len());
    } else {
      httpd_resp_set_status(j.async, "202 Accepted");
      httpd_resp_send(j.async, nullptr, 0);
    }
    httpd_req_async_handler_complete(j.async);
    releaseAsync();
    noteMcpw();
    return;
  }

  // Sse: response -> "event: message" to a session still Live (sid must
  // match — the slot may already be reused by a new session).
  if (!responded) return;
  // sendMux_: the 3-chunk frame stays atomic vs a ": ka" chunk from mcpka,
  // and reapDead can't complete the async req mid-send. The Live check is
  // re-done INSIDE the hold (mutex_ nested only) — checking first and
  // sending later would leave the complete-mid-send window open.
  xSemaphoreTake(sendMux_, portMAX_DELAY);
  xSemaphoreTake(mutex_, portMAX_DELAY);
  Session* s = nullptr;
  for (auto& c : sessions_) {
    if (c.state == SlotState::Live && strcmp(c.sid, j.sid) == 0) s = &c;
  }
  httpd_req_t* async = s ? s->async : nullptr;
  xSemaphoreGive(mutex_);
  if (s && async) {
    static const char kMsgHead[] = "event: message\ndata: ";
    if (httpd_resp_send_chunk(async, kMsgHead, sizeof(kMsgHead) - 1) != ESP_OK ||
        httpd_resp_send_chunk(async, out.data(), (ssize_t)out.len()) != ESP_OK ||
        httpd_resp_send_chunk(async, "\n\n", 2) != ESP_OK) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      // The slot may have been reaped+reused while we sent — only mark
      // dead when it still carries the async req we failed on.
      if (s->state == SlotState::Live && s->async == async) {
        s->state = SlotState::DeadByUs;
      }
      xSemaphoreGive(mutex_);
    }
  }
  xSemaphoreGive(sendMux_);
}

void McpTransport::keepaliveTick(uint32_t nowMs) {
  if (nowMs - lastKaMs_ < cfg::kMcpKeepaliveMs) return;
  lastKaMs_ = nowMs;
  for (auto& s : sessions_) {
    // sendMux_ (outer) + mutex_ (inner): state re-check and send stay
    // atomic vs reapDead's complete; never sent on a completed async req.
    xSemaphoreTake(sendMux_, portMAX_DELAY);
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool live = s.state == SlotState::Live;
    httpd_req_t* async = s.async;
    xSemaphoreGive(mutex_);
    if (live && async &&
        httpd_resp_send_chunk(async, ": ka\n\n", sizeof(": ka\n\n") - 1) !=
            ESP_OK) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
      if (s.state == SlotState::Live && s.async == async) {
        s.state = SlotState::DeadByUs;
      }
      xSemaphoreGive(mutex_);
    }
    xSemaphoreGive(sendMux_);
  }
}

void McpTransport::workerEntry(void* arg) {
  static_cast<McpTransport*>(arg)->workerLoop();
}

void McpTransport::kaEntry(void* arg) {
  static_cast<McpTransport*>(arg)->kaLoop();
}

void McpTransport::workerLoop() {
  for (;;) {
    Job j{};
    if (xQueueReceive(jobQ_, &j, portMAX_DELAY) == pdTRUE) processJob(j);
  }
}

// Dedicated keepalive/reaper — keepalives and dead-slot reaping must keep
// running while the worker sits inside a long synchronous tool dispatch.
void McpTransport::kaLoop() {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(250));
    reapDead();
    keepaliveTick((uint32_t)(esp_timer_get_time() / 1000ULL));
    noteMcpka();
  }
}

}  // namespace transport
