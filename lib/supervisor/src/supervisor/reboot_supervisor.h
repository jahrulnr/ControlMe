#pragma once
// RebootSupervisor — implementation of IReboot
// (lib/ports/ports/reboot.h) that runs the docs/03 §Reboot graceful shutdown
// sequence IN its own supervisor task (stack cfg::kStackSupervisor bytes,
// queue 2), not in the caller's task:
//
//   1. requestReboot(): app.admissionOpen=false IMMEDIATELY in the caller's
//      task — new actions/uploads/runs are rejected from that moment; then
//      enqueue the POD. Re-entry while rebooting -> Err::DeviceRebooting
//      (DEVICE_REBOOTING 503).
//   2. Task: runs->cancelAll() (if present) — cancel the run queue +
//      in-flight provider calls (cancel = intent, not undo).
//   3. Grace kOwnerGraceMs (<=1 s) so owners finish bounded chunks, then
//      coord->unmountSd() best-effort (flush/close SD).
//   4. NVS marker ns "partner": reboot_intent=1 + reboot_reason +
//      reboot_reqid -> nvs_commit (best-effort; failure -> UNKNOWN outcome).
//   5. Event EventKind::Custom payload {"rebooting":true,"reason":...} —
//      notice <=cfg::kRebootNoticeMs (<=2 s), NOT a delivery guarantee.
//   6. Total deadline cfg::kRebootDeadlineMs (10 s) armed as a one-shot
//      esp_timer in requestReboot() -> esp_restart() even if graceful steps
//      hang; forced -> UNKNOWN outcome (docs/03).
//
// The NVS marker is read ONCE at boot by the integrator (main.cpp) via
// readRebootMarker() — the supervisor never calls it itself; the marker is
// cleared after being read. Marker failure -> previous request UNKNOWN.
// Remounting SD before servo PWM is boot's business (docs/03 step 7), not
// this module's.
#include <stddef.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include "common/config.h"
#include "common/error.h"
#include "ports/reboot.h"

// app.h lives in src/ (not a lib): resolved via __has_include with a
// relative-path fallback — same convention as owner_task.h in the
// storage/servo/camera libs. App holds transport::HttpServer/McpTransport
// by-value, so the transport headers are included explicitly here so
// PlatformIO's LDF adds the transport lib to the supervisor build's include
// path.
#include "transport/http_server.h"
#include "transport/mcp_transport.h"
#if __has_include("app.h")
#include "app.h"
#else
#include "../../../../src/app.h"
#endif

namespace controlme {

class RebootSupervisor : public IReboot {
 public:
  explicit RebootSupervisor(App& app);
  RebootSupervisor(const RebootSupervisor&) = delete;
  RebootSupervisor& operator=(const RebootSupervisor&) = delete;

  // Create queue + supervisor task + deadline timer. Once, from register.
  Status begin();

  // --- IReboot: called from other tasks (httpd/agent) — nonblocking ---
  Status requestReboot(const char* reason) override {
    return requestReboot(reason, nullptr);
  }
  // Variant with requestId (HTTP endpoint): recorded into the NVS marker
  // for next-boot reconciliation; empty reason -> "unspecified".
  Status requestReboot(const char* reason, const char* requestId);
  bool isRebooting() override { return rebooting_; }

  // Called by the integrator ONCE at boot (main.cpp) — NOT from here.
  // No marker -> false and out is emptied; a marker -> true, out is filled
  // (truncation-safe), then the marker is erased so it's read only once.
  static bool readRebootMarker(char* reasonOut, size_t reasonCap,
                               char* requestIdOut = nullptr,
                               size_t requestIdCap = 0);

 private:
  // POD through the queue — values, not pointers (no heap/use-after-free).
  struct RebootReq {
    char reason[64];
    char requestId[cfg::kRequestIdCap];
  };
  static constexpr uint32_t kQueueDepth = 2;
  static constexpr uint32_t kOwnerGraceMs = 1000;  // docs/03: "wait <=1 s"

  static void taskEntry(void* arg);
  void loop();
  void runShutdown(const RebootReq& req);  // ends with esp_restart — never returns
  void writeMarker(const RebootReq& req);
  void publishRebooting(const RebootReq& req);
  static void deadlineThunk(void* arg);    // esp_timer task -> esp_restart()

  App& app_;
  QueueHandle_t queue_ = nullptr;
  TaskHandle_t task_ = nullptr;
  esp_timer_handle_t deadlineTimer_ = nullptr;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;  // check-set rebooting_
  volatile bool rebooting_ = false;
};

}  // namespace controlme
