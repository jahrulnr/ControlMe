#include "common/event.h"

namespace controlme {

const char* eventKindName(EventKind k) {
  switch (k) {
    case EventKind::ScheduleTick: return "SCHEDULE_TICK";
    case EventKind::WebButtonPressed: return "WEB_BUTTON_PRESSED";
    case EventKind::DeviceReady: return "DEVICE_READY";
    case EventKind::Custom: return "CUSTOM";
  }
  return "SCHEDULE_TICK";
}

bool eventKindFromName(const char* s, EventKind& out) {
  if (!s) return false;
  if (strcmp(s, "SCHEDULE_TICK") == 0) { out = EventKind::ScheduleTick; return true; }
  if (strcmp(s, "WEB_BUTTON_PRESSED") == 0) { out = EventKind::WebButtonPressed; return true; }
  if (strcmp(s, "DEVICE_READY") == 0) { out = EventKind::DeviceReady; return true; }
  return false;
}

const char* runStateName(RunState s) {
  switch (s) {
    case RunState::Queued: return "QUEUED";
    case RunState::Running: return "RUNNING";
    case RunState::WaitingTool: return "WAITING_TOOL";
    case RunState::Completed: return "COMPLETED";
    case RunState::Failed: return "FAILED";
    case RunState::Cancelled: return "CANCELLED";
    case RunState::Interrupted: return "INTERRUPTED";
    case RunState::Rebooting: return "REBOOTING";
    case RunState::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* actionStateName(ActionState s) {
  switch (s) {
    case ActionState::Accepted: return "ACCEPTED";
    case ActionState::Running: return "RUNNING";
    case ActionState::Succeeded: return "SUCCEEDED";
    case ActionState::Failed: return "FAILED";
    case ActionState::Cancelled: return "CANCELLED";
    case ActionState::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

}  // namespace controlme
