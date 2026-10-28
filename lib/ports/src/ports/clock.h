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
  // Set the wall clock from an external source (agent/manual) — used when
  // SNTP can't reach a server. A later SNTP sync overrides it.
  virtual bool setWallMs(uint64_t epochMs) = 0;
  // Which source synced wallMs last: "none" | "manual" | "sntp".
  virtual const char* timeSource() = 0;
};

}  // namespace controlme
