#pragma once
// Camera owner (adapter) — the ONLY task that touches esp_camera and the
// image staging pool (docs/02 single-owner). Other callers (HTTP/AI) use the
// blocking-deadline OwnerTask<CameraReq> facade — no bypass, no synchronous
// cross-task callbacks.
//
// Staging: 2 slots x 96 KiB (cfg::kStagingPoolSize x kStagingMaxBytes),
// heap_caps_malloc MALLOC_CAP_SPIRAM with DRAM fallback (honestly logged).
// The 60 s TTL (cfg::kImageTtlMs) applies ONLY to unreferenced images and is
// swept LAZY on every op — no silent eviction when the pool is full (capture
// -> explicit RESOURCE_BUSY, docs/03). imageId "img-N" lives in RAM for this
// boot only; persistence = save to SD.
#include "common/config.h"
#include "common/error.h"
#include "ports/camera.h"

// owner_task.h is a project pattern living in src/, not a lib — resolved
// via CPPPATH when the build env provides src/, else a path relative to this
// file.
#if __has_include("owner_task.h")
#include "owner_task.h"
#else
#include "../../../src/owner_task.h"
#endif

namespace controlme {

enum class CameraOp : uint8_t {
  Init = 0,  // init esp_camera driver + detect psram/flash + allocate staging
  Capture,   // take 1 JPEG frame -> free staging slot
  Pin,       // refCount++ — pinned permanently past TTL
  Release,   // refCount--; reaching 0 -> lastRefMs=now (TTL starts running)
  Discard,   // free the slot immediately — one-shot consumer (HTTP snapshot)
  Read,      // stream slot contents to sink in 4 KiB under the providerReading flag
  Stat,      // metadata only, no stream
  Tick,      // explicit maintenance (TTL sweep) — hook for the supervisor
  StreamBegin,     // lock the driver: capture -> RESOURCE_BUSY until StreamEnd
  StreamPull,      // fb_get; PSRAM pointer in frame/frameLen; StreamRelease required
  StreamRelease,   // fb_return the currently held buffer
  StreamEnd        // return the buffer if still held, release the lock
};

// Request POD — OwnerTask contract: OwnerCtrl MUST be the first member.
// The `sink`/`meta` fields are used by the Read/Capture/Stat ops; sink is an
// object owned by the caller task that stays alive while the owner streams.
// `readDone` (Read op): facade-owned semaphore — given by the owner when the
// stream is truly finished, so a timed-out facade can wait before the sink
// (caller's stack) is destroyed — closes a use-after-free.
struct CameraReq {
  OwnerCtrl ctrl;
  CameraOp op = CameraOp::Tick;
  char imageId[cfg::kIdCap] = {};
  IByteSink* sink = nullptr;            // Read op only
  SemaphoreHandle_t readDone = nullptr; // Read op only
  ImageMeta meta{};                     // output: Capture/Stat/Read
  const uint8_t* frame = nullptr;       // output: StreamPull, PSRAM framebuffer
  uint32_t frameLen = 0;
};

class CameraOwner : public ICamera, public OwnerTask<CameraReq> {
 public:
  // Start the owner task — called once from the register module (caller task).
  Status begin();

  // --- facade: called from other tasks; blocking-deadline via queue ---
  bool isReady() override { return ready_; }
  Status initHw();           // Init op (one-time setup)
  Status tick();             // Tick op
  Status capture(ImageMeta& out) override;
  Status pin(const char* imageId) override;
  Status release(const char* imageId) override;
  Status discard(const char* imageId) override;
  Status readImage(const char* imageId, IByteSink& sink,
                   ImageMeta& metaOut) override;
  Status stat(const char* imageId, ImageMeta& metaOut) override;

  // Stream straight from the driver framebuffer (PSRAM). Only one holder.
  // pull locks the buffer until release. capture while locked -> RESOURCE_BUSY.
  Status beginStream();
  Status pullStreamFrame(const uint8_t*& data, size_t& len);
  Status releaseStreamFrame();
  Status endStream();

  // Init detection results — for the /capabilities feature fragment.
  uint32_t psramBytes() const { return psramBytes_; }
  uint32_t flashBytes() const { return flashBytes_; }
  uint8_t stagingSlots() const { return stagingAllocated_; }

 private:
  struct StagingSlot {
    uint8_t* buf = nullptr;          // kStagingMaxBytes, PSRAM when available
    ImageMeta meta{};
    uint8_t refCount = 0;
    uint32_t lastRefMs = 0;          // last reference / capture time
    bool providerReading = false;    // being streamed — TTL must not touch
    bool occupied = false;           // holding an image (valid imageId)
  };

  static void dispatchThunk(CameraReq& r, void* ctx);
  void handle(CameraReq& r);         // runs IN the owner task
  void handleInit(CameraReq& r);
  void handleCapture(CameraReq& r);
  void handlePin(CameraReq& r);
  void handleRelease(CameraReq& r);
  void handleDiscard(CameraReq& r);
  void handleRead(CameraReq& r);
  void handleStat(CameraReq& r);
  void handleStreamBegin(CameraReq& r);
  void handleStreamPull(CameraReq& r);
  void handleStreamRelease(CameraReq& r);
  void handleStreamEnd(CameraReq& r);
  void dropHeldFrame();

  StagingSlot* findById(const char* imageId);
  StagingSlot* findFree();
  void sweepExpired(uint32_t nowMs);
  static uint32_t nowMs();

  StagingSlot slots_[cfg::kStagingPoolSize];
  volatile bool ready_ = false;      // written by the owner task, read cross-task
  uint8_t stagingAllocated_ = 0;
  uint32_t imageSeq_ = 0;
  uint32_t psramBytes_ = 0;
  uint32_t flashBytes_ = 0;
  void* heldFb_ = nullptr;           // camera_fb_t*, owner task only
  bool streaming_ = false;           // owner task only
};

}  // namespace controlme
