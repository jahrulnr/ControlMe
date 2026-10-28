#include "app.h"
#include "owner_task.h"

namespace controlme {

// Explicit instantiation: a header-only template is only parsed when used
// — this line forces OwnerTask to fully compile now (no surprise for the
// first module to use it later).
struct OwnerCompileProbeReq {
  OwnerCtrl ctrl;
  int value;
  int result;
};
template class OwnerTask<OwnerCompileProbeReq>;



bool App::addFeature(const char* jsonFragment) {
  if (!jsonFragment || featureCount >= kMaxFeatures) return false;
  features[featureCount++] = jsonFragment;
  return true;
}

}  // namespace controlme
