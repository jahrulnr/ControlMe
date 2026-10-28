#pragma once
// Run queue port — one active AI run, a shared FIFO for chat and
// automation (docs/02, docs/03). RuleEngine -> submit(); AgentRunner
// consumes.
#include "common/error.h"
#include "common/event.h"
#include "common/config.h"

namespace controlme {

enum class RunSource : uint8_t { Chat = 0, RuleEvent, Manual };

struct RunRequest {
  char sessionId[cfg::kIdCap] = {};          // empty = sessionless run
  char instruction[1024] = {};
  char imageIds[cfg::kMaxRunAttachments][cfg::kIdCap] = {};
  uint8_t imageCount = 0;

  RunSource source = RunSource::Chat;
  // Immutable snapshot (docs/03): a run's execution input doesn't change
  // when the rule is edited after enqueue.
  char ruleId[cfg::kRuleIdCap] = {};
  uint64_t ruleRevision = 0;
  uint64_t settingsRevision = 0;
  char matchedEventIds[cfg::kMaxTriggersPerRule][cfg::kIdCap] = {};
  uint8_t matchedCount = 0;
  char ancestorRuleIds[cfg::kMaxAncestorRules][cfg::kRuleIdCap] = {};
  uint8_t ancestorCount = 0;
  uint8_t hopCount = 0;

  MutationEnvelope env = {};
};

class IRunQueue {
 public:
  virtual ~IRunQueue() = default;
  virtual Status submit(const RunRequest& req, char runIdOut[cfg::kIdCap]) = 0;
  virtual Status cancel(const char* runId) = 0;   // intent, not undo
  virtual Status cancelAll() = 0;                  // used by the reboot supervisor
  virtual bool runActive() = 0;
};

}  // namespace controlme
