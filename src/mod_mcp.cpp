// MCP module — instantiates DeviceActions (13-tool catalog) + McpServer
// (mcpproto, pure) + McpTransport :81. All large objects in PSRAM (psramNew/
// psramArr): the ~29 KiB JsonDoc and the response buffer must not go to
// internal.
#include <esp_log.h>
#include "app.h"
#include "actions/device_actions.h"
#include "common/config.h"
#include "common/psram_alloc.h"
#include "llmproto/json_mini.h"
#include "mcpproto/mcp_dispatch.h"

namespace {

const char* kTag = "mcpmod";

}  // namespace

void controlme_register_mcp(controlme::App& app) {
  auto* tools = controlme::psramNew<controlme::DeviceActions>(app);
  if (!tools) {
    ESP_LOGE(kTag, "DeviceActions alloc failed — MCP off");
    return;
  }
  app.tools = tools;

  auto* doc = controlme::psramNew<controlme::llmproto::JsonDoc>();
  char* args = static_cast<char*>(
      controlme::psramMalloc(controlme::cfg::kToolArgsMax));
  char* toolOut = static_cast<char*>(
      controlme::psramMalloc(controlme::cfg::kMcpToolResultCap));
  char* attach = static_cast<char*>(
      controlme::psramMalloc(controlme::cfg::kMcpAttachCap));
  if (!doc || !args || !toolOut || !attach) {
    ESP_LOGE(kTag, "MCP buffer alloc failed — MCP off");
    controlme::psramFree(args);
    controlme::psramFree(toolOut);
    controlme::psramFree(attach);
    controlme::psramDelete(doc);
    return;
  }
  controlme::mcp::McpBuffers bufs{doc, args, controlme::cfg::kToolArgsMax,
                                  toolOut, controlme::cfg::kMcpToolResultCap,
                                  attach, controlme::cfg::kMcpAttachCap};
  auto* server = controlme::psramNew<controlme::mcp::McpServer>(*tools, bufs);
  if (!server) {
    ESP_LOGE(kTag, "McpServer alloc failed — MCP off");
    controlme::psramFree(args);
    controlme::psramFree(toolOut);
    controlme::psramFree(attach);
    controlme::psramDelete(doc);
    return;
  }
  controlme::Status st = app.mcp.begin(controlme::cfg::kMcpPort, *server);
  if (!st.ok()) {
    ESP_LOGE(kTag, "mcp :%u failed: %s", (unsigned)controlme::cfg::kMcpPort,
             st.detail ? st.detail : "?");
    return;
  }
  // Fragment must be static storage — addFeature stores the pointer.
  static char s_feature[128];
  snprintf(s_feature, sizeof(s_feature),
           "{\"mcp\":{\"port\":%u,\"endpoints\":{\"streamableHttp\":"
           "\"/mcp\",\"sse\":\"/sse\"}}}",
           (unsigned)controlme::cfg::kMcpPort);
  if (!app.addFeature(s_feature)) {
    ESP_LOGW(kTag, "mcp feature fragment add failed");
  }
}
