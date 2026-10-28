# 05 — ESP32-CAM Pin & Hardware Map

> The camera and SD pin map was checked against the **LOCAL Espressif reference** already read (not an online verification, not a unit test result). The exact unit in the photo has not been verified for revision/clone status.

## Modules (from the user's photo)

- **Classic AI-Thinker-style ESP32-CAM** + **USB programmer base** (likely **ESP32-CAM-MB**) — the exact revision/clone cannot be proven from the photo.
- Assumed nominal **4 MB flash + onboard PSRAM** (typically 4 MB; the exact capacity must be read from the boot flash info & PSRAM init — don't claim present/absent before reading).
- **Onboard OV2640** camera via FFC cable + power rail from the board regulator — factory-fixed connection, **no external DVP adapter**.
- **Onboard microSD slot** — factory-fixed connection to the SDMMC peripheral.
- The USB port on the programmer base is a **UART bridge** (not native ESP32 USB) — **GPIO1 (TX0) and GPIO3 (RX0) are reserved** for programming/console.

## Onboard Camera Pins (fixed-connection reference — not a rewire proposal)

`CAMERA_MODEL_AI_THINKER` mapping from the official Espressif example:

| Signal | GPIO |
|--------|------|
| PWDN | 32 |
| RESET | -1 (no dedicated reset GPIO) |
| XCLK | 0 |
| SCCB_SDA | 26 |
| SCCB_SCL | 27 |
| D0 / Y2 | 5 |
| D1 / Y3 | 18 |
| D2 / Y4 | 19 |
| D3 / Y5 | 21 |
| D4 / Y6 | 36 |
| D5 / Y7 | 39 |
| D6 / Y8 | 34 |
| D7 / Y9 | 35 |
| VSYNC | 25 |
| HREF | 23 |
| PCLK | 22 |

- These are **onboard connections**, not reusable header pins — don't treat them as "free pins".
- XCLK: LEDC **timer0 channel0, 20 MHz** (from the local example; explicit LEDC mode — the classic ESP32 has high-speed **and** low-speed modes, don't assume LS-only).
- Onboard flash LED = **GPIO4**; status LED = **GPIO33** (candidate, depends on the board variant — not a free header pin).

## Onboard SD Card (SDMMC slot 1, 1-bit width)

| Signal | GPIO | Note |
|--------|------|------|
| CLK | 14 | fixed slot 1 |
| CMD | 15 | fixed slot 1 |
| DAT0 | 2 | the only data line in 1-bit mode |
| DAT1 | 4 | physically wired; **unused** for data in 1-bit mode |
| DAT2 | 12 | physically wired; unused for data; GPIO12 is also a strapping pin |
| DAT3 | 13 | physically wired; **must be HIGH during card init** — if LOW, the card may enter SPI mode |

- **width=1 is configured explicitly** in the driver; 4-bit mode would seize pins shared with the servo/LED — not baseline.
- GPIO2 is a **strapping** pin (must be LOW to enter download) — an SD pull-up on this line can interfere with flashing.
- The local SD_MMC README requires a **~10 kΩ pull-up on DAT3**; don't add a new pull-up on GPIO12. **Pull-ups fitted on the board must be inspected** — don't assume they're all present.
- The classic ESP32 can't remap SDMMC pins freely like the newer series.

## Servo (conditional — shares pins with the SD card)

| Role | GPIO | Status |
|------|------|--------|
| Servo PAN (primary) | 13 | **candidate, conditional** — this pin is also SD DAT3; see coordination below |
| Servo TILT (optional) | 12 | **conditional** — VDD_SDIO strapping pin; an external HIGH pull during reset can select the wrong flash voltage → boot failure. Must pass power-on/reset/brown-out testing with the servo attached |

- **No two-servo guarantee** for all units — baseline is one pan servo; tilt only after electrical qualification. `servo_set` for an unconfigured channel → `NOT_SUPPORTED`.
- GPIO13 is not a "free pin" — it stays wired to the card's DAT3.
- When servo PWM is stopped for SD coordination, GPIO13 is driven **HIGH**; with no valid PWM the servo position may not hold (not a physical-safety guarantee).

## SD ↔ Servo Coordination (async, in DeviceActions — NOT blocking between owners)

Sequence on **first boot (before PWM is active)** and on **every SD re-init/remount**:

1. Suspend `servo_set` admission for channel 13.
2. Ask the Servo owner to stop PWM and drive GPIO13 **HIGH**; wait for the ACK **without holding any owner lock**.
3. Ask the Storage owner to mount SD slot1 width1.
4. Success → restore PWM to the last desired + reopen admission.
5. Failure → explicit `TIMEOUT`/`SD_UNAVAILABLE`; GPIO13 **stays HIGH** until recovery/user-controlled reconfigure — no blind PWM resume; `servo_set` → `RESOURCE_BUSY` while the handoff/recovery is unfinished.
6. Servo13 requests arriving during the handoff → `RESOURCE_BUSY` (not waiting forever).
7. Bounded deadline at each step; the Storage owner never waits on the Servo owner and vice versa (no deadlock).
8. Reboot → remount completes **before** PWM goes active. **The initial mount task must precede the PWM13 task** — including when no SD is fitted; a permanent no-SD mode is a separate manual configuration choice (review question), not automatic.

## Expander alternative (OPTIONAL — review trade-off, not baseline)

PCA9685 (I2C PWM, 16 channels) is a candidate for two servos: **SDA=3, SCL=1** — valid only if the programmer base is **removed or electrically isolated** from both UART pins at run time, the serial console is off after takeover, TX activity at boot is qualified, and PWM outputs are disabled during init. Not plug-and-play, not free GPIOs; it stays an option table with its constraints.

## Reserved Pins (do not use)

| GPIO | Reason |
|------|--------|
| 1, 3 | UART0 TX/RX — programming/console via the USB base (unless the expander route above is qualified) |
| 6–11 | internal SPI flash |
| 16, 17 | PSRAM (when fitted) |
| 0 | camera XCLK + download strap — don't add actuators |
| 2 | SD DAT0 + download strap |
| 4 | flash LED + SD DAT1 — **no servo PWM** |
| 12 | SD DAT2 + VDD_SDIO strap (conditional tilt candidate) |
| 13 | SD DAT3 (conditional pan candidate) |
| 14, 15 | SD CLK/CMD |
| 34–39 | input-only + camera data lines — not free outputs |

## Reference Sources (local, already read) & Official Checklist (not yet fetched)

**Local sources inspected (provenance: LOCAL, not online):**

- `~/.platformio/packages/framework-arduinoespressif32/package.json` — Arduino core 3.3.5.
- `~/.platformio/packages/framework-arduinoespressif32/libraries/ESP32/examples/Camera/CameraWebServer/camera_pins.h` (lines ~160–180) — AI_THINKER pins.
- `~/.platformio/packages/framework-arduinoespressif32/libraries/SD_MMC/README.md` — SD mapping + DAT3 pull-up/strap notes.
- `~/.platformio/packages/framework-arduinoespressif32-libs/esp32/include/soc/esp32/include/soc/sdmmc_pins.h` — fixed slot1 pins.
- `~/.platformio/platforms/espressif32/boards/esp32cam.json` — esp32 MCU, 4 MB flash, BOARD_HAS_PSRAM flag (**not** proof of actual capacity).

**Related official references (noted, not yet fetched/verified online):**

- https://github.com/espressif/arduino-esp32/tree/master/libraries/ESP32/examples/Camera/CameraWebServer
- https://github.com/espressif/arduino-esp32/tree/master/libraries/SD_MMC
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/sdmmc_host.html
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/sd_pullup_requirements.html
- https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf
- https://wiki.ai-thinker.com/esp32-cam

Items to check before final pins: VDD_SDIO strap (GPIO12), GPIO2 behavior during download, presence of the DAT3 pull-up on the unit, actual PSRAM size, the chosen LEDC HS/LS mode.
