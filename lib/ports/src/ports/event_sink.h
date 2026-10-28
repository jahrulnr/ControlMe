#pragma once
// Event sink: the rule engine / UI SSE consumes published events.
// Implementations may only enqueue (nonblocking) — heavy work belongs in the
// consumer task.
#include "common/event.h"

namespace controlme {

class IEventSink {
 public:
  virtual ~IEventSink() = default;
  // Called from the caller's context; implementations enqueue + return fast.
  // Full -> return Err::QueueFull (caller records an overflow counter).
  virtual Status publish(const Event& ev) = 0;
};

}  // namespace controlme
