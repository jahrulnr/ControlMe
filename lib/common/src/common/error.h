#pragma once
// Shared error taxonomy across layers — aligned with the
// docs/07-api-contracts.md catalog. Adapters map esp_err_t to Err at the
// adapter boundary; errors are carried as explicit return values (see
// docs/04-code-principles.md).
#include <stdint.h>
#include "common/log.h"   // ESP_LOGx -> Arduino log_x (IDF INFO compiled-out)

namespace controlme {

enum class Err : uint8_t {
  None = 0,
  InvalidArgument,    // 422 bad argument/schema; run-now on a disabled rule
  NotSupported,       // 422 feature not supported (fetch needs JS; unconfigured servo channel; mime)
  NotFound,           // 404
  FileBusy,           // 409 rename/delete while a handle/lease is active
  RevisionConflict,   // 409 stale expectedRevision; chunk offset with different payload
  ResourceBusy,       // 409 image pool full; write lease in use; servo handoff; camera busy
  UnknownExecution,   // 409 foreign expectedBootId; isRetry cache miss; past outcome unproven
  ImageTooLarge,      // 413
  QueueFull,          // 429 queue full / dedup cache full of active entries
  DeviceRebooting,    // 503
  SdUnavailable,      // 503
  ProviderUnavailable,// 503
  Timeout,            // 504
  ProtocolIncomplete, // 502 provider stream cut / call conflict before terminal
  Internal            // 500
};

struct Status {
  Err code = Err::None;
  const char* detail = nullptr;  // string literal / static storage only
  bool ok() const { return code == Err::None; }
  explicit operator bool() const { return ok(); }
  static Status Ok() { return {}; }
  static Status Fail(Err c, const char* d = nullptr) { return {c, d}; }
};

const char* errName(Err e);          // "REVISION_CONFLICT" etc (for the API catalog)
// Inverse of errName: catalog name -> Err. false on unknown name.
// Used by dedup replay ("failed:<NAME>") so the original error code replays.
bool errFromName(const char* name, Err& out);
int errHttpStatus(Err e);            // mapping to HTTP status
const char* errTitle(Err e);         // problem+json title

}  // namespace controlme
