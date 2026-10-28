// SdServoCoordinator implementation — the entire handoff sequence runs IN
// the coordinator task; each step is a deadline-bounded facade call to
// another owner.
#include "servo/sd_servo_coordinator.h"

#include <esp_log.h>
#include "common/config.h"

namespace controlme {

static const char* kTag = "coord";
static const char* kPanId = "pan";
static constexpr uint32_t kQueueDepth = 4;
static constexpr uint32_t kStackBytes = 4 * 1024;   // docs/02: small 4 KiB task
// Caller deadline fence for the WHOLE handoff: 4 steps (suspend,
// driveHigh, mount, restore), each already deadline-bounded by
// kToolTimeoutMs inside its owner facade.
static constexpr uint32_t kHandoffDeadlineMs = 4 * cfg::kToolTimeoutMs;

SdServoCoordinator::SdServoCoordinator(IServo& servo, IStorage* storage)
    : servo_(servo), storage_(storage) {}

Status SdServoCoordinator::begin() {
  return start(OwnerTaskConfig<CoordReq>{
      .name = "coord",
      .stackBytes = kStackBytes,
      .priority = cfg::kPrioCoordinator,
      .queueDepth = kQueueDepth,
      .handle = &SdServoCoordinator::dispatchThunk,
      .ctx = this,
      .core = cfg::kCoreApp});
}

// ------------------------------------------------------------------ facade

Status SdServoCoordinator::mountSd() {
  if (handoffActive_) {
    // Another handoff in progress -> fail fast, don't wait forever.
    return Status::Fail(Err::ResourceBusy, "sd-servo handoff active");
  }
  CoordReq in{}, out{};
  in.op = CoordOp::MountSd;
  return call(in, out, kHandoffDeadlineMs);
}

Status SdServoCoordinator::unmountSd() {
  if (handoffActive_) {
    return Status::Fail(Err::ResourceBusy, "sd-servo handoff active");
  }
  CoordReq in{}, out{};
  in.op = CoordOp::UnmountSd;
  return call(in, out, kHandoffDeadlineMs);
}

// ------------------------------------------------------------- task owner

void SdServoCoordinator::dispatchThunk(CoordReq& r, void* ctx) {
  SdServoCoordinator& self = *static_cast<SdServoCoordinator*>(ctx);
  self.handoffActive_ = true;
  r.ctrl.status = (r.op == CoordOp::MountSd) ? self.doMount()
                                           : self.doUnmount();
  self.handoffActive_ = false;
}

Status SdServoCoordinator::doMount() {
  if (!storage_) {
    return Status::Fail(Err::SdUnavailable, "storage owner not installed");
  }
  // Idempotent: already mounted & servo admission normal -> no handoff
  // needed. Mounted but suspended (leftover of a past failure) -> restore
  // directly; the card is already initialized so the pin is free.
  if (storage_->isMounted() && !servo_.isSuspended(kPanId)) {
    return Status::Ok();
  }

  // (1) Close servo_set admission for GPIO13.
  Status st = servo_.suspendChannel(kPanId);
  if (!st.ok()) {
    ESP_LOGW(kTag, "servo suspend failed: %s", errName(st.code));
    return st;
  }

  if (!storage_->isMounted()) {
    // (2) PWM off + GPIO13 HIGH — card init requirement (DAT3).
    st = servo_.drivePinHigh(kPanId);
    if (!st.ok()) {
      ESP_LOGW(kTag, "drivePinHigh failed: %s", errName(st.code));
      return st;   // admission stays closed; pin state as reported by servo
    }
    // (3) Mount SD slot1 width1 — storage owner's internal deadline.
    st = storage_->mount();
    if (!st.ok()) {
      // Card didn't come up (absent / init failed). The SD bus isn't
      // holding GPIO13, so PWM must come back on — otherwise servo_set stays
      // rejected RESOURCE_BUSY forever with no card. The next mountSd() still
      // repeats suspend → HIGH → mount before touching the bus.
      ESP_LOGW(kTag, "sd mount failed: %s — servo restored",
               errName(st.code));
      Status rs = servo_.restoreChannel(kPanId);
      if (!rs.ok()) {
        ESP_LOGW(kTag, "servo restore after failed mount: %s",
                 errName(rs.code));
        return rs;
      }
      return st;
    }
  }

  // (4) Restore PWM to the last desired + open admission.
  st = servo_.restoreChannel(kPanId);
  if (!st.ok()) {
    // SD is mounted but servo admission stays closed — reported as-is;
    // the next mountSd() heals it via the restore-only path.
    ESP_LOGW(kTag, "servo restore failed: %s", errName(st.code));
  }
  return st;
}

Status SdServoCoordinator::doUnmount() {
  if (!storage_) {
    return Status::Fail(Err::SdUnavailable, "storage owner not installed");
  }
  if (!storage_->isMounted()) {
    return Status::Ok();   // nothing to release; servo state untouched
  }
  // Symmetric sequence: admission closed + pin HIGH during card release.
  Status st = servo_.suspendChannel(kPanId);
  if (!st.ok()) return st;
  st = servo_.drivePinHigh(kPanId);
  if (!st.ok()) return st;
  st = storage_->unmount();
  if (!st.ok()) {
    // The card may still be electrically active — leave suspended + HIGH.
    ESP_LOGW(kTag, "sd unmount failed: %s", errName(st.code));
    return st;
  }
  // SD slot released -> GPIO13 free -> PWM may run again.
  return servo_.restoreChannel(kPanId);
}

}  // namespace controlme
