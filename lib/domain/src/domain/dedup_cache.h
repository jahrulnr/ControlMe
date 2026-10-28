#pragma once
// DedupCache for requestIds per boot (docs/03 "Mutation Request
// Envelope"). 32-entry capacity; ACTIVE entries are pinned (cannot be
// evicted); oldest TERMINAL entries evicted first; a cache full of active
// entries -> Full (caller maps to QUEUE_FULL). Purely fixed-size, no heap.
#include <stdint.h>
#include <stddef.h>
#include "common/config.h"
#include "common/error.h"

namespace controlme {

// Small opaque outcome stored per requestId (e.g. actionId+state summary).
inline constexpr size_t kDedupOutcomeCap = 80;

class DedupCache {
 public:
  static constexpr size_t kCapacity = cfg::kDedupCacheEntries;  // 32

  enum class AdmitResult : uint8_t {
    New,               // new ACTIVE entry created and pinned
    Duplicate,         // requestId within retention; outcomeOut filled
                       // with the terminal outcome, or EMPTY if the entry is
                       // still ACTIVE
    Full,              // full of active entries -> QUEUE_FULL
    UnknownExecution,  // isRetry + cache miss -> never re-executed (no
                       // entry made); also for empty requestIds
  };

  // Admit a requestId per the docs/03 envelope.
  // outcomeOut may be nullptr; when != nullptr it's always NUL-ed first.
  AdmitResult admit(const char* requestId, bool isRetry,
                    char* outcomeOut, size_t outcomeCap);

  // ACTIVE -> TERMINAL + store the opaque outcome.
  // NotFound when the requestId is absent or already TERMINAL.
  Status complete(const char* requestId, const char* outcome);

  // Fresh boot: all entries dropped.
  void reset();

  // Diagnostics/tests.
  uint8_t activeCount() const;
  uint8_t terminalCount() const;
  bool contains(const char* requestId) const;

 private:
  enum EntryState : uint8_t { Empty = 0, Active, Terminal };
  struct Entry {
    char requestId[cfg::kRequestIdCap] = {};
    char outcome[kDedupOutcomeCap] = {};
    uint8_t state = Empty;
    uint32_t stamp = 0;  // last transition sequence number (for oldest eviction)
  };
  int find(const char* requestId) const;
  int evictable() const;  // Empty first, then oldest Terminal; -1 = all ACTIVE

  Entry entries_[kCapacity] = {};
  uint32_t stamp_ = 0;
};

}  // namespace controlme
