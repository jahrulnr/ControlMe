// Supervisor module — wires RebootSupervisor (lib/supervisor) into App +
// the POST /api/v1/device/reboot {reason?, requestId?, expectedBootId?}
// endpoint.
//
// Semantics (docs/03 §Reboot + docs/07):
//   - foreign expectedBootId -> UNKNOWN_EXECUTION WITHOUT executing.
//   - Ok -> requestReboot() returns immediately (admission closes at once);
//     the graceful sequence runs in the supervisor task, not the httpd task.
//     A 202 {"state":"REBOOTING"} response = accepted, not already restarted.
//   - Re-entry while rebooting -> DEVICE_REBOOTING (503).
// The NVS marker is written by the supervisor task; the integrator reads it
// next boot via RebootSupervisor::readRebootMarker() — not here.
//
// IMPORTANT: call controlme_register_supervisor() BEFORE
// controlme_register_core() — core's static "GET /*" fallback is matched in
// order and would swallow routes registered afterwards.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <cJSON.h>
#include <esp_log.h>
#include "app.h"
#include "common/psram_alloc.h"
#include "common/event.h"   // copyStr
#include "supervisor/reboot_supervisor.h"

namespace {

const char* kTag = "mod-supervisor";
using controlme::Err;
using controlme::Status;
using transport::HttpRequest;

// Local JSON parser with parseJsonBody semantics (maxBytes bound;
// truncated body -> ProtocolIncomplete; size/parse errors ->
// InvalidArgument). transport::JsonBody isn't used — same instantiation
// defect noted in mod_storage.cpp.
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

// POST /api/v1/device/reboot — optional JSON body
// {reason?, requestId?, expectedBootId?}; no body = valid (default reason).
Status handleDeviceReboot(HttpRequest& req, void* ctx) {
  controlme::App& app = *static_cast<controlme::App*>(ctx);
  if (!app.rebootCtl) {
    return req.sendProblem(Err::Internal, "reboot supervisor not installed");
  }
  char reason[64] = {};
  char requestId[controlme::cfg::kRequestIdCap] = {};
  char expectedBootId[controlme::cfg::kBootIdCap] = {};
  if (req.contentLength() > 0) {
    JsonDoc jd;
    Status s = readJsonBody(req, jd);
    if (!s.ok()) {
      return req.sendProblem(s.code, s.detail ? s.detail : "invalid json body");
    }
    jsonStr(jd.root, "reason", reason, sizeof(reason));
    jsonStr(jd.root, "requestId", requestId, sizeof(requestId));
    jsonStr(jd.root, "expectedBootId", expectedBootId, sizeof(expectedBootId));
  }
  // foreign expectedBootId -> UNKNOWN_EXECUTION WITHOUT executing (docs/03).
  if (expectedBootId[0] != '\0' && strcmp(expectedBootId, app.bootId) != 0) {
    return req.sendProblem(Err::UnknownExecution,
                           "expectedBootId does not match the current boot",
                           requestId[0] ? requestId : nullptr);
  }
  // requestId goes into the NVS marker (next-boot outcome reconciliation).
  // The handler knows the concrete type — this module created the instance.
  controlme::RebootSupervisor& sup =
      *static_cast<controlme::RebootSupervisor*>(app.rebootCtl);
  Status s = sup.requestReboot(reason[0] ? reason : nullptr,
                               requestId[0] ? requestId : nullptr);
  if (!s.ok()) {
    return req.sendProblem(s.code, s.detail,
                           requestId[0] ? requestId : nullptr);
  }
  char esc[6 * sizeof(requestId) + 8];  // jsonEscape worst-case 6x + margin
  transport::jsonEscape(requestId, esc, sizeof(esc));
  char body[sizeof(esc) + 48];
  if (requestId[0]) {
    snprintf(body, sizeof(body),
             "{\"state\":\"REBOOTING\",\"requestId\":\"%s\"}", esc);
  } else {
    snprintf(body, sizeof(body), "{\"state\":\"REBOOTING\"}");
  }
  return req.sendJson(202, body);
}

}  // namespace

void controlme_register_supervisor(controlme::App& app) {
  static controlme::RebootSupervisor sup(app);
  Status s = sup.begin();
  if (!s.ok()) {
    ESP_LOGE(kTag, "reboot supervisor start failed: %s",
             s.detail ? s.detail : controlme::errName(s.code));
  }
  // Slot filled even when start fails — requestReboot() answers an
  // honest Internal ("supervisor not started"), clearer than a null pointer
  // (mod_servo).
  app.rebootCtl = &sup;
  Status rs = app.http.addRoute(HTTP_POST, "/api/v1/device/reboot",
                                &handleDeviceReboot, &app);
  if (!rs.ok()) {
    ESP_LOGE(kTag, "route device/reboot failed: %s", controlme::errName(rs.code));
  }
  app.addFeature("{\"supervisor\":true}");
}
