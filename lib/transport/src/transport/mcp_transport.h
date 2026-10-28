#pragma once
// McpTransport — MCP server :81 on a second esp_http_server (separate
// from :80 so long-lived SSE/POST sessions don't block REST). Two
// transports:
//   - Streamable HTTP: POST /mcp -> 200 application/json (stateless).
//   - Legacy HTTP+SSE: GET /sse (endpoint frame) + POST /messages -> 202,
//     the following response via "event: message" on the session stream.
// httpd handlers (kStackMcpd stack) only read the body into a PSRAM buffer +
// enqueue; parse/dispatch/serialize runs in the worker (core0,
// kPrioMcpWorker) — the sole sender on the session socket. Slot lifecycle
// follows the SseServer pattern: Reserved/Live/DeadByPeer/DeadByUs + reap;
// the ": ka" keepalive detects peer-FIN that close_fn doesn't catch for
// async sockets.
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_http_server.h>
#include "common/config.h"
#include "common/error.h"
#include "mcpproto/mcp_dispatch.h"

namespace transport {

class McpTransport {
 public:
  McpTransport() = default;
  McpTransport(const McpTransport&) = delete;
  McpTransport& operator=(const McpTransport&) = delete;

  controlme::Status begin(uint16_t port, controlme::mcp::McpServer& server);
  bool running() const { return hd_ != nullptr; }

 private:
  enum class SlotState : uint8_t {
    Free = 0,
    Reserved,     // /sse handler claimed the slot, async not attached yet
    Live,
    DeadByPeer,   // close_fn: socket closed by httpd
    DeadByUs,     // send failed/timeout: we close
  };
  struct Session {
    httpd_req_t* async = nullptr;
    int sockfd = -1;
    char sid[12] = {};          // "s-" + 8 hex
    SlotState state = SlotState::Free;
  };
  struct Job {
    enum class Kind : uint8_t { Http, Sse } kind;
    httpd_req_t* async = nullptr;  // kind==Http: async req awaiting the response
    char sid[12] = {};             // kind==Sse: destination session
    char* body = nullptr;          // PSRAM, freed by the worker
    size_t len = 0;
  };

  static esp_err_t hPostMcp(httpd_req_t* r);
  static esp_err_t hGetMcp(httpd_req_t* r);
  static esp_err_t hDeleteMcp(httpd_req_t* r);
  static esp_err_t hGetSse(httpd_req_t* r);
  static esp_err_t hPostMessages(httpd_req_t* r);
  static void closeHook(httpd_handle_t hd, int sockfd);
  static void workerEntry(void* arg);
  void workerLoop();
  void reapDead();
  void processJob(const Job& j);
  void keepaliveTick(uint32_t nowMs);
  bool originOk(httpd_req_t* r);
  static char* readBody(httpd_req_t* r, size_t len);  // PSRAM len+1, NUL-term
  bool tryReserveAsync();   // mutex-guarded: unit available -> ++ & true
  void releaseAsync();      // counterpart of tryReserveAsync, called EVERY complete

  httpd_handle_t hd_ = nullptr;
  TaskHandle_t worker_ = nullptr;
  QueueHandle_t jobQ_ = nullptr;
  SemaphoreHandle_t mutex_ = nullptr;  // session registry only; not during send
  Session sessions_[controlme::cfg::kMcpSessions] = {};
  // Async units in use (mutex_-guarded) — max kMcpAsyncMax so there's
  // always >=1 non-async session for LRU evict.
  uint8_t asyncInUse_ = 0;
  controlme::mcp::McpServer* server_ = nullptr;
  char* respBuf_ = nullptr;            // PSRAM kMcpRespCap, one per worker
  uint32_t lastKaMs_ = 0;
};

}  // namespace transport
