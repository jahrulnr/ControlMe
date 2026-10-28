#pragma once
// Time port. The domain uses IClock (mockable in host tests).
#include <stdint.h>

namespace controlme {

class IClock {
 public:
  virtual ~IClock() = default;
  virtual uint64_t monoMs() = 0;        // monotonic since boot
  virtual uint64_t wallMs() = 0;        // wall-clock epoch ms; valid only when wallSynced()
  virtual bool wallSynced() = 0;
};

}  // namespace controlme
