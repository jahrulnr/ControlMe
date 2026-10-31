# Camera and visual feedback

Call `camera_capture` with `{}` to capture a JPEG without flash, or with `{"flash":true}` to pulse the flash during the capture. Its metadata includes `imageId`, dimensions, size, and `downloadUrl`. When the image fits, MCP also returns an inline image block; otherwise use the download URL. `image_read` accepts `imageId` for a staged image or `path` for an SD image. Call `contract` with `{"reference":"camera-format"}` for tested size, ratio, and flash guidance.

Staging is temporary RAM: at most two allocated slots, each initially 96 KiB and able to grow to 384 KiB in PSRAM. Unreferenced images expire after 60 seconds; when full, a new capture replaces the oldest unpinned image. An old ID or URL can therefore return `NOT_FOUND`. Images in active reads are pinned; if all slots are pinned, capture can return `RESOURCE_BUSY`. A reboot also clears staged images. Fetch an image promptly when it matters; staging is not persistent storage.

For visual control, capture, inspect, move in modest increments, allow the hardware to settle, and capture again. An applied servo command does not prove that the target was reached.
