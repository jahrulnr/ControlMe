// Servo module — wires ServoOwner + SdServoCoordinator into App.
// Called from setup() (composition root) as controlme_register_servo().
//
// GPIO13 PWM is intentionally NOT started here: that pin is SD DAT3 and must
// be HIGH during card init, so PWM only comes on via the restore at the end
// of app.coord->mountSd() (docs/05 step 8 — the mount attempt precedes
// PWM13). A failed mount still restores PWM; servo_set -> RESOURCE_BUSY
// only while a handoff is running.
//
// No HTTP routes in this slice — /actions endpoints belong to the next wave.
#include <esp_log.h>
#include "app.h"
#include "servo/sd_servo_coordinator.h"
#include "servo/servo_owner.h"

namespace {
constexpr char kTag[] = "mod_servo";
}

void controlme_register_servo(controlme::App& app) {
  static controlme::ServoOwner s_servo;
  // The coordinator captures app.storage AT register time: if the storage
  // module exists it MUST be registered before servo; nullptr stays honest
  // -> mountSd() answers SdUnavailable until the wiring is fixed.
  static controlme::SdServoCoordinator s_coord(s_servo, app.storage);

  controlme::Status st = s_servo.begin();
  if (!st.ok()) {
    ESP_LOGE(kTag, "servo owner start failed: %s",
             st.detail ? st.detail : controlme::errName(st.code));
  } else {
    // tilt (GPIO12) is not SD-shared — restore it right away instead of
    // waiting for mountSd's restore, which only owns GPIO13. Channels are
    // born suspended; restore attaches PWM at the neutral angle.
    controlme::Status rt = s_servo.restoreChannel("tilt");
    if (!rt.ok()) {
      ESP_LOGW(kTag, "tilt restore failed: %s",
               rt.detail ? rt.detail : controlme::errName(rt.code));
    }
  }
  st = s_coord.begin();
  if (!st.ok()) {
    ESP_LOGE(kTag, "sd-servo coordinator start failed: %s",
             st.detail ? st.detail : controlme::errName(st.code));
  }

  // Slots are filled even when start fails — the facade itself answers
  // Internal ("owner not started"), more honest than a vague null pointer.
  app.servo = &s_servo;
  app.coord = &s_coord;

  app.addFeature("{\"servo\":{\"channels\":[\"pan\",\"tilt\"]}}");
}
