#pragma once
// Structured events for the rule engine — format per
// docs/03-ai-tools-events.md. Fixed-size POD: safe through FreeRTOS queues,
// no heap allocation.
#include <stdint.h>
#include <string.h>
#include "common/config.h"

namespace controlme {

// Baseline trigger types = software only (no GPIO).
enum class EventKind : uint8_t {
  ScheduleTick = 0,    // SCHEDULE_TICK — clock adapter, wall-clock valid when synced
  WebButtonPressed,    // WEB_BUTTON_PRESSED — UI input via POST /api/v1/events
  DeviceReady,         // DEVICE_READY — produced by firmware after init
  Custom,              // non-trigger event (run/action state notifications
                       // etc.); concrete type carried in the JSON payload
};

const char* eventKindName(EventKind k);                 // "SCHEDULE_TICK" etc.
bool eventKindFromName(const char* s, EventKind& out);  // parse name -> kind

struct Event {
  char id[cfg::kIdCap] = {};
  char bootId[cfg::kBootIdCap] = {};
  char source[cfg::kSourceCap] = {};     // "clock" | "web-console" | "firmware" | ...
  EventKind kind = EventKind::ScheduleTick;
  uint64_t occurredMonoMs = 0;
  uint32_t sequence = 0;
  char correlationId[cfg::kIdCap] = {};
  char causationId[cfg::kIdCap] = {};
  char ancestorRuleIds[cfg::kMaxAncestorRules][cfg::kRuleIdCap] = {};
  uint8_t ancestorRuleCount = 0;
  uint8_t hopCount = 0;
  char payload[cfg::kPayloadCap] = {};
};

// Shared mutation envelope (docs/03): requestId + expectedBootId + isRetry.
struct MutationEnvelope {
  char requestId[cfg::kRequestIdCap] = {};
  char expectedBootId[cfg::kBootIdCap] = {};
  bool isRetry = false;
};

// State machine (docs/03).
enum class RunState : uint8_t {
  Queued, Running, WaitingTool, Completed, Failed, Cancelled, Interrupted,
  Rebooting, Unknown
};
enum class ActionState : uint8_t {
  Accepted, Running, Succeeded, Failed, Cancelled, Unknown
};

const char* runStateName(RunState s);
const char* actionStateName(ActionState s);

inline void copyStr(char* dst, size_t cap, const char* src) {
  if (!src) src = "";
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

}  // namespace controlme
