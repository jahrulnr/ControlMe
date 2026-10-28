#pragma once
// Servo owner (adapter) — single owner of the servo LEDC PWM (docs/02,
// docs/05).
//
// Baseline: one "pan" channel on GPIO13 shared with SD DAT3. The channel
// table supports a second "tilt" slot (GPIO12, LEDC channel2) deliberately
// kept configured=false until it passes electrical qualification (docs/05
// Servo).
//
// LEDC: timer1 @50 Hz, 14-bit resolution, explicit speed_mode
// LEDC_LOW_SPEED_MODE — chosen because it exists on every ESP32 variant
// (high-speed is classic-ESP32-only), explicit duty latch via
// ledc_update_duty, and the high-speed group is left untouched for the camera
// XCLK path (timer0/channel0 @20 MHz).
//
// PWM is NOT attached at begin(): GPIO13 = DAT3 must be HIGH during card
// init, so PWM only goes active via restoreChannel() called by
// SdServoCoordinator at the end of mountSd — on success or failure. Channels
// are born suspended -> setAngle -> ResourceBusy until the first restore.
#include <stdint.h>
#include <driver/ledc.h>
#include "common/config.h"
#include "common/error.h"
#include "ports/servo.h"

// owner_task.h is a project pattern living in src/, not a lib — resolved
// via CPPPATH when the build env provides src/, else a path relative to this
// file.
#if __has_include("owner_task.h")
#include "owner_task.h"
#else
#include "../../../src/owner_task.h"
#endif

namespace controlme {

enum class ServoOp : uint8_t {
  SetAngle = 0,
  CurrentAngle,
  ChannelInfo,
  Suspend,
  DriveHigh,
  Restore,
};

// Request POD — OwnerTask contract: OwnerCtrl MUST be the first member.
struct ServoReq {
  OwnerCtrl ctrl;
  ServoOp op = ServoOp::SetAngle;
  char id[sizeof(ServoChannelInfo::id)] = {};
  float angleDeg = 0.f;                    // in:  setAngle
  float outDeg = 0.f;                      // out: currentAngle
  ServoChannelInfo info = {};              // out: channelInfo
};

// Default pulse<->degree mapping (docs/02); settings-based calibration = another slice.
struct ServoCalib {
  float angleMinDeg = 0.f;
  float angleMaxDeg = 180.f;
  uint32_t pulseMinUs = 500;
  uint32_t pulseMaxUs = 2400;
};

class ServoOwner : public IServo, public OwnerTask<ServoReq> {
 public:
  ServoOwner();

  // Start the owner task — called once from the register module (caller
  // task). Does not enable PWM / touch GPIO13.
  Status begin();

  // --- IServo: blocking-deadline facade, called from other tasks ---
  uint8_t channelCount() override;
  Status channelInfo(const char* id, ServoChannelInfo& out) override;
  Status setAngle(const char* id, float angleDeg) override;
  Status currentAngle(const char* id, float& outDeg) override;
  Status suspendChannel(const char* id) override;
  Status drivePinHigh(const char* id) override;
  Status restoreChannel(const char* id) override;
  bool isSuspended(const char* id) override;

 private:
  struct ChannelState {
    const char* id;              // static literal ("pan"/"tilt")
    uint8_t gpio;
    ledc_channel_t ledcChannel;
    bool configured;
    volatile bool suspended;     // written by the owner task; read atomically from outside
    bool pwmAttached;            // owner task only
    float desiredDeg;            // last requested angle — NOT a measured position
  };

  static void dispatchThunk(ServoReq& r, void* ctx);
  void handle(ServoReq& r);      // runs IN the owner task

  ChannelState* findChannel(const char* id);
  const ChannelState* findChannel(const char* id) const;
  uint32_t dutyTicks(float angleDeg) const;
  Status applyDuty(ChannelState& ch);
  Status attachPwm(ChannelState& ch);
  Status detachPwmToHigh(ChannelState& ch);

  ServoCalib calib_;
  ChannelState channels_[2];
  bool timerReady_ = false;      // LEDC timer1 — lazy on first attach
};

}  // namespace controlme
