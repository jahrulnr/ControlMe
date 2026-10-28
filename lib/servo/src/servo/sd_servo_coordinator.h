#pragma once
// SdServoCoordinator — orchestrates the SD<->GPIO13 handoff (docs/05
// SD<->Servo Coordination). Its own small owner task: mountSd()/unmountSd()
// from outside = enqueue + bounded wait; the steps run SERIALIZED in the
// coordinator task via the blocking-deadline facades of the Servo & Storage
// owners — no cross-owner locks (docs/02 Concurrency: no locks while
// awaiting).
//
// mountSd sequence: suspend "pan" admission -> drivePinHigh (PWM off, GPIO13
// HIGH, DAT3 init requirement) -> storage->mount() -> restoreChannel (PWM +
// open admission), whether mount succeeds or fails. A failure still returns
// the SD error. The next mountSd() repeats suspend → HIGH → mount before the
// bus is touched.
#include "common/config.h"
#include "common/error.h"
#include "ports/coordinator.h"
#include "ports/servo.h"
#include "ports/storage.h"

// owner_task.h is a project pattern living in src/, not a lib — resolved
// via CPPPATH when the build env provides src/, else a path relative to this
// file.
#if __has_include("owner_task.h")
#include "owner_task.h"
#else
#include "../../../src/owner_task.h"
#endif

namespace controlme {

enum class CoordOp : uint8_t { MountSd = 0, UnmountSd };

// Request POD — OwnerTask contract: OwnerCtrl MUST be the first member.
struct CoordReq {
  OwnerCtrl ctrl;
  CoordOp op = CoordOp::MountSd;
};

class SdServoCoordinator : public ISdServoCoordinator,
                           public OwnerTask<CoordReq> {
 public:
  // servo: required (reference) — GPIO13 handoff target.
  // storage: may be nullptr -> mountSd/unmountSd honestly reply SdUnavailable.
  SdServoCoordinator(IServo& servo, IStorage* storage);

  // Start the coordinator task — called once from the register module.
  Status begin();

  // --- ISdServoCoordinator: facade blocking-deadline ---
  Status mountSd() override;
  Status unmountSd() override;
  bool handoffActive() override { return handoffActive_; }

 private:
  static void dispatchThunk(CoordReq& r, void* ctx);
  Status doMount();      // runs in the coordinator task
  Status doUnmount();

  IServo& servo_;
  IStorage* storage_;
  volatile bool handoffActive_ = false;   // written by the coordinator task only
};

}  // namespace controlme
