# ControlMe Agent Guide

## Project Context

ControlMe is ESP32-CAM firmware that provides an MCP server for external AI
agents. The model and reasoning run outside the board; the firmware executes
device tools. The build target is `esp32cam`, using PlatformIO, C++17,
Arduino/ESP-IDF, and FreeRTOS. Keep `README.md` focused on branding; put
engineering instructions in this guide and the local skills.

## Start Here

1. Read the files you will change, the related ports, and the closest existing
   implementation.
2. Load the matching local skills from the table below, not every skill at once.
3. Follow active source code when comments or historical documents disagree;
   explain the conflict before changing a contract or behavior.
4. Keep changes within the user's request. Do not add unnecessary layers,
   dependencies, product policies, or unrelated refactors.
5. Write documentation and skill metadata in English for the global community.

| Task area | Skill |
| --- | --- |
| Firmware, owner tasks, memory, camera, SD, servos, logs, reboot | [controlme-firmware](.agents/skills/controlme-firmware/SKILL.md) |
| Tool catalog, JSON-RPC, MCP, HTTP/SSE, attachments | [controlme-mcp-tools](.agents/skills/controlme-mcp-tools/SKILL.md) |
| Hardware diagrams, three.js viewer, components and pins | [controlme-board-viewer](.agents/skills/controlme-board-viewer/SKILL.md) |

## Code Map And Layer Boundaries

| Location | Responsibility |
| --- | --- |
| `src/main.cpp`, `src/app.h`, `src/mod_*.cpp` | Composition root, dependency injection, startup, module and route registration |
| `src/core_routes.cpp` | REST status and capabilities |
| `src/owner_task.h` | Queues, deadlines, and cross-task request lifecycles |
| `lib/ports/src/ports/` | Device, clock, sink, dispatcher, and lifecycle interfaces |
| `lib/actions/src/actions/device_actions.cpp` | Application tool handlers and the `kTools[]` catalog |
| `lib/domain/` | Pure domain logic, currently the dedup cache |
| `lib/common/`, `lib/llmproto/`, `lib/mcpproto/` | Shared types, JSON parsing, and host-tested MCP protocol code |
| `lib/camera/`, `lib/storage/`, `lib/servo/`, `lib/filelog/`, `lib/supervisor/` | Device adapters and resource owners |
| `lib/transport/` | `esp_http_server` adapters for REST and MCP |
| `docs/board-*.js`, `docs/board-*.css`, `docs/board-design.html` | Hardware viewer, separate from the firmware |
| `test/host_*/` | Native C++ tests for pure libraries |

Keep pure business rules in the domain layer, tool orchestration in the
application layer, and hardware/network access in adapters. Inject dependencies
through ports and `App` wiring instead of constructing drivers in business logic.
Do not add pass-through layers. Pure libraries must remain buildable without
Arduino/ESP-IDF.

## Working Commands

Run these commands from the repository root. Requirements: PlatformIO for
firmware, `g++` with ASan/UBSan for host tests, Node.js for viewer syntax checks,
and Python 3 for serving local documentation. This repository has no Makefile
or npm build script.

| Purpose | Command |
| --- | --- |
| Build firmware | `pio run -e esp32cam` |
| Test pure libraries | `bash test/run_host_tests.sh` |
| Check viewer syntax | `node --check docs/board-components.js && node --check docs/board-model.js && node --check docs/board-viewer.js` |
| Serve local documentation | `python3 -m http.server 8000 --bind 127.0.0.1 --directory docs` |
| Monitor serial when needed | `pio device monitor -e esp32cam` |
| Upload firmware, only when requested | `pio run -e esp32cam -t upload` |

If `pio` is not on PATH, check the local installation; on this machine the
executable is at `~/.platformio/penv/bin/pio`. Do not assume this path is portable.
Host tests invoke the compiler directly, with one executable per `.cpp` file in
`test/host_*`, not `pio test`. `env:native` is not an ESP32 application build.
Open the viewer at `http://127.0.0.1:8000/board-design.html`. It loads three.js
from a CDN through an import map, not an npm dependency.

## Configuration And Active Contracts

- Wi-Fi: copy `include/secret.h.example` to `include/secret.h` only if the local
  file does not exist, then fill in the local configuration. Do not overwrite
  an existing file. `include/secret.h` is already gitignored. The `partner` NVS
  namespace with `wifi_ssid`/`wifi_pass` keys takes precedence over macros; do
  not rename the namespace without an explicit migration requirement.
- REST runs on port 80. MCP runs on port 81: `POST /mcp` for stateless Streamable
  HTTP; `GET /sse` and `POST /messages?session_id=...` for the legacy transport.
- Tool sources of truth are `kTools[]`, `toolWired()`, and `tools/list`; do not
  hardcode a tool count from old comments. `web_search`/`web_fetch` are only
  advertised when their adapters are installed.
- Capabilities come from module registration, not a wishlist. `App::addFeature`
  stores a pointer, so JSON fragments must have static lifetime.
- LittleFS stores runtime logs, not a UI bundle. There is no need for `uploadfs`;
  do not format the log filesystem or combine `upload`/`uploadfs` targets.
- Register storage before servos; the coordinator captures `app.storage` at
  construction. Coordinated SD mounting happens before pan PWM is enabled.
- The [hardware pin map](docs/05-hardware-pins.md) still says GPIO13 stays HIGH
  after a failed mount. The active `SdServoCoordinator::doMount()` implementation
  attempts to restore the servo and returns the mount error. Do not change
  recovery behavior just to match the historical document.

## Verification And Reporting

Select checks for the changed area. Run host tests for pure-library changes;
build the firmware for firmware, port, or wiring changes. Host tests do not
prove driver behavior, timing, FreeRTOS concurrency, or hardware operation.
For viewer changes, run syntax checks and inspect desktop and mobile
screenshots; see the viewer skill for interaction scenarios.

For documentation-only changes, check relative paths/links, command examples
against actual configuration, skill frontmatter, and `git diff --check`. Do not
report builds, tests, or device checks that were not run. Flashing, rebooting,
deleting files, and changing device state are not automatic validation steps;
confirm these actions with the user.

Review contracts and behavior, layering and dependency injection, ownership and
race/deadlock risks, memory use, readability, and error handling/logging. Include
visual results for UI changes. Report passing checks and verification limits.
