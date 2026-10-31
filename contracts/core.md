# ControlMe: core contract

ControlMe is an ESP32-CAM controlled through MCP. Call `contract` with no arguments first. Use `contract` with a `reference` to load details only when needed:

- `camera`: capture, image lifetime, and visual feedback.
- `camera-format`: tested size, aspect-ratio, and flash guidance.
- `motion`: pan/tilt control and interpreting the result.
- `storage`: SD listing, reads, and staged writes.
- `device`: status, time, logs, errors, and reboot.

Check `tools/list` for the actual available tools and argument schemas. Call `device_status` before relying on device readiness or SD availability. A successful command reports what the device accepted or applied; it does not establish a physical outcome. Observe the result and adjust in small steps when controlling hardware. Treat tool errors as results: inspect the error, check status, and retry only when appropriate. After a timeout, the operation may still finish; check the resulting state before repeating a change.

If you do not know how a servo moves the camera, capture an image, move one servo by a small amount, let it settle, and capture another image. Compare stationary features in the images to infer the servo's axis and direction. Repeat for the other servo as needed.
