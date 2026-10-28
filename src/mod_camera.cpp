// Camera module — registers CameraOwner + HTTP snapshot/image routes
// (docs/07). MUST be called from setup() BEFORE controlme_register_core:
// the static GET /* fallback must stay last so it doesn't swallow
// /api/v1/images/*.
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include "app.h"
#include "camera/camera_owner.h"
#include "common/config.h"

namespace {

const char* kTag = "camera";

// GET /api/v1/camera/snapshot — captures using the same staging-pool
// budget (no free third object). Used ONCE: pin -> stream -> DISCARD, no
// dangling ref — the imageId is never exposed to the client, so the slot
// must go free immediately. The old release() held the slot 60 s
// (unreferenced-image TTL) so a ~1 fps streamer drained the pool ->
// RESOURCE_BUSY.
controlme::Status handleSnapshot(transport::HttpRequest& req, void* ctx) {
  controlme::App& app = *static_cast<controlme::App*>(ctx);
  controlme::ICamera* cam = app.camera;
  if (!cam || !cam->isReady()) {
    return req.sendProblem(controlme::Err::Internal, "camera not ready");
  }
  controlme::ImageMeta meta;
  controlme::Status st = cam->capture(meta);
  if (!st.ok()) return req.sendProblem(st.code, st.detail);
  st = cam->pin(meta.imageId);
  if (!st.ok()) {
    cam->discard(meta.imageId);   // pin failed -> one-shot image unused
    return req.sendProblem(st.code, st.detail);
  }
  transport::ChunkedSink sink(req, 200, meta.mime);
  st = cam->readImage(meta.imageId, sink, meta);
  cam->discard(meta.imageId);   // slot free now — not waiting for the TTL
  if (!st.ok()) return st;  // response already started — the bridge closes the connection
  return sink.finish();
}

// GET /api/v1/images/* — read a staged image by id; pin holds the TTL
// during the stream, release returns it to unreferenced (60 s TTL runs).
controlme::Status handleImage(transport::HttpRequest& req, void* ctx) {
  controlme::App& app = *static_cast<controlme::App*>(ctx);
  controlme::ICamera* cam = app.camera;
  if (!cam || !cam->isReady()) {
    return req.sendProblem(controlme::Err::Internal, "camera not ready");
  }
  char id[controlme::cfg::kIdCap];
  if (!req.tailCopy("/api/v1/images/", id, sizeof(id)) || !*id) {
    return req.sendProblem(controlme::Err::NotFound, "image not found");
  }
  controlme::Status st = cam->pin(id);
  if (!st.ok()) return req.sendProblem(st.code, st.detail);  // 404 NOT_FOUND
  controlme::ImageMeta meta;
  st = cam->stat(id, meta);
  if (!st.ok()) {
    cam->release(id);
    return req.sendProblem(st.code, st.detail);
  }
  transport::ChunkedSink sink(req, 200, meta.mime);
  st = cam->readImage(id, sink, meta);
  cam->release(id);
  if (!st.ok()) return st;
  return sink.finish();
}

void addOrLog(transport::HttpServer& http, const char* uri,
              transport::HttpHandler h, controlme::App& app) {
  controlme::Status st = http.addRoute(HTTP_GET, uri, h, &app);
  if (!st.ok()) {
    ESP_LOGE(kTag, "route %s failed: %s", uri, controlme::errName(st.code));
  }
}

}  // namespace

void controlme_register_camera(controlme::App& app) {
  static controlme::CameraOwner s_owner;
  static bool s_registered = false;
  if (s_registered) return;  // a second registration would clobber the owner task/queue
  s_registered = true;

  controlme::Status st = s_owner.begin();
  if (!st.ok()) {
    ESP_LOGE(kTag, "owner start failed: %s", st.detail ? st.detail : "?");
  } else {
    st = s_owner.initHw();
    if (!st.ok()) {
      // Routes stay registered — handlers reply with an honest problem (not ready).
      ESP_LOGE(kTag, "camera init failed: %s", st.detail ? st.detail : "?");
    }
  }
  app.camera = &s_owner;  // isReady()==false stays honest to clients

  addOrLog(app.http, "/api/v1/camera/snapshot", &handleSnapshot, app);
  addOrLog(app.http, "/api/v1/images/*", &handleImage, app);

  // Capabilities fragment — static storage (filled once at register).
  static char s_feature[160];
  snprintf(s_feature, sizeof(s_feature),
           "{\"camera\":{\"ready\":%s,\"frameSize\":\"QVGA\","
           "\"stagingSlots\":%u,\"psramBytes\":%lu}}",
           s_owner.isReady() ? "true" : "false",
           (unsigned)s_owner.stagingSlots(),
           (unsigned long)s_owner.psramBytes());
  if (!app.addFeature(s_feature)) {
    ESP_LOGW(kTag, "camera feature fragment add failed");
  }
}
