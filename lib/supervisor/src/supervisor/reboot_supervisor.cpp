// RebootSupervisor implementation — see the header for the docs/03 §Reboot
// sequence. All graceful steps run in the supervisor task; callers
// (httpd/agent) only close admission + enqueue and return immediately.
#include "supervisor/reboot_supervisor.h"

#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs.h>
#include "common/config.h"
#include "common/event.h"

namespace controlme {

namespace {
const char* kTag = "supervisor";
// The NVS namespace is shared with other modules (wifi creds) — the
// reboot keys belong to the supervisor alone; names <=15 chars (NVS limit).
const char* kNvsNs = "partner";
const char* kNvsIntent = "reboot_intent";   // u8: 1 = intentional reboot
const char* kNvsReason = "reboot_reason";   // str
const char* kNvsReqId = "reboot_reqid";     // str — for UNKNOWN reconciliation

// Minimal JSON escape for the event payload ('"'->\" '\\'->\\
// byte<0x20 -> \u00XX). Never writes a truncated escape — truncation stays
// valid JSON; transport::jsonEscape isn't used so this lib doesn't depend on
// transport.
size_t escJson(const char* src, char* dst, size_t cap) {
  size_t n = 0;
  if (!dst || cap == 0) return 0;
  dst[0] = '\0';
  if (!src) return 0;
  for (const char* p = src; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    char ubuf[8];
    const char* rep = nullptr;
    if (c == '"') {
      rep = "\\\"";
    } else if (c == '\\') {
      rep = "\\\\";
    } else if (c < 0x20) {
      snprintf(ubuf, sizeof(ubuf), "\\u%04x", c);
      rep = ubuf;
    }
    const size_t rl = rep ? strlen(rep) : 1;
    if (n + rl + 1 > cap) break;
    if (rep) {
      memcpy(dst + n, rep, rl);
    } else {
      dst[n] = (char)c;
    }
    n += rl;
  }
  dst[n] = '\0';
  return n;
}
}  // namespace

RebootSupervisor::RebootSupervisor(App& app) : app_(app) {}

Status RebootSupervisor::begin() {
  queue_ = xQueueCreate(kQueueDepth, sizeof(RebootReq));
  if (!queue_) return Status::Fail(Err::Internal, "supervisor queue alloc");
  esp_timer_create_args_t tcfg = {};
  tcfg.callback = &RebootSupervisor::deadlineThunk;
  tcfg.name = "reboot-dl";
  if (esp_timer_create(&tcfg, &deadlineTimer_) != ESP_OK) {
    return Status::Fail(Err::Internal, "supervisor timer alloc");
  }
  if (xTaskCreatePinnedToCore(&taskEntry, "supervisor", cfg::kStackSupervisor,
                              this, cfg::kPrioSupervisor, &task_,
                              cfg::kCoreApp) != pdPASS) {
    return Status::Fail(Err::Internal, "supervisor task create");
  }
  return Status::Ok();
}

Status RebootSupervisor::requestReboot(const char* reason,
                                       const char* requestId) {
  // Step 1 docs/03: close admission IMMEDIATELY in the caller's task —
  // also on re-entry (idempotent) so no new action/upload/run slips
  // through.
  app_.admissionOpen = false;
  if (!queue_ || !deadlineTimer_) {
    // Module failed to start: the rebooting flag is NOT set so the caller
    // sees an honest Internal, not a fake DeviceRebooting forever.
    return Status::Fail(Err::Internal, "supervisor not started");
  }
  taskENTER_CRITICAL(&mux_);
  const bool already = rebooting_;
  rebooting_ = true;
  taskEXIT_CRITICAL(&mux_);
  if (already) {
    return Status::Fail(Err::DeviceRebooting, "reboot already in progress");
  }
  // Step 6 docs/03: the total deadline is armed HERE — even if the
  // task/queue or a graceful step hangs, esp_restart still happens (forced ->
  // UNKNOWN outcome). Once per boot; the rebooting_ gate prevents
  // re-arming.
  if (esp_timer_start_once(deadlineTimer_,
                           (uint64_t)cfg::kRebootDeadlineMs * 1000ULL) !=
      ESP_OK) {
    ESP_LOGW(kTag, "deadline timer arm failed — only the graceful sequence will run");
  }
  RebootReq req{};
  copyStr(req.reason, sizeof(req.reason),
          (reason && reason[0]) ? reason : "unspecified");
  copyStr(req.requestId, sizeof(req.requestId), requestId);
  if (xQueueSend(queue_, &req, 0) != pdTRUE) {
    // Timer stays armed: the device still reboots <=kRebootDeadlineMs —
    // this error is honest (the graceful sequence won't run) but the reboot
    // intent isn't lost.
    ESP_LOGW(kTag, "queue full — only the deadline timer will restart");
    return Status::Fail(Err::QueueFull, "supervisor queue full");
  }
  return Status::Ok();
}

void RebootSupervisor::taskEntry(void* arg) {
  static_cast<RebootSupervisor*>(arg)->loop();
}

void RebootSupervisor::loop() {
  for (;;) {
    RebootReq req{};
    if (xQueueReceive(queue_, &req, portMAX_DELAY) != pdTRUE) continue;
    runShutdown(req);  // esp_restart at the end — practically never returns
  }
}

void RebootSupervisor::runShutdown(const RebootReq& req) {
  ESP_LOGI(kTag, "reboot starting: reason='%s' requestId='%s'", req.reason,
           req.requestId);
  // Step 2: cancel the run queue + in-flight provider calls.
  if (app_.runs) {
    Status st = app_.runs->cancelAll();
    if (!st.ok()) ESP_LOGW(kTag, "runs cancelAll failed: %s", errName(st.code));
  }
  // Step 3: grace <=1 s so owners finish bounded chunks, then controlled
  // SD unmount (flush/close) — best-effort; the deadline timer is the fence
  // if unmount blocks past the remaining budget.
  vTaskDelay(pdMS_TO_TICKS(kOwnerGraceMs));
  if (app_.coord) {
    Status st = app_.coord->unmountSd();
    if (!st.ok()) ESP_LOGW(kTag, "unmountSd failed: %s", errName(st.code));
  }
  // Step 4: NVS marker — best-effort; failure -> next boot outcome UNKNOWN.
  writeMarker(req);
  // Step 5: REBOOTING event — notice <=kRebootNoticeMs so SSE gets a
  // chance to flush; not a delivery guarantee.
  publishRebooting(req);
  vTaskDelay(pdMS_TO_TICKS(cfg::kRebootNoticeMs));
  // This line must print BEFORE the flush so it makes the file — the
  // graceful reboot trail in prev.log ends with "esp_restart".
  ESP_LOGI(kTag, "graceful done — esp_restart");
  if (app_.logs) app_.logs->flush(cfg::kRebootNoticeMs);
  esp_restart();  // never returns
}

void RebootSupervisor::writeMarker(const RebootReq& req) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "nvs marker open failed: %d", (int)err);
    return;
  }
  nvs_set_u8(h, kNvsIntent, 1);
  nvs_set_str(h, kNvsReason, req.reason);
  if (req.requestId[0]) {
    nvs_set_str(h, kNvsReqId, req.requestId);
  } else {
    nvs_erase_key(h, kNvsReqId);  // old marker must not carry into this boot
  }
  err = nvs_commit(h);
  nvs_close(h);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "nvs marker commit failed: %d", (int)err);
  }
}

void RebootSupervisor::publishRebooting(const RebootReq& req) {
  if (!app_.events) return;
  Event ev{};
  ev.kind = EventKind::Custom;
  copyStr(ev.source, sizeof(ev.source), "firmware");
  ev.occurredMonoMs = app_.clock ? app_.clock->monoMs() : 0;
  char escR[96];  // enough for a 63-char reason surviving escJson in the payload
  escJson(req.reason, escR, sizeof(escR));
  // ev.payload kPayloadCap=160: prefix ~29 + escR <=95 -> always fits whole.
  snprintf(ev.payload, sizeof(ev.payload),
           "{\"rebooting\":true,\"reason\":\"%s\"}", escR);
  Status st = app_.events->publish(ev);
  if (!st.ok()) {
    ESP_LOGW(kTag, "REBOOTING event publish failed: %s", errName(st.code));
  }
}

void RebootSupervisor::deadlineThunk(void* /*arg*/) {
  // Runs in the esp_timer task — total docs/03 deadline: reboot even if
  // flush/graceful steps fail or hang.
  ESP_LOGW(kTag, "reboot deadline %lu ms reached — forced restart",
           (unsigned long)cfg::kRebootDeadlineMs);
  esp_restart();
}

bool RebootSupervisor::readRebootMarker(char* reasonOut, size_t reasonCap,
                                        char* requestIdOut,
                                        size_t requestIdCap) {
  if (reasonOut && reasonCap) reasonOut[0] = '\0';
  if (requestIdOut && requestIdCap) requestIdOut[0] = '\0';
  nvs_handle_t h;
  if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return false;
  uint8_t intent = 0;
  const bool has =
      nvs_get_u8(h, kNvsIntent, &intent) == ESP_OK && intent != 0;
  if (has) {
    if (reasonOut && reasonCap) {
      size_t l = reasonCap;
      nvs_get_str(h, kNvsReason, reasonOut, &l);
    }
    if (requestIdOut && requestIdCap) {
      size_t l = requestIdCap;
      nvs_get_str(h, kNvsReqId, requestIdOut, &l);
    }
    // Read-once: erase so the next boot doesn't claim it again.
    nvs_erase_key(h, kNvsIntent);
    nvs_erase_key(h, kNvsReason);
    nvs_erase_key(h, kNvsReqId);
    nvs_commit(h);
  }
  nvs_close(h);
  return has;
}

}  // namespace controlme
