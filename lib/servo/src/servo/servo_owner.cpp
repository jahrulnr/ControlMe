// ServoOwner implementation — all LEDC/GPIO mutation runs IN the owner
// task (confinement). Caller tasks only interact through the call() facade.
#include "servo/servo_owner.h"

#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_log.h>
#include <string.h>
#include "common/config.h"
#include "common/event.h"   // copyStr

namespace controlme {

static const char* kTag = "servo";

// Pin candidates from docs/05 — pan is used, tilt conditional (not configured yet).
static const char* kPanId = "pan";
static const char* kTiltId = "tilt";
static constexpr int kPanGpio = 13;
static constexpr int kTiltGpio = 12;

static constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;  // header
static constexpr ledc_timer_t kTimer = LEDC_TIMER_1;
static constexpr ledc_timer_bit_t kDutyRes = LEDC_TIMER_14_BIT;  // >=14 bit (docs/02)
static constexpr uint32_t kDutyPeriod = 1u << 14;   // ticks per period
static constexpr uint32_t kFreqHz = 50;
static constexpr float kInitialAngleDeg = 90.f;     // neutral on first attach
static constexpr uint32_t kCtlTimeoutMs = cfg::kToolTimeoutMs;   // facade deadline

static Status mapEspErr(esp_err_t e, const char* what) {
  ESP_LOGE(kTag, "%s failed: %s", what, esp_err_to_name(e));
  return Status::Fail(e == ESP_ERR_TIMEOUT ? Err::Timeout : Err::Internal, what);
}

ServoOwner::ServoOwner()
    : channels_{
          // Channels are born suspended: PWM13 may only go active via the
          // post-SD-mount restore (docs/05 step 8 — mount precedes PWM).
          {kPanId, kPanGpio, LEDC_CHANNEL_1, /*configured*/ true,
           /*suspended*/ true, /*attached*/ false, kInitialAngleDeg},
          {kTiltId, kTiltGpio, LEDC_CHANNEL_2, /*configured*/ false,
           /*suspended*/ true, /*attached*/ false, kInitialAngleDeg},
      } {}

// ------------------------------------------------------------------ facade

Status ServoOwner::begin() {
  return start(OwnerTaskConfig<ServoReq>{
      .name = "servo",
      .stackBytes = cfg::kStackServo,     // bytes — ESP-IDF convention
      .priority = cfg::kPrioServo,
      .queueDepth = cfg::kServoQueueDepth,
      .handle = &ServoOwner::dispatchThunk,
      .ctx = this,
      .core = cfg::kCoreApp});
}

uint8_t ServoOwner::channelCount() {
  uint8_t n = 0;
  for (const ChannelState& ch : channels_) {
    if (ch.configured) ++n;
  }
  return n;
}

Status ServoOwner::channelInfo(const char* id, ServoChannelInfo& out) {
  ServoReq in{}, res{};
  in.op = ServoOp::ChannelInfo;
  copyStr(in.id, sizeof(in.id), id);
  Status st = call(in, res, kCtlTimeoutMs);
  if (st.ok()) out = res.info;
  return st;
}

Status ServoOwner::setAngle(const char* id, float angleDeg) {
  ServoReq in{}, out{};
  in.op = ServoOp::SetAngle;
  copyStr(in.id, sizeof(in.id), id);
  in.angleDeg = angleDeg;
  return call(in, out, kCtlTimeoutMs);   // Ok = COMMAND_APPLIED
}

Status ServoOwner::currentAngle(const char* id, float& outDeg) {
  ServoReq in{}, res{};
  in.op = ServoOp::CurrentAngle;
  copyStr(in.id, sizeof(in.id), id);
  Status st = call(in, res, kCtlTimeoutMs);
  if (st.ok()) outDeg = res.outDeg;
  return st;
}

Status ServoOwner::suspendChannel(const char* id) {
  ServoReq in{}, out{};
  in.op = ServoOp::Suspend;
  copyStr(in.id, sizeof(in.id), id);
  return call(in, out, kCtlTimeoutMs);
}

Status ServoOwner::drivePinHigh(const char* id) {
  ServoReq in{}, out{};
  in.op = ServoOp::DriveHigh;
  copyStr(in.id, sizeof(in.id), id);
  return call(in, out, kCtlTimeoutMs);
}

Status ServoOwner::restoreChannel(const char* id) {
  ServoReq in{}, out{};
  in.op = ServoOp::Restore;
  copyStr(in.id, sizeof(in.id), id);
  return call(in, out, kCtlTimeoutMs);
}

bool ServoOwner::isSuspended(const char* id) {
  // Fail-closed: unknown/unconfigured channels never admit.
  const ChannelState* ch = findChannel(id);
  return !ch || !ch->configured || ch->suspended;
}

// ------------------------------------------------------------- task owner

void ServoOwner::dispatchThunk(ServoReq& r, void* ctx) {
  static_cast<ServoOwner*>(ctx)->handle(r);
}

void ServoOwner::handle(ServoReq& r) {
  ChannelState* ch = findChannel(r.id);
  switch (r.op) {
    case ServoOp::ChannelInfo:
      if (!ch) {
        r.ctrl.status = Status::Fail(Err::NotFound, "unknown servo id");
      } else {
        copyStr(r.info.id, sizeof(r.info.id), ch->id);
        r.info.gpio = ch->gpio;
        r.info.configured = ch->configured;
        r.ctrl.status = Status::Ok();
      }
      break;

    case ServoOp::SetAngle:
      if (!ch || !ch->configured) {
        r.ctrl.status = Status::Fail(Err::NotSupported, "servo channel");
      } else if (ch->suspended) {
        r.ctrl.status = Status::Fail(Err::ResourceBusy, "servo handoff");
      } else {
        // Clamp to the [0,180] calibration range — absolute target = idempotent.
        float a = r.angleDeg;
        if (a < calib_.angleMinDeg) a = calib_.angleMinDeg;
        if (a > calib_.angleMaxDeg) a = calib_.angleMaxDeg;
        ch->desiredDeg = a;
        // COMMAND_APPLIED: signal applied, NOT a position claim (docs/03).
        // Invariant suspended=false -> attached; defensive path stays honest.
        r.ctrl.status = ch->pwmAttached
                            ? applyDuty(*ch)
                            : Status::Fail(Err::ResourceBusy, "pwm not active");
      }
      break;

    case ServoOp::CurrentAngle:
      if (!ch) {
        r.ctrl.status = Status::Fail(Err::NotFound, "unknown servo id");
      } else if (!ch->configured) {
        r.ctrl.status = Status::Fail(Err::NotSupported, "servo channel");
      } else {
        r.outDeg = ch->desiredDeg;   // commanded angle — no position feedback
        r.ctrl.status = Status::Ok();
      }
      break;

    case ServoOp::Suspend:
      if (!ch) {
        r.ctrl.status = Status::Fail(Err::NotFound, "unknown servo id");
      } else if (!ch->configured) {
        r.ctrl.status = Status::Fail(Err::NotSupported, "servo channel");
      } else {
        ch->suspended = true;   // close admission; PWM may keep running
        r.ctrl.status = Status::Ok();
      }
      break;

    case ServoOp::DriveHigh:
      if (!ch) {
        r.ctrl.status = Status::Fail(Err::NotFound, "unknown servo id");
      } else if (!ch->configured) {
        r.ctrl.status = Status::Fail(Err::NotSupported, "servo channel");
      } else {
        r.ctrl.status = detachPwmToHigh(*ch);
      }
      break;

    case ServoOp::Restore:
      if (!ch) {
        r.ctrl.status = Status::Fail(Err::NotFound, "unknown servo id");
      } else if (!ch->configured) {
        r.ctrl.status = Status::Fail(Err::NotSupported, "servo channel");
      } else {
        Status st = attachPwm(*ch);
        if (st.ok()) {
          ch->suspended = false;   // open admission (docs/05 step 4)
        } else {
          // Attach failed: don't leave the pin ambiguous — force it back
          // HIGH (best effort; the original status is still reported as-is).
          detachPwmToHigh(*ch);
        }
        r.ctrl.status = st;
      }
      break;
  }
}

// ------------------------------------------------------------------ helpers

ServoOwner::ChannelState* ServoOwner::findChannel(const char* id) {
  if (!id) return nullptr;
  for (ChannelState& ch : channels_) {
    if (strncmp(ch.id, id, sizeof(ServoChannelInfo::id)) == 0) return &ch;
  }
  return nullptr;
}

const ServoOwner::ChannelState* ServoOwner::findChannel(const char* id) const {
  if (!id) return nullptr;
  for (const ChannelState& ch : channels_) {
    if (strncmp(ch.id, id, sizeof(ServoChannelInfo::id)) == 0) return &ch;
  }
  return nullptr;
}

uint32_t ServoOwner::dutyTicks(float angleDeg) const {
  const float span = calib_.angleMaxDeg - calib_.angleMinDeg;
  float frac = span > 0.f ? (angleDeg - calib_.angleMinDeg) / span : 0.f;
  if (frac < 0.f) frac = 0.f;
  if (frac > 1.f) frac = 1.f;
  const float pulseUs = (float)calib_.pulseMinUs +
                        frac * (float)(calib_.pulseMaxUs - calib_.pulseMinUs);
  // duty = pulse_us / period_us * 2^res ;  period_us = 1e6 / freq
  float duty = pulseUs * (float)kDutyPeriod * (float)kFreqHz / 1000000.f;
  // Never reach 2^res — full-duty hw bug at max resolution (ledc.c note).
  if (duty < 0.f) duty = 0.f;
  if (duty > (float)(kDutyPeriod - 1)) duty = (float)(kDutyPeriod - 1);
  return (uint32_t)duty;
}

Status ServoOwner::applyDuty(ChannelState& ch) {
  esp_err_t e = ledc_set_duty(kSpeedMode, ch.ledcChannel, dutyTicks(ch.desiredDeg));
  if (e != ESP_OK) return mapEspErr(e, "ledc_set_duty");
  // Low-speed mode: a new duty only takes effect after an explicit update.
  e = ledc_update_duty(kSpeedMode, ch.ledcChannel);
  if (e != ESP_OK) return mapEspErr(e, "ledc_update_duty");
  return Status::Ok();
}

Status ServoOwner::attachPwm(ChannelState& ch) {
  if (!timerReady_) {
    ledc_timer_config_t tcfg = {};
    tcfg.speed_mode = kSpeedMode;
    tcfg.duty_resolution = kDutyRes;
    tcfg.timer_num = kTimer;
    tcfg.freq_hz = kFreqHz;
    tcfg.clk_cfg = LEDC_AUTO_CLK;   // LS on ESP32 -> APB 80 MHz
    tcfg.deconfigure = false;
    esp_err_t e = ledc_timer_config(&tcfg);
    if (e != ESP_OK) return mapEspErr(e, "ledc_timer_config");
    timerReady_ = true;
  }
  ledc_channel_config_t ccfg = {};
  ccfg.gpio_num = ch.gpio;
  ccfg.speed_mode = kSpeedMode;
  ccfg.channel = ch.ledcChannel;
  ccfg.intr_type = LEDC_INTR_DISABLE;
  ccfg.timer_sel = kTimer;
  ccfg.duty = dutyTicks(ch.desiredDeg);   // restore the last desired
  ccfg.hpoint = 0;
  ccfg.sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD;
  esp_err_t e = ledc_channel_config(&ccfg);
  if (e != ESP_OK) return mapEspErr(e, "ledc_channel_config");
  ch.pwmAttached = true;
  return Status::Ok();
}

Status ServoOwner::detachPwmToHigh(ChannelState& ch) {
  // ledc_stop is only valid when the LEDC group was initialized (attached)
  // before — otherwise ESP_ERR_INVALID_STATE. Idle level 1 = pin HIGH via the
  // LEDC path.
  if (ch.pwmAttached) {
    esp_err_t e = ledc_stop(kSpeedMode, ch.ledcChannel, 1);
    if (e != ESP_OK) return mapEspErr(e, "ledc_stop");
    ch.pwmAttached = false;
  }
  // Release the pin from the LEDC signal and drive it HIGH as a plain
  // GPIO: the level is set FIRST before it becomes an output — this ordering
  // prevents a LOW glitch while routing switches from the LEDC signal to the
  // GPIO register.
  esp_err_t e = gpio_set_level((gpio_num_t)ch.gpio, 1);
  if (e != ESP_OK) return mapEspErr(e, "gpio_set_level");
  e = gpio_set_direction((gpio_num_t)ch.gpio, GPIO_MODE_OUTPUT);
  if (e != ESP_OK) return mapEspErr(e, "gpio_set_direction");
  return Status::Ok();
}

}  // namespace controlme
