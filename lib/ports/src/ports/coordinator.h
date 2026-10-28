#pragma once
// SD<->GPIO13 handoff coordinator port (docs/02 SdServoCoordinator,
// docs/05 sequence). Owned by the servo module; called by the composition
// root / paths needing exclusive SD access against servo13 PWM.
#include "common/error.h"

namespace controlme {

class ISdServoCoordinator {
 public:
  virtual ~ISdServoCoordinator() = default;

  // Mount SD with the full handoff: suspend servo13 admission -> drive
  // GPIO13 HIGH -> mount -> restore PWM. Restore also runs when mount fails
  // so the servo stays usable without a card; the next call repeats the
  // handoff before touching the bus. Blocking in the caller's task with an
  // internal bounded deadline. ResourceBusy when another handoff is running;
  // SdUnavailable when mount fails.
  virtual Status mountSd() = 0;
  // Controlled unmount (used by the supervisor / user request).
  virtual Status unmountSd() = 0;
  // true during a handoff transition — servo_set(GPIO13) -> ResourceBusy.
  virtual bool handoffActive() = 0;
};

}  // namespace controlme
