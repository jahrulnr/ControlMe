#pragma once
// DeviceActions — dispatcher for the 14 tools (docs/03 table) on top of the
// App ports. Called blocking in the caller's task (ActionService /
// AgentRunner worker); all device access goes through the owner facades
// (queue+deadline), not locks — per docs/04 (one resource one owner, no
// bypass).
//
// Tool result = compact JSON <= ~kToolResultCap bytes so it fits the
// ActionService record (outcome <=1 KiB, docs/03 "paginated results"). Lists
// (sd_list/web_search) are emitted per element and honestly truncated via a
// "truncated" flag + internal nextCursor when the budget runs out; binary
// (sd_read) is base64-streamed per chunk — not a whole copy in memory.
// No outcome is hidden or softened (docs/04): port errors are passed through
// as-is as shared-taxonomy Err.
#include "common/config.h"
#include "common/error.h"
#include "ports/actions.h"

namespace controlme {

struct App;  // src/app.h — full definition only needed in the .cpp

class DeviceActions : public IToolDispatcher {
 public:
  explicit DeviceActions(App& app) : app_(app) {}

  Status dispatch(const char* toolName, const char* argsJson,
                  const ToolCallCtx& ctx, IByteSink& resultJson,
                  ToolAttach* attach = nullptr) override;
  uint8_t toolCount() override;
  bool toolSpec(uint8_t idx, char* nameOut, size_t nameCap, char* descOut,
                size_t descCap, char* schemaOut, size_t schemaCap) override;

 private:
  App& app_;
};

}  // namespace controlme
