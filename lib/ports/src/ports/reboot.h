#pragma once
// Reboot port — used by the supervisor; shutdown sequence in docs/03.
#include "common/error.h"

namespace controlme {

class IReboot {
 public:
  virtual ~IReboot() = default;
  // Nonblocking: the supervisor runs the graceful sequence + deadline.
  virtual Status requestReboot(const char* reason) = 0;
  virtual bool isRebooting() = 0;
};

}  // namespace controlme
