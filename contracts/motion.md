# Pan and tilt

Use `servo_set` with `{"servoId":"pan","angleDeg":90}` or `{"servoId":"tilt","angleDeg":90}`. Angles are absolute degrees; the firmware clamps them to 0–180. `COMMAND_APPLIED` means the PWM command was applied, not that the servo reached the position: there is no position feedback. Avoid assuming an exact physical field of view or direction without observing the installation.

For framing, use a capture as feedback, make a small angle adjustment, let motion settle, then capture again. If `RESOURCE_BUSY` reports a servo handoff, the shared SD/pan pin is busy; retry after the handoff rather than issuing repeated rapid commands. The board's physical wiring and servo travel can limit the usable range.
