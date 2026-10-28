# ControlMe

**From conversation to physical action.**

ControlMe is firmware for classic AI-Thinker-style ESP32-CAM boards that makes
physical hardware available to external AI agents through MCP (Model Context
Protocol). The AI model and reasoning run outside the board. The agent calls
tools, and the firmware executes them on the device.

```text
external AI agent
       |
       v
  MCP (HTTP :81)
       |
       v
 ControlMe / ESP32-CAM
       |
       +--> JPEG camera
       +--> pan/tilt servos
       +--> microSD
       +--> status, logs, reboot
```

## What You Can Do

- **Camera** - capture JPEG photos and read staged images. Tools return metadata
  and a `downloadUrl`; inline images are included when they fit the attachment
  budget.
- **Pan/tilt servos** - position the camera with angle commands. Pan on GPIO13
  shares SD DAT3, so SD mounting and servo control are coordinated. Tilt on
  GPIO12 depends on board-specific hardware qualification.
- **microSD** - work with files, including chunked writes for larger payloads.
- **Device management** - inspect status, set the clock, reboot, and read logs
  from the current or previous boot.

## Why ControlMe

- The MCP server runs directly on the board, with no separate MCP bridge.
- Hardware access follows the FreeRTOS owner-task pattern, with bounded queues,
  deadlines, and explicit errors.
- PSRAM holds staged images and large buffers, preserving internal memory for
  drivers and communication.

## From Intent To Action

Tell an external agent what you want to do. The agent translates the request
into tool calls on the board.

- "Take a photo with the camera."
- "Pan the camera to the left, then take a picture."
- "Check the previous boot log. Why did the device restart?"

## Hardware

An ESP32-CAM board with an onboard OV2640 camera and microSD slot. The pan servo
uses GPIO13, shared with SD DAT3. The tilt servo uses GPIO12, which has boot
strapping constraints. Verify the specific board setup; two-servo operation is
not guaranteed across all boards.

## Stack

PlatformIO, C++17, Arduino/ESP-IDF, FreeRTOS, MCP.

## Documentation

- [Hardware pin map](docs/05-hardware-pins.md)
- [Interactive board viewer](docs/board-design.html) - an illustrative draft,
  not manufacturing CAD
- [Agent guide](AGENTS.md) - engineering workflows, commands, and active contracts
- [Agent skills](.agents/skills/) - task-specific workflows

## License

ControlMe is licensed under the **GNU General Public License v3.0 only**
(`GPL-3.0-only`). See [LICENCE](LICENCE) for the full license text.
