#pragma once
// Servo port — single owner of the servo LEDC PWM.
// SD<->GPIO13 handoff: the coordinator requests suspend/driveHigh/resume
// through here. PWM off -> pin driven HIGH (SD card init requirement,
// docs/05).
#include "common/error.h"

namespace controlme {

struct ServoChannelInfo {
  char id[16] = {};          // "pan" | "tilt"
  uint8_t gpio = 0;
  bool configured = false;
};

class IServo {
 public:
  virtual ~IServo() = default;

  // Count & info of actually-configured channels (capabilities).
  virtual uint8_t channelCount() = 0;
  virtual Status channelInfo(const char* id, ServoChannelInfo& out) = 0;

  // Mutation: COMMAND_APPLIED = signal applied, NOT a position claim.
  // Unconfigured channel -> NotSupported; mid-handoff -> ResourceBusy.
  virtual Status setAngle(const char* id, float angleDeg) = 0;
  virtual Status currentAngle(const char* id, float& outDeg) = 0;

  // --- SD<->GPIO13 coordination hooks (called by SdServoCoordinator only) ---
  virtual Status suspendChannel(const char* id) = 0;   // close admission for this channel
  virtual Status drivePinHigh(const char* id) = 0;     // PWM off + GPIO HIGH
  virtual Status restoreChannel(const char* id) = 0;   // PWM on to the last desired + open admission
  virtual bool isSuspended(const char* id) = 0;
};

}  // namespace controlme
