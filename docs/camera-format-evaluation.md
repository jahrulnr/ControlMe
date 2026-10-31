# Camera format evaluation

On 2026-11-01, tested on the connected AI-Thinker ESP32-CAM (8 MiB PSRAM)
over MCP after flashing the size-selection tool. The camera remained at the
same commanded position; the indoor scene and automatic exposure could change
between frames. Results are samples, not throughput guarantees.

| Preset | Result | Example JPEG size |
| --- | --- | ---: |
| QQVGA 160x120 | Capture and inline attachment | 1.7–2.0 KiB |
| 240X240 | Capture and inline attachment | 4.0–5.0 KiB |
| QVGA 320x240 | Capture and inline attachment in these samples | 4.0–6.0 KiB |
| CIF 400x296 | Capture | about 8 KiB |
| HVGA 480x320 | Capture | about 8 KiB |
| VGA 640x480 | Capture and HTTP download | 14–16 KiB |
| SVGA 800x600 | Capture and HTTP download | 22–24 KiB |
| XGA 1024x768 | Capture and HTTP download | 49–53 KiB |
| HD 1280x720 | Capture and HTTP download | 62–65 KiB |
| SXGA 1280x1024 | Capture and HTTP download | 82–95 KiB |
| UXGA 1600x1200 | Capture and HTTP download after staging growth | 168–185 KiB |

The returned JPEG dimensions were checked from the JPEG headers; inline images
in the first comparison were byte-compared against HTTP downloads. The
320x320 preset configured but timed out twice while capturing, so it is not
advertised. A single initial transition to QQVGA failed with an SCCB write
error; the camera restored QVGA, and later transitions and QQVGA capture
succeeded. No permanent recovery guarantee is implied by that observation.

Repeated flash captures at QVGA, 240x240, VGA, and UXGA succeeded. The live
log recorded GPIO4 high during each requested pulse and low afterward; images
with flash showed strong brightening of the nearby object, while subsequent
non-flash captures returned to ambient illumination. The onboard SD card was
not mounted, so concurrent SD/flash operation was not tested.

QVGA, VGA, and UXGA showed broadly similar framing in the observed scene;
these shots were not a controlled optical calibration. Do not infer exact
field-of-view equivalence, or a predictable crop, from this sample alone.
