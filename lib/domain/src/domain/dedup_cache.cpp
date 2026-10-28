#include "domain/dedup_cache.h"

#include <string.h>
#include "common/event.h"  // copyStr

namespace controlme {

int DedupCache::find(const char* requestId) const {
  for (size_t i = 0; i < kCapacity; ++i) {
    if (entries_[i].state != Empty &&
        strcmp(entries_[i].requestId, requestId) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int DedupCache::evictable() const {
  int oldest = -1;
  for (size_t i = 0; i < kCapacity; ++i) {
    if (entries_[i].state == Empty) return static_cast<int>(i);  // free slot
    if (entries_[i].state == Terminal &&
        (oldest < 0 || entries_[i].stamp < entries_[oldest].stamp)) {
      oldest = static_cast<int>(i);  // oldest terminal = eviction candidate
    }
  }
  return oldest;  // -1 when all are ACTIVE (pinned)
}

DedupCache::AdmitResult DedupCache::admit(const char* requestId, bool isRetry,
                                        char* outcomeOut, size_t outcomeCap) {
  if (outcomeOut && outcomeCap > 0) outcomeOut[0] = '\0';
  if (!requestId || requestId[0] == '\0') {
    return AdmitResult::UnknownExecution;
  }
  const int idx = find(requestId);
  if (idx >= 0) {
    const Entry& e = entries_[idx];
    if (e.state == Terminal && outcomeOut && outcomeCap > 0) {
      copyStr(outcomeOut, outcomeCap, e.outcome);
    }
    // ACTIVE: outcomeOut stays empty — entry still running.
    return AdmitResult::Duplicate;
  }
  if (isRetry) {
    return AdmitResult::UnknownExecution;  // miss + retry: no entry made
  }
  const int slot = evictable();
  if (slot < 0) return AdmitResult::Full;  // full of active entries
  Entry& e = entries_[slot];
  copyStr(e.requestId, sizeof(e.requestId), requestId);
  e.outcome[0] = '\0';
  e.state = Active;
  e.stamp = ++stamp_;
  return AdmitResult::New;
}

Status DedupCache::complete(const char* requestId, const char* outcome) {
  const int idx = requestId ? find(requestId) : -1;
  if (idx < 0 || entries_[idx].state != Active) {
    return Status::Fail(Err::NotFound, "requestId");
  }
  Entry& e = entries_[idx];
  copyStr(e.outcome, sizeof(e.outcome), outcome);
  e.state = Terminal;
  e.stamp = ++stamp_;
  return Status::Ok();
}

void DedupCache::reset() {
  for (Entry& e : entries_) e = Entry{};
  stamp_ = 0;
}

uint8_t DedupCache::activeCount() const {
  uint8_t n = 0;
  for (const Entry& e : entries_) {
    if (e.state == Active) ++n;
  }
  return n;
}

uint8_t DedupCache::terminalCount() const {
  uint8_t n = 0;
  for (const Entry& e : entries_) {
    if (e.state == Terminal) ++n;
  }
  return n;
}

bool DedupCache::contains(const char* requestId) const {
  return requestId && find(requestId) >= 0;
}

}  // namespace controlme
