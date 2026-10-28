// Core :80 routes — GET /api/v1/status, GET /api/v1/capabilities.
#include <stdio.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include "app.h"

namespace {

controlme::Status handleStatus(transport::HttpRequest& req, void* ctx) {
  controlme::App& app = *static_cast<controlme::App*>(ctx);
  char buf[384];
  snprintf(buf, sizeof(buf),
           "{\"bootId\":\"%s\",\"lastEventSequence\":%lu,\"uptimeMs\":%llu,"
           "\"sdMounted\":%s,\"heapFree\":%u,\"heapMinFree\":%u,"
           "\"heapLargestBlock\":%u}",
           app.bootId, (unsigned long)app.lastEventSequence,
           (unsigned long long)(app.clock ? app.clock->monoMs() : 0),
           (app.storage && app.storage->isMounted()) ? "true" : "false",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_minimum_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                      MALLOC_CAP_8BIT));
  return req.sendJson(200, buf);
}

controlme::Status handleCapabilities(transport::HttpRequest& req, void* ctx) {
  controlme::App& app = *static_cast<controlme::App*>(ctx);
  // Small stream via ChunkedSink — features[] only holds fragments from
  // truly-registered modules (empty = honest, no features yet).
  transport::ChunkedSink sink(req, 200, "application/json");
  char head[96];
  int n = snprintf(head, sizeof(head), "{\"api\":\"v1\",\"bootId\":\"%s\"",
                   app.bootId);
  sink.write((const uint8_t*)head, (size_t)n);
  sink.write((const uint8_t*)",\"features\":[",
             sizeof(",\"features\":[") - 1);
  for (uint8_t i = 0; i < app.featureCount; ++i) {
    if (i) sink.write((const uint8_t*)",", 1);
    sink.write((const uint8_t*)app.features[i], strlen(app.features[i]));
  }
  sink.write((const uint8_t*)"]}", 2);
  return sink.finish();
}

void addOrLog(transport::HttpServer& http, const char* uri,
              transport::HttpHandler h, controlme::App& app) {
  controlme::Status st = http.addRoute(HTTP_GET, uri, h, &app);
  if (!st.ok()) {
    ESP_LOGE("core", "route %s failed: %s", uri, controlme::errName(st.code));
  }
}

}  // namespace

void controlme_register_core(controlme::App& app) {
  addOrLog(app.http, "/api/v1/status", &handleStatus, app);
  addOrLog(app.http, "/api/v1/capabilities", &handleCapabilities, app);
}
