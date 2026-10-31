#include "domain/image_staging.h"

namespace controlme {

int selectImageStagingSlot(const ImageStagingCandidate* slots, size_t count) {
  int oldest = -1;
  for (size_t i = 0; i < count; ++i) {
    const auto& slot = slots[i];
    if (!slot.allocated || slot.pinned) continue;
    if (!slot.occupied) return static_cast<int>(i);
    if (oldest < 0 || slot.captureOrder < slots[oldest].captureOrder)
      oldest = static_cast<int>(i);
  }
  return oldest;
}

}  // namespace controlme
