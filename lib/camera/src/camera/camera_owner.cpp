// CameraOwner implementation — every handler body runs IN the owner task
// (confinement). Caller tasks only interact through the call() facade.
#include "camera/camera_owner.h"

#include <stdio.h>
#include <string.h>
#include "common/config.h"
#include "common/event.h"   // copyStr
#include "common/psram_alloc.h"
#include <esp_camera.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_psram.h>
#include <esp_timer.h>

namespace controlme {

static const char* kTag = "camera";

// Facade timeout: every op is bounded work for the owner (Read copies one
// kIoChunk — network send pacing lives in the caller's sink.write).
static constexpr uint32_t kCtlTimeoutMs = cfg::kToolTimeoutMs;    // 10 s

// CameraReq ~4.2 KiB — two on a caller stack would overflow the facade
// tasks (httpd 8 KiB, mcpw 6 KiB). All facades heap-allocate a pair
// (PSRAM-first), same pattern as StorageOwner's StorageReqPair.
struct CameraReqPair { CameraReq in, o; };
static CameraReqPair* allocCameraReq() {
  return controlme::psramNew<CameraReqPair>();
}

// ------------------------------------------------------------------ facade

Status CameraOwner::begin() {
  return start(OwnerTaskConfig<CameraReq>{
      .name = "camera",
      .stackBytes = cfg::kStackCamera,        // bytes — ESP-IDF convention
      .priority = cfg::kPrioCamera,
      .queueDepth = cfg::kCameraQueueDepth,
      .handle = &CameraOwner::dispatchThunk,
      .ctx = this,
      .core = cfg::kCoreApp});
}

Status CameraOwner::initHw() {
  CameraReqPair* rp = allocCameraReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  rp->in.op = CameraOp::Init;
  Status st = call(rp->in, rp->o, kCtlTimeoutMs);
  controlme::psramDelete(rp);
  return st;
}

Status CameraOwner::tick() {
  CameraReqPair* rp = allocCameraReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  rp->in.op = CameraOp::Tick;
  Status st = call(rp->in, rp->o, kCtlTimeoutMs);
  controlme::psramDelete(rp);
  return st;
}

Status CameraOwner::capture(ImageMeta& out) {
  CameraReqPair* rp = allocCameraReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  rp->in.op = CameraOp::Capture;
  Status st = call(rp->in, rp->o, kCtlTimeoutMs);
  if (st.ok()) out = rp->o.meta;
  controlme::psramDelete(rp);
  return st;
}

Status CameraOwner::pin(const char* imageId) {
  CameraReqPair* rp = allocCameraReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  rp->in.op = CameraOp::Pin;
  copyStr(rp->in.imageId, sizeof(rp->in.imageId), imageId);
  Status st = call(rp->in, rp->o, kCtlTimeoutMs);
  controlme::psramDelete(rp);
  return st;
}

Status CameraOwner::release(const char* imageId) {
  CameraReqPair* rp = allocCameraReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  rp->in.op = CameraOp::Release;
  copyStr(rp->in.imageId, sizeof(rp->in.imageId), imageId);
  Status st = call(rp->in, rp->o, kCtlTimeoutMs);
  controlme::psramDelete(rp);
  return st;
}

Status CameraOwner::discard(const char* imageId) {
  CameraReqPair* rp = allocCameraReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  rp->in.op = CameraOp::Discard;
  copyStr(rp->in.imageId, sizeof(rp->in.imageId), imageId);
  Status st = call(rp->in, rp->o, kCtlTimeoutMs);
  controlme::psramDelete(rp);
  return st;
}

// Per-chunk pump — same pattern as StorageOwner::read: each iteration is
// one bounded Read request (owner memcpy only). sink.write runs in THIS
// (caller's) task, so a slow client can never occupy the camera queue and
// nothing caller-owned crosses the task boundary — the caller may time
// out or die between chunks with no lifetime hazard.
Status CameraOwner::readImage(const char* imageId, IByteSink& sink,
                              ImageMeta& metaOut) {
  CameraReqPair* rp = allocCameraReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  Status st = Status::Ok();
  uint32_t off = 0;
  bool first = true;
  for (;;) {
    CameraReq& in = rp->in;
    in = CameraReq{};
    in.op = CameraOp::Read;
    copyStr(in.imageId, sizeof(in.imageId), imageId);
    in.offset = off;
    st = call(in, rp->o, kCtlTimeoutMs);
    if (!st.ok()) break;
    if (first) { metaOut = rp->o.meta; first = false; }
    if (rp->o.dataLen == 0) break;  // EOF
    st = sink.write(rp->o.data, rp->o.dataLen);
    if (!st.ok()) break;
    off += rp->o.dataLen;
    if (off >= rp->o.meta.bytes) break;  // image fully sent
  }
  controlme::psramDelete(rp);
  return st;
}

Status CameraOwner::beginStream() {
  CameraReqPair* rp = allocCameraReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  rp->in.op = CameraOp::StreamBegin;
  Status st = call(rp->in, rp->o, kCtlTimeoutMs);
  controlme::psramDelete(rp);
  return st;
}

Status CameraOwner::pullStreamFrame(const uint8_t*& data, size_t& len) {
  CameraReqPair* rp = allocCameraReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  rp->in.op = CameraOp::StreamPull;
  Status st = call(rp->in, rp->o, kCtlTimeoutMs);
  if (st.ok()) {
    data = rp->o.frame;
    len = rp->o.frameLen;
  }
  controlme::psramDelete(rp);
  return st;
}

Status CameraOwner::releaseStreamFrame() {
  CameraReqPair* rp = allocCameraReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  rp->in.op = CameraOp::StreamRelease;
  Status st = call(rp->in, rp->o, kCtlTimeoutMs);
  controlme::psramDelete(rp);
  return st;
}

Status CameraOwner::endStream() {
  CameraReqPair* rp = allocCameraReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  rp->in.op = CameraOp::StreamEnd;
  Status st = call(rp->in, rp->o, kCtlTimeoutMs);
  controlme::psramDelete(rp);
  return st;
}

Status CameraOwner::stat(const char* imageId, ImageMeta& metaOut) {
  CameraReqPair* rp = allocCameraReq();
  if (!rp) return Status::Fail(Err::Internal, "alloc req");
  rp->in.op = CameraOp::Stat;
  copyStr(rp->in.imageId, sizeof(rp->in.imageId), imageId);
  Status st = call(rp->in, rp->o, kCtlTimeoutMs);
  if (st.ok()) metaOut = rp->o.meta;
  controlme::psramDelete(rp);
  return st;
}

// ------------------------------------------------------------- task owner

void CameraOwner::dispatchThunk(CameraReq& r, void* ctx) {
  static_cast<CameraOwner*>(ctx)->handle(r);
}

uint32_t CameraOwner::nowMs() {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

void CameraOwner::handle(CameraReq& r) {
  // LAZY TTL sweep on every op (docs/03): expired = unreferenced & not
  // pinned (in-progress reads refresh lastRefMs per chunk). Tick = sweep.
  sweepExpired(nowMs());
  switch (r.op) {
    case CameraOp::Init:    handleInit(r);    break;
    case CameraOp::Capture: handleCapture(r); break;
    case CameraOp::Pin:     handlePin(r);     break;
    case CameraOp::Release: handleRelease(r); break;
    case CameraOp::Discard: handleDiscard(r); break;
    case CameraOp::Read:    handleRead(r);    break;
    case CameraOp::Stat:    handleStat(r);    break;
    case CameraOp::Tick:    r.ctrl.status = Status::Ok(); break;
    case CameraOp::StreamBegin:   handleStreamBegin(r);   break;
    case CameraOp::StreamPull:    handleStreamPull(r);    break;
    case CameraOp::StreamRelease: handleStreamRelease(r); break;
    case CameraOp::StreamEnd:     handleStreamEnd(r);     break;
  }
}

void CameraOwner::handleInit(CameraReq& r) {
  // Detect real hardware FIRST (bench docs requirement: "read flash info
  // & PSRAM init") — recorded as-is, used by the feature fragment.
  psramBytes_ = (uint32_t)esp_psram_get_size();
  uint32_t flash = 0;
  if (esp_flash_get_size(nullptr, &flash) == ESP_OK) {
    flashBytes_ = flash;  // size from the image header; not a physical guarantee
  }
  ESP_LOGI(kTag, "detected: psram=%lu bytes, flash=%lu bytes",
           (unsigned long)psramBytes_, (unsigned long)flashBytes_);

  // Exact AI-Thinker pin map (docs/05). XCLK uses LEDC timer0 channel0
  // @20 MHz — the speed mode is chosen internally by the esp32-camera driver
  // (the config API doesn't expose the field).
  camera_config_t cfg = {};
  cfg.pin_pwdn = 32;
  cfg.pin_reset = -1;          // no dedicated reset GPIO
  cfg.pin_xclk = 0;
  cfg.pin_sccb_sda = 26;
  cfg.pin_sccb_scl = 27;
  cfg.pin_d7 = 35;             // Y9
  cfg.pin_d6 = 34;             // Y8
  cfg.pin_d5 = 39;             // Y7
  cfg.pin_d4 = 36;             // Y6
  cfg.pin_d3 = 21;             // Y5
  cfg.pin_d2 = 19;             // Y4
  cfg.pin_d1 = 18;             // Y3
  cfg.pin_d0 = 5;              // Y2
  cfg.pin_vsync = 25;
  cfg.pin_href = 23;
  cfg.pin_pclk = 22;
  cfg.xclk_freq_hz = 20000000;
  cfg.ledc_timer = LEDC_TIMER_0;
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.frame_size = FRAMESIZE_QVGA;   // 320x240 — docs/02 budget
  cfg.jpeg_quality = 12;
  cfg.fb_count = 2;   // one held by the sender, one filled by the sensor
  cfg.grab_mode = CAMERA_GRAB_LATEST;
  cfg.fb_location = psramBytes_ > 0 ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    ready_ = false;
    ESP_LOGE(kTag, "esp_camera_init failed: 0x%x — endpoints honestly fail",
             (unsigned)err);
    r.ctrl.status = Status::Fail(Err::Internal, "camera init failed");
    return;
  }

  // Staging pool: PSRAM first, DRAM fallback with an honest per-slot log.
  stagingAllocated_ = 0;
  for (uint8_t i = 0; i < cfg::kStagingPoolSize; ++i) {
    StagingSlot& s = slots_[i];
    s.buf = (uint8_t*)heap_caps_malloc(cfg::kStagingMaxBytes, MALLOC_CAP_SPIRAM);
    if (s.buf) {
      ESP_LOGI(kTag, "staging slot %u: %u bytes PSRAM", (unsigned)i,
               (unsigned)cfg::kStagingMaxBytes);
    } else {
      s.buf = (uint8_t*)heap_caps_malloc(cfg::kStagingMaxBytes,
                                         MALLOC_CAP_INTERNAL |
                                             MALLOC_CAP_8BIT);
      if (s.buf) {
        ESP_LOGW(kTag, "staging slot %u: DRAM fallback %u bytes (PSRAM insufficient/absent)",
                 (unsigned)i, (unsigned)cfg::kStagingMaxBytes);
      } else {
        ESP_LOGE(kTag, "staging slot %u: %u byte alloc FAILED — slot unused",
                 (unsigned)i, (unsigned)cfg::kStagingMaxBytes);
      }
    }
    if (s.buf) ++stagingAllocated_;
  }
  ready_ = true;
  ESP_LOGI(kTag, "camera ready: QVGA jpeg, staging=%u slot", stagingAllocated_);
  r.ctrl.status = Status::Ok();
}

void CameraOwner::handleCapture(CameraReq& r) {
  if (!ready_) {
    r.ctrl.status = Status::Fail(Err::Internal, "camera not ready");
    return;
  }
  if (streaming_) {
    r.ctrl.status = Status::Fail(Err::ResourceBusy, "stream active");
    return;
  }
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    r.ctrl.status = Status::Fail(Err::Internal, "capture failed");
    return;
  }
  if (fb->len > cfg::kStagingMaxBytes) {
    esp_camera_fb_return(fb);
    r.ctrl.status = Status::Fail(Err::ImageTooLarge, "frame > 96KiB");
    return;
  }
  StagingSlot* s = findFree();
  if (!s) {
    // Full pool -> explicit RESOURCE_BUSY; NO silent/pinned eviction.
    esp_camera_fb_return(fb);
    r.ctrl.status = Status::Fail(Err::ResourceBusy, "staging pool full");
    return;
  }
  const size_t len = fb->len;
  const uint16_t w = (uint16_t)fb->width, h = (uint16_t)fb->height;
  memcpy(s->buf, fb->buf, len);
  // fb returned to the driver IMMEDIATELY — before filling meta/waiting for a consumer.
  esp_camera_fb_return(fb);
  snprintf(s->meta.imageId, sizeof(s->meta.imageId), "img-%lu",
           (unsigned long)(++imageSeq_));
  copyStr(s->meta.mime, sizeof(s->meta.mime), "image/jpeg");
  s->meta.bytes = (uint32_t)len;
  s->meta.width = w;
  s->meta.height = h;
  s->meta.pinned = false;
  s->refCount = 0;
  s->occupied = true;
  s->lastRefMs = nowMs();  // newly created without refs -> TTL runs from now
  r.meta = s->meta;
  r.ctrl.status = Status::Ok();
}

void CameraOwner::handlePin(CameraReq& r) {
  StagingSlot* s = findById(r.imageId);
  if (!s) {
    r.ctrl.status = Status::Fail(Err::NotFound, "image not found");
    return;
  }
  ++s->refCount;
  s->meta.pinned = true;
  r.meta = s->meta;
  r.ctrl.status = Status::Ok();
}

void CameraOwner::handleRelease(CameraReq& r) {
  StagingSlot* s = findById(r.imageId);
  if (!s) {
    r.ctrl.status = Status::Fail(Err::NotFound, "image not found");
    return;
  }
  if (s->refCount > 0) --s->refCount;
  if (s->refCount == 0) {
    s->meta.pinned = false;
    s->lastRefMs = nowMs();  // TTL starts when the last ref is released
  }
  r.meta = s->meta;
  r.ctrl.status = Status::Ok();
}

void CameraOwner::handleDiscard(CameraReq& r) {
  StagingSlot* s = findById(r.imageId);
  if (!s) {
    r.ctrl.status = Status::Fail(Err::NotFound, "image not found");
    return;
  }
  // Freed immediately — not a silent eviction: the caller explicitly
  // states the imageId won't be used again (one-shot snapshot; docs/02
  // explicit lifetime until the consumer finishes reading).
  s->occupied = false;
  s->refCount = 0;
  s->meta = ImageMeta{};
  s->lastRefMs = 0;
  r.ctrl.status = Status::Ok();
}

void CameraOwner::handleRead(CameraReq& r) {
  StagingSlot* s = findById(r.imageId);
  if (!s) {
    r.ctrl.status = Status::Fail(Err::NotFound, "image not found");
    return;
  }
  // Content is JPEG only — other mimes unknown -> NOT_SUPPORTED (docs/03).
  if (strcmp(s->meta.mime, "image/jpeg") != 0) {
    r.ctrl.status = Status::Fail(Err::NotSupported, "mime not supported");
    return;
  }
  // One bounded chunk per request — offset >= bytes is a plain EOF
  // (dataLen 0), not an error: the facade pumps until then and a mid-read
  // TTL/discard surfaces as an honest NotFound on the next chunk.
  r.dataLen = 0;
  if (r.offset < s->meta.bytes) {
    uint32_t n = s->meta.bytes - r.offset;
    if (n > cfg::kIoChunk) n = cfg::kIoChunk;
    memcpy(r.data, s->buf + r.offset, n);
    r.dataLen = n;
    s->lastRefMs = nowMs();  // a live reader keeps the TTL honest
  }
  r.meta = s->meta;
  r.ctrl.status = Status::Ok();
}

void CameraOwner::dropHeldFrame() {
  if (!heldFb_) return;
  esp_camera_fb_return(static_cast<camera_fb_t*>(heldFb_));
  heldFb_ = nullptr;
}

void CameraOwner::handleStreamBegin(CameraReq& r) {
  if (!ready_) {
    r.ctrl.status = Status::Fail(Err::Internal, "camera not ready");
    return;
  }
  if (streaming_) {
    r.ctrl.status = Status::Fail(Err::ResourceBusy, "stream active");
    return;
  }
  streaming_ = true;
  r.ctrl.status = Status::Ok();
}

void CameraOwner::handleStreamPull(CameraReq& r) {
  if (!streaming_) {
    r.ctrl.status = Status::Fail(Err::InvalidArgument, "stream not started");
    return;
  }
  if (heldFb_) {
    r.ctrl.status = Status::Fail(Err::ResourceBusy, "stream frame not released");
    return;
  }
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    r.ctrl.status = Status::Fail(Err::Internal, "capture failed");
    return;
  }
  // The JPEG must stay in PSRAM. DRAM buffers aren't sent: internal RAM is
  // too thin to hold a frame plus a TCP window.
  if (!esp_ptr_external_ram(fb->buf)) {
    esp_camera_fb_return(fb);
    r.ctrl.status = Status::Fail(Err::Internal, "frame not in PSRAM");
    return;
  }
  if (r.ctrl.state == kOwnerAbandoned) {
    esp_camera_fb_return(fb);
    r.ctrl.status = Status::Fail(Err::Timeout, "stream pull abandoned");
    return;
  }
  heldFb_ = fb;
  r.frame = fb->buf;
  r.frameLen = (uint32_t)fb->len;
  r.ctrl.status = Status::Ok();
}

void CameraOwner::handleStreamRelease(CameraReq& r) {
  dropHeldFrame();
  r.ctrl.status = Status::Ok();
}

void CameraOwner::handleStreamEnd(CameraReq& r) {
  dropHeldFrame();
  streaming_ = false;
  r.ctrl.status = Status::Ok();
}

void CameraOwner::handleStat(CameraReq& r) {
  StagingSlot* s = findById(r.imageId);
  if (!s) {
    r.ctrl.status = Status::Fail(Err::NotFound, "image not found");
    return;
  }
  r.meta = s->meta;
  r.ctrl.status = Status::Ok();
}

// ------------------------------------------------------------------ helpers

CameraOwner::StagingSlot* CameraOwner::findById(const char* imageId) {
  if (!imageId || !*imageId) return nullptr;
  for (auto& s : slots_) {
    if (s.occupied && strcmp(s.meta.imageId, imageId) == 0) return &s;
  }
  return nullptr;
}

CameraOwner::StagingSlot* CameraOwner::findFree() {
  for (auto& s : slots_) {
    if (s.buf && !s.occupied && s.refCount == 0) {
      return &s;
    }
  }
  return nullptr;
}

void CameraOwner::sweepExpired(uint32_t now) {
  for (auto& s : slots_) {
    if (s.occupied && s.refCount == 0 &&
        (int32_t)(now - s.lastRefMs) > (int32_t)cfg::kImageTtlMs) {
      ESP_LOGI(kTag, "staging %s expired (TTL %lu s) — slot freed",
               s.meta.imageId, (unsigned long)(cfg::kImageTtlMs / 1000));
      s.occupied = false;
      s.meta = ImageMeta{};
      s.lastRefMs = 0;
    }
  }
}

}  // namespace controlme
