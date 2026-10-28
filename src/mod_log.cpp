// Log module — DeviceLog :80 download route (GET
// /api/v1/logs/{current|previous}) for agents that can't read a ~cap file
// via the log_read tool alone. All file I/O goes through the IDeviceLog
// facade (the FileLog owner task) — handlers never touch LittleFS directly.
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include "app.h"
#include "common/psram_alloc.h"

namespace {

const char* kTag = "logmod";

controlme::Status handleLogDownload(transport::HttpRequest& req, void* ctx) {
  controlme::App& app = *static_cast<controlme::App*>(ctx);
  if (!app.logs) {
    return req.sendProblem(controlme::Err::NotSupported, "log not installed");
  }
  char which[16];
  if (!req.tailCopy("/api/v1/logs/", which, sizeof(which)) || !*which) {
    return req.sendProblem(controlme::Err::NotFound, "log not found");
  }
  controlme::LogFile wf;
  if (strcmp(which, "current") == 0) wf = controlme::LogFile::Current;
  else if (strcmp(which, "previous") == 0) wf = controlme::LogFile::Previous;
  else return req.sendProblem(controlme::Err::NotFound, "log not found");

  // PSRAM chunk — the IDeviceLog Timeout rule: dst is deliberately
  // leaked since the owner may still be writing to it.
  auto buf = controlme::psramArr<char>(controlme::cfg::kIoChunk);
  if (!buf) {
    return req.sendProblem(controlme::Err::Internal, "log buffer alloc");
  }
  transport::ChunkedSink sink(req, 200, "text/plain; charset=utf-8");
  size_t off = 0;
  uint32_t gen = 0;
  bool first = true;
  for (;;) {
    controlme::LogReadResult res{};
    controlme::Status st =
        app.logs->read(wf, off, buf.get(), controlme::cfg::kIoChunk,
                       controlme::cfg::kToolTimeoutMs, res);
    if (!st.ok()) {
      if (st.code == controlme::Err::Timeout) {
        ESP_LOGW(kTag, "log download timeout — %u B chunk left leaked",
                 (unsigned)controlme::cfg::kIoChunk);
        buf.release();
      }
      if (first) return req.sendProblem(st.code, st.detail);
      return st;  // stream already started — the bridge closes the connection
    }
    // File keep-tail truncated mid-download -> stop (the old offset is
    // meaningless against new content). gen is captured from the FIRST read
    // — a gen==0 sentinel is wrong because generation does start at 0.
    if (first) gen = res.generation;
    first = false;
    if (res.generation != gen) break;
    if (res.outLen == 0) break;
    st = sink.write((const uint8_t*)buf.get(), res.outLen);
    if (!st.ok()) return st;
    off += res.outLen;
    if (res.eof) break;
  }
  return sink.finish();
}

}  // namespace

void controlme_register_log(controlme::App& app) {
  controlme::Status st = app.http.addRoute(
      HTTP_GET, "/api/v1/logs/*", &handleLogDownload, &app);
  if (!st.ok()) {
    ESP_LOGE(kTag, "route /api/v1/logs failed: %s", controlme::errName(st.code));
  }
  if (!app.addFeature(
          "{\"logs\":{\"files\":[\"current\",\"previous\"],"
          "\"download\":\"/api/v1/logs/{which}\"}}")) {
    ESP_LOGW(kTag, "logs feature fragment add failed");
  }
}
