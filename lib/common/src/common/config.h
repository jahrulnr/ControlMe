#pragma once
// Configuration constants — initial targets from docs/02-architecture.md.
// Values are TUNABLE PROPOSALS, not measurements.
#include <stdint.h>
#include <stddef.h>

namespace controlme::cfg {

// Id/string capacities (bytes, including NUL)
inline constexpr size_t kIdCap = 24;          // actionId, runId, imageId, uploadId, sessionId
inline constexpr size_t kRequestIdCap = 40;
inline constexpr size_t kBootIdCap = 16;
inline constexpr size_t kSourceCap = 24;
inline constexpr size_t kRuleIdCap = 24;
inline constexpr size_t kPathCap = 160;
inline constexpr size_t kPayloadCap = 160;
inline constexpr size_t kToolNameCap = 32;

// Queues (depths)
inline constexpr uint32_t kEventQueueDepth = 32;
inline constexpr uint32_t kStorageQueueDepth = 16;
inline constexpr uint32_t kCameraQueueDepth = 8;
inline constexpr uint32_t kServoQueueDepth = 8;

// Task stacks (BYTES — ESP-IDF convention, not words)
inline constexpr uint32_t kStackTransport = 8 * 1024;
inline constexpr uint32_t kStackStorage = 6 * 1024;
inline constexpr uint32_t kStackCamera = 6 * 1024;
inline constexpr uint32_t kStackServo = 4 * 1024;
inline constexpr uint32_t kStackSupervisor = 4 * 1024;
inline constexpr uint32_t kStackMcpWorker = 6 * 1024;
inline constexpr uint32_t kStackMcpd = 4 * 1024;
inline constexpr uint32_t kStackFileLog = 4 * 1024;

// Dual-core scheduling — the single source of truth for our task
// core+priority. ESP32 convention: core 0 (PRO_CPU) already runs the wifi
// driver (prio ~23), tcpip (~18) and most ISRs; core 1 (APP_CPU) runs the
// Arduino loopTask (prio 1) — we remove our loopTask since loop() is empty.
//
// Core 0 = "wire plane": all tasks whose life is pumping lwIP sockets.
//   Placed next to tcpip so WiFi->socket copies don't cross cores, and
//   always lower priority than the time-sensitive device plane — leftover
//   CPU on core 0 falls to the agent (TLS bursts, mostly blocked on recv).
// Core 1 = "device plane": all hardware owners & domain logic. The camera
//   is the most time-sensitive (20fps frame timing); the supervisor above
//   it so reboot always wins preemption; agent/sim lowest since they're
//   long workers that may be preempted anytime.
//
// ALL cross-plane communication goes through FreeRTOS queues/semaphores
// (the owner-task pattern) — no shared mutable state without a mutex.
//
// Implicit IDF context (not our tasks, for tuning reference):
//   core0: ipc0(24), wifi(23), cam_task(23 — the esp32-camera driver pins
//          it here; DMA frame assembly — the real frame-timing load lives
//          on the wire plane), esp_timer(22 — hosts the reboot-dl
//          callback), sys_evt(20), tiT/tcpip(18), IDLE0(0 — watched by the
//          5s TWDT)
//   core1: ipc1(8), arduino_events(19 — WiFi event dispatch, light load),
//          loopTask(1 — we remove it in loop()), IDLE1(0)
//   no-affinity: Tmr Svc(1, FreeRTOS daemon)
//
// Documented exception: the "socket-pumping tasks live on core0" claim is
// not absolute — a device-plane owner MAY execute a caller's sink I/O in
// its own context (camera Read writes httpd_resp_send_chunk, and agent
// vision uploads via B64Sink, from the prio8 core1 camera task). All those
// paths are deadline-bounded (send_wait_timeout 5s/chunk, facade 30s) →
// worst case an honest Err::Timeout, not a hang. Core0 callers waiting on
// `done` are also deadline-bounded — no unbounded cross-plane wait.
// Plain types (int/unsigned) — this header is also compiled by host tests
// without FreeRTOS; implicit conversion to BaseType_t/UBaseType_t is safe
// at call sites.
inline constexpr int kCoreNet = 0;
inline constexpr int kCoreApp = 1;
inline constexpr unsigned kPrioStorage = 5;     // core1 — FATFS/SD I/O
inline constexpr unsigned kPrioHttpd = 6;       // core0 — httpd :80/:81
inline constexpr unsigned kPrioCoordinator = 6; // core1 — sd/servo arbitration
inline constexpr unsigned kPrioServo = 7;       // core1 — apply PWM/claim pin
inline constexpr unsigned kPrioCamera = 8;      // core1 — frame timing
// Below the camera: these tasks wait on the owner queue. Higher than the
// owner would deadlock frame pulls since the owner would never get CPU.
inline constexpr unsigned kPrioSupervisor = 9;  // core1 — lifecycle arbiter
inline constexpr unsigned kPrioMcpWorker = 5;   // core0 — parse+dispatch MCP
inline constexpr unsigned kPrioFileLog = 4;     // core1 — append+flush (below storage)

// Memory & streaming
inline constexpr size_t kIoChunk = 4096;             // file I/O & base64 per chunk

// MCP server (:81 — McpTransport; transport ganda: Streamable HTTP + SSE legacy)
inline constexpr uint16_t kMcpPort = 81;
// httpd ctrl_port must differ from the default 32768 used by the :80 server.
inline constexpr uint16_t kMcpdCtrlPort = 32769;
inline constexpr uint8_t kMcpSessions = 2;           // legacy SSE session slots
inline constexpr uint8_t kMcpMaxOpenSockets = 4;
// Async sessions (SSE + in-flight POST /mcp) must never fill ALL
// sockets: httpd_accept_conn only purges NON-async sessions (LRU skip
// for_async_req) — 4 async = nothing to evict -> listen fd stays readable
// -> select hot-loop -> core0 livelock -> TASK_WDT (live finding).
inline constexpr uint8_t kMcpAsyncMax = kMcpMaxOpenSockets - 1;
inline constexpr size_t kMcpBodyMax = 16 * 1024;     // body POST /mcp & /messages
inline constexpr size_t kMcpRespCap = 16 * 1024;     // envelope; tool text is escaped 2x inside it
inline constexpr size_t kMcpToolResultCap = 8 * 1024;  // raw DeviceActions JSON result (PSRAM)
// Base64 attachment scratch (PSRAM) — an inline image content block when the
// payload fits; ~6 KiB binary covers QVGA. Bigger frames fall back to the
// downloadUrl/resource_link in the same result.
inline constexpr size_t kMcpAttachCap = 8 * 1024;
inline constexpr uint8_t kMcpJobQueue = 8;
inline constexpr uint32_t kMcpKeepaliveMs = 5000;

// DeviceLog (internal LittleFS — /logs/current.log + prev.log)
// Effective cap computed at begin (after rotation): min(kLogFileCap, freeFs
// - kLogFsReserve); below kLogMinCap writing is disabled (prev stays
// readable) — a log must not drain a nearly-full FS.
inline constexpr size_t kLogFileCap = 100 * 1024;    // upper bound per file
inline constexpr size_t kLogFsReserve = 32 * 1024;   // FS reserve outside the log
inline constexpr size_t kLogMinCap = 8 * 1024;       // minimum cap for writes to stay on
inline constexpr size_t kLogRingCap = 4096;          // static capture ring (internal DRAM)
inline constexpr uint32_t kLogPollMs = 100;          // FileLog owner idle tick
inline constexpr uint32_t kLogFlushMs = 1000;        // periodic flush to FS
inline constexpr size_t kLogWriteBatch = 1024;       // write once this much accumulates
inline constexpr size_t kFrameMaxBytes = 96 * 1024;  // JPEG QVGA budget; more -> IMAGE_TOO_LARGE
inline constexpr size_t kStagingMaxBytes = 96 * 1024;
inline constexpr uint8_t kStagingPoolSize = 2;       // PSRAM staging objects
inline constexpr uint32_t kImageTtlMs = 60 * 1000;   // unreferenced images only
inline constexpr uint8_t kMaxRunAttachments = 2;
inline constexpr size_t kToolArgsMax = 8 * 1024;

// Dedup & retention
inline constexpr uint8_t kDedupCacheEntries = 32;    // requestIds per boot, active entries pinned

// Rule engine
inline constexpr uint8_t kMaxTriggersPerRule = 8;
inline constexpr uint8_t kMaxAncestorRules = 8;

// Timeouts (ms — all tunable)
inline constexpr uint32_t kConnectTimeoutMs = 10 * 1000;
inline constexpr uint32_t kIdleTimeoutMs = 30 * 1000;
inline constexpr uint32_t kToolTimeoutMs = 10 * 1000;
inline constexpr uint32_t kWriteLeaseIdleMs = 15 * 1000;
inline constexpr uint32_t kRebootDeadlineMs = 10 * 1000;
inline constexpr uint32_t kRebootNoticeMs = 2 * 1000;

// Pagination
inline constexpr uint8_t kPageDefault = 20;
inline constexpr uint8_t kPageMax = 100;

}  // namespace controlme::cfg
