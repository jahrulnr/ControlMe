#pragma once
// Single-owner pattern (docs/02-architecture.md,
// docs/04-code-principles.md): one FreeRTOS task owns one resource; other
// callers NEVER touch the resource directly — they send requests via xQueue
// and wait on a deadline-bounded `done` SemaphoreHandle_t.
//   - No owner mutex held while waiting on long network/IO.
//   - No synchronous cross-task callbacks: completion = xSemaphoreGive on
//     the request's own semaphore.
//   - Requests are heap-allocated so they stay valid even when the caller
//     times out first; ownership passes to the owner (state=abandoned) so
//     there's no use-after-free and no leak.
//
// Module usage (e.g. Storage/Camera/Servo):
//   struct MyReq {                       // MUST be trivially copyable (POD)
//     controlme::OwnerCtrl ctrl;           // MUST be the first member, leave unset
//     // ... input parameters ...
//     // ... result fields, filled by the handler ...
//   };
//   static void myHandler(MyReq& r, void* ctx) {
//     r.ctrl.status = controlme::Status::Ok();   // or Fail(...)
//   }
//   static controlme::OwnerTask<MyReq> g_myOwner;
//   g_myOwner.start({.name="my", .stackBytes=controlme::cfg::kStackStorage,
//                    .priority=5, .queueDepth=controlme::cfg::kStorageQueueDepth,
//                    .handle=&myHandler, .ctx=&myState});
//   // Thread-safe facade — called from other tasks (HTTP handlers etc.):
//   MyReq in{}, out{}; in.path/... = ...;
//   controlme::Status st = g_myOwner.call(in, out, controlme::cfg::kToolTimeoutMs);

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stddef.h>
#include <type_traits>
#include <esp_log.h>
#include "common/error.h"
#include "common/config.h"
#include "common/psram_alloc.h"

namespace controlme {

// Control block that MUST be the first member of every owner request.
struct OwnerCtrl {
  SemaphoreHandle_t done = nullptr;
  volatile uint8_t state = 0;  // 0=pending, 1=done, 2=abandoned
  Status status = Status::Ok();
};

enum : uint8_t { kOwnerPending = 0, kOwnerDone = 1, kOwnerAbandoned = 2 };

template <typename TReq>
struct OwnerTaskConfig {
  const char* name;
  uint32_t stackBytes;                 // BYTES, ESP-IDF convention (not words)
  UBaseType_t priority;
  uint32_t queueDepth;                 // see cfg::k*QueueDepth
  void (*handle)(TReq& req, void* ctx);  // runs in the owner task
  void* ctx = nullptr;
  BaseType_t core = controlme::cfg::kCoreApp;  // owner = device plane (core1)
  // Optional idle hook: called each loop iteration (request done OR
  // receive timeout idleMs). For the owner's periodic work (buffer drain,
  // flush) — keep it short; not a priority mechanism.
  uint32_t idleMs = 0;                     // 0 = no tick (portMAX_DELAY)
  void (*idle)(void* ctx) = nullptr;       // ctx = cfg.ctx
};

template <typename TReq>
class OwnerTask {
  static_assert(std::is_trivially_copyable<TReq>::value,
                "OwnerTask request must be POD/trivially copyable");
  static_assert(offsetof(TReq, ctrl) == 0,
                "OwnerCtrl ctrl must be the first request member");

 public:
  OwnerTask() = default;
  OwnerTask(const OwnerTask&) = delete;
  OwnerTask& operator=(const OwnerTask&) = delete;

  Status start(const OwnerTaskConfig<TReq>& cfg) {
    if (!cfg.handle || cfg.queueDepth == 0 || cfg.stackBytes == 0) {
      return Status::Fail(Err::InvalidArgument, "owner cfg invalid");
    }
    cfg_ = cfg;
    queue_ = xQueueCreate(cfg_.queueDepth, sizeof(TReq*));
    if (!queue_) return Status::Fail(Err::Internal, "owner queue alloc");
    mux_ = xSemaphoreCreateMutex();
    if (!mux_) {
      vQueueDelete(queue_);
      queue_ = nullptr;
      return Status::Fail(Err::Internal, "owner mux alloc");
    }
    if (xTaskCreatePinnedToCore(&taskEntry, cfg_.name, cfg_.stackBytes, this,
                                cfg_.priority, &task_, cfg_.core) != pdPASS) {
      // Partial failure: don't leave a queue/mux with no consumer — the
      // facade would think the owner is ready and requests would hang until
      // timeout (review finding: call() only checks queue_/mux_ non-null).
      vSemaphoreDelete(mux_);
      mux_ = nullptr;
      vQueueDelete(queue_);
      queue_ = nullptr;
      return Status::Fail(Err::Internal, "owner task create");
    }
    return Status::Ok();
  }

  // Facade: copy `in` to heap, enqueue, wait on deadline-bounded done.
  // Result available in `out` (including out.ctrl.status). Full queue ->
  // QueueFull immediately; deadline -> Timeout and the request is handed to
  // the owner (the owner frees it when done).
  Status call(const TReq& in, TReq& out, uint32_t timeoutMs) {
    if (!queue_ || !mux_) {
      return Status::Fail(Err::Internal, "owner not started");
    }
    TReq* r = controlme::psramNew<TReq>(in);  // PSRAM-first: internal is reserved
    if (!r) return Status::Fail(Err::Internal, "owner req alloc");
    r->ctrl = OwnerCtrl{};
    // The handler MUST write ctrl.status; default-fail prevents fake success.
    r->ctrl.status = Status::Fail(Err::Internal, "owner: unset result");
    r->ctrl.done = xSemaphoreCreateBinary();
    if (!r->ctrl.done) {
      controlme::psramDelete(r);
      return Status::Fail(Err::Internal, "owner sem alloc");
    }
    if (xQueueSend(queue_, &r, 0) != pdTRUE) {
      vSemaphoreDelete(r->ctrl.done);
      controlme::psramDelete(r);
      noteOverflow();
      return Status::Fail(Err::QueueFull, "owner queue full");
    }
    if (xSemaphoreTake(r->ctrl.done, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {
      out = *r;
      Status st = r->ctrl.status;
      vSemaphoreDelete(r->ctrl.done);
      controlme::psramDelete(r);
      return st;
    }
    // Deadline passed: race for abandoned status. If the owner won first,
    // its give must be coming -> wait and return the normal result.
    xSemaphoreTake(mux_, portMAX_DELAY);
    if (r->ctrl.state == kOwnerPending) r->ctrl.state = kOwnerAbandoned;
    uint8_t st8 = r->ctrl.state;
    xSemaphoreGive(mux_);
    if (st8 == kOwnerAbandoned) {
      return Status::Fail(Err::Timeout, "owner timeout");
    }
    xSemaphoreTake(r->ctrl.done, portMAX_DELAY);
    out = *r;
    Status st = r->ctrl.status;
    vSemaphoreDelete(r->ctrl.done);
    controlme::psramDelete(r);
    return st;
  }

  uint32_t overflowCount() const { return overflow_; }

 private:
  static void taskEntry(void* arg) { static_cast<OwnerTask*>(arg)->loop(); }

  void loop() {
    const TickType_t wait =
        (cfg_.idle && cfg_.idleMs) ? pdMS_TO_TICKS(cfg_.idleMs) : portMAX_DELAY;
    for (;;) {
      TReq* r = nullptr;
      if (xQueueReceive(queue_, &r, wait) != pdTRUE || !r) {
        // Receive timeout: the owner's idle work (drain etc.) — safe since
        // no in-flight request is waiting.
        if (cfg_.idle) cfg_.idle(cfg_.ctx);
        continue;
      }
      cfg_.handle(*r, cfg_.ctx);
      xSemaphoreTake(mux_, portMAX_DELAY);
      if (r->ctrl.state == kOwnerAbandoned) {
        // Caller already timed out: the owner is responsible for freeing.
        xSemaphoreGive(mux_);
        vSemaphoreDelete(r->ctrl.done);
        controlme::psramDelete(r);
        continue;
      }
      r->ctrl.state = kOwnerDone;
      xSemaphoreGive(mux_);
      xSemaphoreGive(r->ctrl.done);
      // Idle AFTER done is given — a waiting facade must not pay for
      // another request's drain/truncate (review finding: Read/Flush could
      // time out spuriously when idle ran before the give).
      if (cfg_.idle) cfg_.idle(cfg_.ctx);
    }
  }

  // Overflow summarized at most once per 5 s (docs/02 Overflow & Observability).
  void noteOverflow() {
    ++overflow_;
    const uint32_t now = esp_log_timestamp();
    if (now - lastOverflowLog_ >= 5000) {
      lastOverflowLog_ = now;
      ESP_LOGW(cfg_.name ? cfg_.name : "owner", "queue overflow, total=%lu",
               (unsigned long)overflow_);
    }
  }

  QueueHandle_t queue_ = nullptr;
  SemaphoreHandle_t mux_ = nullptr;   // only for short state transitions
  TaskHandle_t task_ = nullptr;
  OwnerTaskConfig<TReq> cfg_ = {};
  uint32_t overflow_ = 0;
  uint32_t lastOverflowLog_ = 0;
};

}  // namespace controlme
