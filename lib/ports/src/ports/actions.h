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

// Optional binary attachment for one tool call (e.g. image_read JPEG).
// `data` is a caller-owned sink the callee streams the BASE64 payload into
// (no escaping needed downstream); `dataCap` = its writable capacity.
// The callee fills mime/uri/name and sets hasData only when the payload was
// written in full — a partial write must leave hasData=false (uri/name/mime
// may still be set so the caller can emit a link instead).
struct ToolAttach {
  IByteSink* data = nullptr;
  size_t dataCap = 0;
  char mime[24] = {};
  char uri[192] = {};
  char name[cfg::kIdCap] = {};
  bool hasData = false;
};

class IToolDispatcher {
 public:
  virtual ~IToolDispatcher() = default;
  // Execute one tool. Result = raw JSON written to the sink (paginated/
  // bounded — as-is, not hidden). Blocking in the caller's task with an
  // internal deadline per tool category. `attach` is optional; nullptr =
  // the caller doesn't render attachments (tools then only carry URLs in
  // their JSON result).
  virtual Status dispatch(const char* toolName, const char* argsJson,
                          const ToolCallCtx& ctx, IByteSink& resultJson,
                          ToolAttach* attach = nullptr) = 0;
  // Tool catalog for advertising to the provider (name + description + jsonSchema).
  virtual uint8_t toolCount() = 0;
  virtual bool toolSpec(uint8_t idx, char* nameOut, size_t nameCap,
                        char* descOut, size_t descCap, char* schemaOut,
                        size_t schemaCap) = 0;
};

}  // namespace controlme
