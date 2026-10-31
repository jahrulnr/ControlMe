#pragma once
// Camera port — owner of the OV2640 framebuffer + PSRAM staging pool.
// Staging: max 2 objects, growing in PSRAM as needed, 60 s TTL for unreferenced ones.
// imageId lives in RAM for this boot only; persistence = save to SD.
#include "common/error.h"
#include "common/config.h"
#include "ports/writer.h"

namespace controlme {

enum class CameraSize : uint8_t {
  QQVGA, Square240, QVGA, CIF, HVGA, VGA, SVGA, XGA, HD, SXGA, UXGA
};

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
  // A null requested size reads the active size; changing it reconfigures
  // the camera in its owner task. The setting resets at reboot.
  virtual Status configure(const CameraSize* requested, CameraSize& active) = 0;
  // Capture replaces the oldest unpinned staged image when full. If all
  // slots are pinned -> ResourceBusy; frame > budget -> ImageTooLarge.
  virtual Status capture(ImageMeta& out, bool flash = false) = 0;
  // Pin/unpin ref (run attachments etc.); pinned permanently past TTL.
  virtual Status pin(const char* imageId) = 0;
  virtual Status release(const char* imageId) = 0;
  // Discard the image now — the slot becomes free WITHOUT waiting for the
  // 60 s TTL. For one-shot consumers (HTTP snapshot) that don't expose an
  // imageId: without this, each snapshot holds a slot ~60 s so a ~1 fps
  // stream drains the 2-slot pool -> permanent RESOURCE_BUSY.
  virtual Status discard(const char* imageId) = 0;
  // Stream image bytes to the sink (4 KiB chunks; sink.write runs in the
  // caller's task — a slow client never occupies the camera owner);
  // unknown mime -> NotSupported.
  virtual Status readImage(const char* imageId, IByteSink& sink, ImageMeta& metaOut) = 0;
  virtual Status stat(const char* imageId, ImageMeta& metaOut) = 0;
};

}  // namespace controlme
