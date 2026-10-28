#pragma once
// App — singleton composition root (wired in main.cpp setup()).
// Modules register HTTP routes + capabilities via functions
// controlme_register_<modul>(App&) — called from setup() only.
// Convention example:
//   void controlme_register_storage(App& app) {
//     app.http.addRoute(HTTP_GET, "/api/v1/files", &handleList, &app);
//     app.addFeature("{\"tools\":[\"sd_list\",\"sd_read\"]}");
//   }
#include <stdint.h>
#include "common/config.h"
#include "common/error.h"
#include "ports/clock.h"
#include "ports/event_sink.h"
#include "ports/camera.h"
#include "ports/storage.h"
#include "ports/servo.h"
#include "ports/coordinator.h"
#include "ports/actions.h"
#include "ports/runner.h"
#include "ports/reboot.h"
#include "ports/devlog.h"
#include "ports/web.h"
#include "transport/http_server.h"
#include "transport/mcp_transport.h"

namespace controlme {

struct App {
  IClock* clock = nullptr;        // adapter in main.cpp
  IEventSink* events = nullptr;   // sink assigning sequence + SSE fan-out

  // Device module slots — filled by each module's
  // controlme_register_<modul>() (src/mod_*.cpp); nullptr = not installed ->
  // endpoint fails honestly.
  ICamera* camera = nullptr;
  IStorage* storage = nullptr;
  IServo* servo = nullptr;
  ISdServoCoordinator* coord = nullptr;  // SD<->GPIO13 handoff orchestration
  IToolDispatcher* tools = nullptr;      // DeviceActions (13 tools via MCP)
  IRunQueue* runs = nullptr;             // kept — reboot_supervisor calls cancelAll() when installed
  IReboot* rebootCtl = nullptr;          // reboot supervisor
  IWebSearch* webSearch = nullptr;
  IWebFetch* webFetch = nullptr;
  IDeviceLog* logs = nullptr;            // DeviceLog (filelog) — persistent LittleFS log
  volatile bool admissionOpen = true;    // false once shutdown/reboot starts

  transport::HttpServer http;     // :80 — REST status/capabilities/logs
  transport::McpTransport mcp;    // :81 — MCP (streamable HTTP + legacy SSE)
  char httpBaseUrl[32] = {};      // "http://<ip>" after wifi connect; empty offline

  char bootId[cfg::kBootIdCap] = {};
  // Published-state watermark (docs/07): updated by the sink in a short
  // critical section; read atomically (32-bit) by the status handler.
  volatile uint32_t lastEventSequence = 0;

  bool littleFsMounted = false;   // used by DeviceLog; failed mount -> logs nullptr
  bool wifiConnected = false;     // SD status read live: storage->isMounted()

  // Capabilities: only features ACTUALLY registered (docs/07 — honest,
  // not a wishlist). Fragment = raw JSON object with static storage
  // (a literal); the module owns it, App only stores the pointer.
  static constexpr uint8_t kMaxFeatures = 16;
  const char* features[kMaxFeatures] = {};
  uint8_t featureCount = 0;
  bool addFeature(const char* jsonFragment);
};

}  // namespace controlme

// Core routes — registered from setup(): GET /api/v1/status,
// /capabilities, then the static GET /* fallback (always last).
void controlme_register_core(controlme::App& app);
