---
name: controlme-firmware
description: Modify and debug ControlMe ESP32-CAM firmware built with PlatformIO, Arduino/ESP-IDF, and FreeRTOS. Use for owner tasks, ports and App wiring, camera, SD, servos, LittleFS logs, reboot, PSRAM use, or firmware build failures.
---

# ControlMe Firmware

## Find The Change Boundary

1. Read `AGENTS.md`, `platformio.ini`, `src/app.h`, and the relevant sections of
   `src/main.cpp` and `src/mod_*.cpp`.
2. Read the related interface in `lib/ports/src/ports/` before changing adapters.
3. Follow `src/owner_task.h` and the closest existing device owner. Keep pure
   logic in host-testable libraries and wiring in the composition root. Do not
   create a framework or a new port without a concrete need.

## Preserve Ownership And Deadlines

- Make owner requests trivially copyable, with `OwnerCtrl` as the first member.
  Let `OwnerTask` manage semaphore/state lifecycles.
- Treat a timeout as the caller stopping its wait, not proof that the work has
  stopped. The owner may still process an abandoned request.
- Never store pointers to caller-owned buffers, sinks, or semaphores in request
  payloads. Use bounded inline data such as `data[cfg::kIoChunk]` and copy results
  per chunk. Sink writes and network receives run in the caller task.
- Do not hold an owner mutex while waiting on long I/O or another owner. Use a
  coordinator for resource handoffs; avoid owners waiting on each other.
- Take stack size, priority, core affinity, queue depth, and deadlines from
  `lib/common/src/common/config.h` or the existing owner configuration.
  ESP-IDF FreeRTOS stack sizes here are in bytes, not words.
- Preserve explicit errors such as `Timeout`, `QueueFull`, and `ResourceBusy`.
  Do not replace them with unbounded retries or false success. Keep the
  rate-limited overflow summaries in `OwnerTask::noteOverflow()`.

## Preserve Memory And Peripheral Contracts

- Use existing PSRAM helpers for large objects, JSON, and image staging; check
  allocation failures. Reserve internal DRAM for drivers, sockets, and stacks.
  Do not move large buffers onto the stack or into internal globals without
  assessing the memory cost.
- Read `lib/camera/src/camera/camera_owner.cpp` for the staging lifecycle:
  capture/pin/read/release/discard. Ensure error paths also perform cleanup.
- Preserve SDMMC slot 1, width 1 and GPIO13 handoffs through
  `lib/servo/src/servo/sd_servo_coordinator.cpp`: suspend, HIGH, mount, restore.
  A failed mount currently attempts to restore the servo, then returns an error;
  a failed unmount leaves it suspended/HIGH. Do not make these paths identical.
- Keep storage registration before servos and the mount attempt before pan PWM.
  GPIO12 for tilt still has board strapping constraints. Check
  `docs/05-hardware-pins.md` and the active driver rather than assuming all boards
  behave identically.
- Do not let servo LEDC timers/channels conflict with camera XCLK. Check the
  active settings in both drivers before changing timer or pin allocation.
- In `lib/filelog/`, the entire `captureChar` path that may run in an ISR or with
  flash cache disabled must reside in IRAM and touch only internal DRAM. Do not
  add heap allocation, PSRAM, logging, filesystem access, or FreeRTOS task APIs
  to this path; preserve the existing spinlock try-lock/drop behavior.
- Install log capture after `Serial.begin()`, as in `src/main.cpp`. LittleFS
  contains current/previous boot logs; do not run `uploadfs`.

## Verify

1. Run `bash test/run_host_tests.sh` if pure libraries also changed.
2. Run `pio run -e esp32cam` for firmware changes. Inspect the link result and
   memory-use changes, not just compilation status.
3. Distinguish host/build results from device evidence. When hardware testing
   is approved, use status, logs, and stack watermarks to test affected failure
   paths, timeouts, reboot, and camera/SD/servo interactions.
4. Preserve `monitor_rts = 0` and `monitor_dtr = 0` in `platformio.ini`. Opening
   serial with RTS/DTR asserted can hold the programmer base in reset/download
   mode. Do not infer a device crash from an empty monitor.
5. Report commands, results, and limitations. Do not flash or reboot as part of
   an automatic build step.
