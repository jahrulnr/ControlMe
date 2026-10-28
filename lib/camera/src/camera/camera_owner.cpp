// CameraOwner implementation — every handler body runs IN the owner task
// (confinement). Caller tasks only interact through the call() facade.
#include "camera/camera_owner.h"

#include <stdio.h>
#include <string.h>
#include "common/config.h"
#include "common/event.h"   // copyStr
#include <esp_camera.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_psram.h>
#include <esp_timer.h>

namespace controlme {

static const char* kTag = "camera";

// Facade timeouts: control ops use the common tool budget; Read is longer
// because it involves a network stream to the client (4 KiB per send).
static constexpr uint32_t kCtlTimeoutMs = cfg::kToolTimeoutMs;    // 10 s
static constexpr uint32_t kReadTimeoutMs = cfg::kIdleTimeoutMs;   // 30 s
// Maximum wait for "owner stopped writing to the sink" after a facade
// timeout — the httpd socket send_wait_timeout is 5 s per send.
static constexpr uint32_t kReadAbandonWaitMs = 15 * 1000;

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
  CameraReq in{}, out{};
  in.op = CameraOp::Init;
  return call(in, out, kCtlTimeoutMs);
}

Status CameraOwner::tick() {
  CameraReq in{}, out{};
  in.op = CameraOp::Tick;
  return call(in, out, kCtlTimeoutMs);
}

Status CameraOwner::capture(ImageMeta& out) {
  CameraReq in{}, res{};
  in.op = CameraOp::Capture;
  Status st = call(in, res, kCtlTimeoutMs);
  if (st.ok()) out = res.meta;
  return st;
}

Status CameraOwner::pin(const char* imageId) {
  CameraReq in{}, out{};
  in.op = CameraOp::Pin;
  copyStr(in.imageId, sizeof(in.imageId), imageId);
  return call(in, out, kCtlTimeoutMs);
}

Status CameraOwner::release(const char* imageId) {
  CameraReq in{}, out{};
  in.op = CameraOp::Release;
  copyStr(in.imageId, sizeof(in.imageId), imageId);
  return call(in, out, kCtlTimeoutMs);
}

Status CameraOwner::discard(const char* imageId) {
  CameraReq in{}, out{};
  in.op = CameraOp::Discard;
  copyStr(in.imageId, sizeof(in.imageId), imageId);
  return call(in, out, kCtlTimeoutMs);
}

Status CameraOwner::readImage(const char* imageId, IByteSink& sink,
                              ImageMeta& metaOut) {
  CameraReq in{}, res{};
  in.op = CameraOp::Read;
  copyStr(in.imageId, sizeof(in.imageId), imageId);
  in.sink = &sink;
  // The sink lives on this task's stack — if call() times out, the owner
  // may still be writing to it. readDone ensures the owner has fully stopped
  // before we return (the owner exits at a chunk boundary via the abandoned
  // state check).
  SemaphoreHandle_t readDone = xSemaphoreCreateBinary();
  if (!readDone) return Status::Fail(Err::Internal, "read sem alloc");
  in.readDone = readDone;
  Status st = call(in, res, kReadTimeoutMs);
  if (st.code == Err::Timeout) {
    // Wait bound = httpd socket send timeout (5 s) + chunk margin.
    xSemaphoreTake(readDone, pdMS_TO_TICKS(kReadAbandonWaitMs));
  }
  vSemaphoreDelete(readDone);
  if (st.ok()) metaOut = res.meta;
  return st;
}

Status CameraOwner::beginStream() {
  CameraReq in{}, out{};
  in.op = CameraOp::StreamBegin;
  return call(in, out, kCtlTimeoutMs);
}

Status CameraOwner::pullStreamFrame(const uint8_t*& data, size_t& len) {
  CameraReq in{}, res{};
  in.op = CameraOp::StreamPull;
  Status st = call(in, res, kCtlTimeoutMs);
  if (!st.ok()) return st;
  data = res.frame;
  len = res.frameLen;
  return st;
}

Status CameraOwner::releaseStreamFrame() {
  CameraReq in{}, out{};
  in.op = CameraOp::StreamRelease;
  return call(in, out, kCtlTimeoutMs);
}

Status CameraOwner::endStream() {
  CameraReq in{}, out{};
  in.op = CameraOp::StreamEnd;
  return call(in, out, kCtlTimeoutMs);
}

Status CameraOwner::stat(const char* imageId, ImageMeta& metaOut) {
  CameraReq in{}, res{};
  in.op = CameraOp::Stat;
  copyStr(in.imageId, sizeof(in.imageId), imageId);
  Status st = call(in, res, kCtlTimeoutMs);
  if (st.ok()) metaOut = res.meta;
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
  // LAZY TTL sweep on every op (docs/03): expired = unreferenced, not
  // pinned, not being read by a provider. Tick = sweep only.
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
  s->providerReading = false;
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
  // explicit lifetime until the provider finishes reading). Owner ops are
  // serialized on one task -> providerReading/refCount can't change mid-op.
  s->occupied = false;
  s->refCount = 0;
  s->providerReading = false;
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
  if (!r.sink) {
    r.ctrl.status = Status::Fail(Err::InvalidArgument, "sink null");
    return;
  }
  s->providerReading = true;
  Status st = Status::Ok();
  size_t off = 0;
  while (off < s->meta.bytes) {
    // A caller that ran out of deadline marks the request abandoned —
    // stop the stream ASAP; the sink is a stack object owned by the caller
    // task.
    if (r.ctrl.state == kOwnerAbandoned) {
      st = Status::Fail(Err::Internal, "read abandoned");
      break;
    }
    size_t n = s->meta.bytes - off;
    if (n > cfg::kIoChunk) n = cfg::kIoChunk;
    st = r.sink->write(s->buf + off, n);
    if (!st.ok()) break;
    off += n;
  }
  s->providerReading = false;
  r.meta = s->meta;
  r.ctrl.status = st;
  // Tell the facade the sink won't be touched again (see readImage).
  if (r.readDone) xSemaphoreGive(r.readDone);
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
    if (s.buf && !s.occupied && s.refCount == 0 && !s.providerReading) {
      return &s;
    }
  }
  return nullptr;
}

void CameraOwner::sweepExpired(uint32_t now) {
  for (auto& s : slots_) {
    if (s.occupied && s.refCount == 0 && !s.providerReading &&
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
