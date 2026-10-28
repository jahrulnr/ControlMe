---
name: controlme-mcp-tools
description: Modify ControlMe device tool contracts and its on-board MCP server. Use for tools/list, tools/call, the kTools catalog, JSON-RPC, Streamable HTTP /mcp, legacy SSE /sse and /messages, protocol errors, image attachments, or transport socket and buffer limits.
---

# ControlMe MCP Tools

## Follow The Existing Flow

Read the relevant change boundaries, not the entire firmware:

1. `lib/ports/src/ports/actions.h`: `IToolDispatcher`, `ToolCallCtx`, `ToolAttach`.
2. `lib/actions/src/actions/device_actions.cpp`: handlers, schemas, `kTools[]`,
   `toolWired()`, and `DeviceActions::dispatch()`.
3. `lib/mcpproto/src/mcpproto/`: JSON-RPC parsing/emission and the pure `McpServer`.
4. `lib/transport/src/transport/mcp_transport.*`: HTTP adapter and SSE lifecycles.
5. `src/mod_mcp.cpp` and `lib/common/src/common/config.h`: wiring, buffers, budgets.

Keep the MCP parser/dispatcher free of Arduino/ESP-IDF. Execute tools through
device ports; do not move driver access into protocol or transport handlers.

## Update Tools Consistently

- Change each handler and its schema in `kTools[]` together. Match required
  fields, types, ranges, pagination, and results to existing runtime validation.
- Preserve `toolWired()` for optional adapters. Treat `tools/list` as the
  advertised contract; do not rely on tool counts in historical comments.
- Write result JSON through a bounded `IByteSink`. Preserve `Status` errors and
  reboot admission behavior. Do not claim global deduplication just because
  `ToolCallCtx` carries a requestId; the dispatcher does not currently use it
  to deduplicate every tool.
- Keep error categories separate: JSON-RPC parse/request/method/params errors
  use protocol error envelopes; tool execution failures use `isError: true`
  with code details in text content. Do not swap them without a requirement.
- Preserve image metadata/downloadUrl and `ToolAttach`: inline base64 is only
  valid when the complete payload fits. A `resource_link` can still be sent
  when the image does not fit inline. Do not report partial attachments as
  successful image payloads.
- For log changes, preserve offset/nextOffset pagination and distinguish current
  from previous boot logs. Use the `IDeviceLog` port, not direct LittleFS access
  from the protocol layer.

## Preserve Both Transports

- Keep stateless `POST :81/mcp` and legacy `GET :81/sse` followed by
  `POST :81/messages?session_id=...`. REST uses a separate server on :80.
- Reserve async budget before `httpd_req_async_handler_begin()` and release it
  on every completion/error path. Async-held requests must always leave one
  non-async slot: `cfg::kMcpAsyncMax = cfg::kMcpMaxOpenSockets - 1`.
- Keep dispatch workers separate from keepalive/reap tasks. Slow tools must not
  monopolize keepalive handling or the session registry.
- Do not hold the registry mutex while sending to a socket. Preserve per-session
  send/close serialization and session identity when slots are reused. Audit
  cleanup for disconnects, full queues, timeouts, and shutdown.
- A custom `httpd` close callback must close the socket itself. If
  `global_user_ctx` points to a static/member object, use a no-op free callback
  so `httpd_stop()` does not free that object.
- Body, output, attachment, queue, and socket limits come from `config.h`. Do not
  raise one cap without checking escaping/envelope costs, PSRAM, internal DRAM,
  and the socket limits of the SDK in use.

## Verify The Changed Boundary

- Run `bash test/run_host_tests.sh` for changes to parsing, JSON emission,
  protocol dispatch, or related pure libraries. Use the fake `IToolDispatcher`
  in `test/host_mcpproto/test_mcp_dispatch.cpp`; test success, invalid params,
  tool errors, escaping/overflow, and attachments where affected.
- Run `pio run -e esp32cam` for application tool, port, wiring, or transport
  changes. Host tests do not execute cJSON DeviceActions or httpd.
- When device testing is approved, check `tools/list` and `tools/call` on both
  transports. For concurrency changes, test open SSE connections alongside
  parallel POSTs and overload errors without watchdog resets. For images,
  compare JPEG attachment bytes with the HTTP download when inline is available.
- Do not run mutation, reboot, delete, or servo movement tools without user
  confirmation. Report which parts were only tested through host tests/builds.
