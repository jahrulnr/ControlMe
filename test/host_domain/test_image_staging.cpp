#include "domain/image_staging.h"
#include <assert.h>

using namespace controlme;

int main() {
  ImageStagingCandidate slots[] = {
      {true, true, false, 2}, {true, true, false, 1}};
  assert(selectImageStagingSlot(slots, 2) == 1);
  slots[1].captureOrder = 3;  // third capture replaces the oldest
  assert(selectImageStagingSlot(slots, 2) == 0);
  slots[0].captureOrder = 4;
  assert(selectImageStagingSlot(slots, 2) == 1);

  slots[1].pinned = true;
  assert(selectImageStagingSlot(slots, 2) == 0);
  slots[0].pinned = true;
  assert(selectImageStagingSlot(slots, 2) == -1);
  slots[0] = {true, false, false, 0};
  assert(selectImageStagingSlot(slots, 2) == 0);
  slots[0].allocated = false;
  assert(selectImageStagingSlot(slots, 2) == -1);
  slots[1] = {true, true, false, 1};
  slots[0] = {true, false, false, 0};
  assert(selectImageStagingSlot(slots, 2) == 0);
  return 0;
}
