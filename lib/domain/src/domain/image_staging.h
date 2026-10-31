#pragma once
#include <stddef.h>
#include <stdint.h>

namespace controlme {

struct ImageStagingCandidate {
  bool allocated;
  bool occupied;
  bool pinned;
  uint64_t captureOrder;
};

// Prefer a free slot; otherwise replace the oldest unpinned capture.
// Return -1 when no slot can be used.
int selectImageStagingSlot(const ImageStagingCandidate* slots, size_t count);

}  // namespace controlme
