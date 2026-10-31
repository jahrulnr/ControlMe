# Camera size, ratio, and flash

Call `camera_configure` with `{}` to inspect the current size, or with `{"size":"VGA"}` to change it. The response gives `size`, `width`, `height`, and `ratio`. Each preset fixes its ratio; ratio is not an independent crop control. Settings reset to QVGA at reboot. Size changes reinitialize the camera; a failure is reported and the previous mode is restored if possible. After a timeout, inspect the current mode before retrying.

Presets: `QQVGA` 160x120 (4:3), `240X240` (1:1), `QVGA` 320x240 (4:3), `CIF` 400x296 (50:37), `HVGA` 480x320 (3:2), `VGA` 640x480 (4:3), `SVGA` 800x600 (4:3), `XGA` 1024x768 (4:3), `HD` 1280x720 (16:9), `SXGA` 1280x1024 (5:4), and `UXGA` 1600x1200 (4:3). A 320x320 preset was excluded after capture failed on this device.

Practical starting choices, based on the tested indoor scene: use QVGA when a small image is useful and an inline MCP image matters; use VGA for clearer detail with a moderate download. Use 240X240 only when square framing is needed, and HD for 16:9. Use UXGA when detail matters more than transfer time and memory. JPEG sizes depend on the scene: in the test QVGA was about 4–6 KiB, VGA about 14–16 KiB, and UXGA about 170–190 KiB. Only complete images that fit the 8 KiB base64 attachment budget appear inline; otherwise fetch the `downloadUrl`. Each staging slot can grow to 384 KiB in PSRAM; larger frames or failed allocation return an error.

`camera_capture` accepts `{"flash":true}`; the default is off. The LED lights only for capture and is switched off afterward. The test showed bright flash reflections on a close object, so start without flash and use it when extra light helps. A flash exposure can also affect the next frame until auto-exposure settles.

Resolution alone does not prove a crop or wider view: compare the same static scene at the same camera position. The test images at QVGA, VGA, and UXGA showed broadly similar framing, but did not establish exact field-of-view equivalence. Changing aspect ratio can change which parts of a scene appear. See `camera` for staging lifetime and visual-control workflow.
