#pragma once
// Camera port — owner of the OV2640 framebuffer + PSRAM staging pool.
// Staging: max 2 objects x 96 KiB (cfg), 60 s TTL only for unreferenced ones.
// imageId lives in RAM for this boot only; persistence = save to SD.
#include "common/error.h"
#include "common/config.h"
#include "ports/writer.h"

namespace controlme {

struct ImageMeta {
  char imageId[cfg::kIdCap] = {};
  char mime[24] = "image/jpeg";
  uint32_t bytes = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  bool pinned = false;
};

class ICamera {
 public:
  virtual ~ICamera() = default;

  virtual bool isReady() = 0;
  // Capture -> staging pool; full pool -> ResourceBusy; frame > budget -> ImageTooLarge.
  virtual Status capture(ImageMeta& out) = 0;
  // Pin/unpin ref (run attachments etc.); pinned permanently past TTL.
  virtual Status pin(const char* imageId) = 0;
  virtual Status release(const char* imageId) = 0;
  // Discard the image now — the slot becomes free WITHOUT waiting for the
  // 60 s TTL. For one-shot consumers (HTTP snapshot) that don't expose an
  // imageId: without this, each snapshot holds a slot ~60 s so a ~1 fps
  // stream drains the 2-slot pool -> permanent RESOURCE_BUSY.
  virtual Status discard(const char* imageId) = 0;
  // Stream image bytes to the sink (4 KiB chunks); unknown mime -> NotSupported.
  virtual Status readImage(const char* imageId, IByteSink& sink, ImageMeta& metaOut) = 0;
  virtual Status stat(const char* imageId, ImageMeta& metaOut) = 0;
};

}  // namespace controlme
