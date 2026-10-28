#pragma once
// Tool dispatch port — DeviceActions (application) executes AI tools and
// manual actions through this facade. Called ON the caller's task
// (agent/httpd); implementations internally use owner facades
// (queue+deadline) — not locks.
#include "common/error.h"
#include "common/config.h"
#include "ports/writer.h"

namespace controlme {

struct ToolCallCtx {
  char runId[cfg::kIdCap] = {};      // empty = direct manual action (UI/API)
  char actionId[cfg::kIdCap] = {};
  char requestId[cfg::kRequestIdCap] = {};
};

class IToolDispatcher {
 public:
  virtual ~IToolDispatcher() = default;
  // Execute one tool. Result = raw JSON written to the sink (paginated/
  // bounded — as-is, not hidden). Blocking in the caller's task with an
  // internal deadline per tool category.
  virtual Status dispatch(const char* toolName, const char* argsJson,
                          const ToolCallCtx& ctx, IByteSink& resultJson) = 0;
  // Tool catalog for advertising to the provider (name + description + jsonSchema).
  virtual uint8_t toolCount() = 0;
  virtual bool toolSpec(uint8_t idx, char* nameOut, size_t nameCap,
                        char* descOut, size_t descCap, char* schemaOut,
                        size_t schemaCap) = 0;
};

}  // namespace controlme
