#pragma once
// McpServer — dispatches one MCP JSON-RPC message to IToolDispatcher.
// Pure (no ESP-IDF/heap): all buffers are caller-owned via McpBuffers; the
// McpServer object itself is psramNew'd by the composition root.
// A response is always one line (no newlines — SSE "data:" frames).
#include <stdint.h>
#include "common/config.h"
#include "ports/actions.h"
#include "llmproto/json_mini.h"
#include "mcpproto/jsonrpc.h"
#include "mcpproto/str_sink.h"

namespace controlme::mcp {

inline constexpr const char* kServerName = "controlme";
inline constexpr const char* kServerVersion = "0.1.0";

// Supported protocol versions (newest first — initialize fallback).
inline constexpr const char* kProtocolVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};
inline constexpr uint8_t kProtocolVersionCount = 3;

struct McpBuffers {
  llmproto::JsonDoc* doc;  // scratch parser (~29 KiB, PSRAM)
  char* args;              // arguments re-serialized for dispatch
  size_t argsCap;
  char* toolOut;           // raw tool JSON result + error scratch
  size_t toolOutCap;
};

class McpServer {
 public:
  McpServer(IToolDispatcher& tools, const McpBuffers& b) : tools_(tools), b_(b) {}

  // Handles ONE JSON-RPC message. `out` is reset first. true = response
  // written to out; false = no response (notification / client response).
  bool handle(const char* body, size_t len, StrSink& out);

 private:
  bool finishOrTooLarge(const RpcId& id, StrSink& out);
  bool emitInitialize(StrSink& out, const RpcRequest& r);
  bool emitToolsList(StrSink& out);
  bool toolCall(const RpcRequest& r, StrSink& out);
  Status findTool(const char* name);  // Ok when present in the wired catalog

  IToolDispatcher& tools_;
  McpBuffers b_;
  // Member scratch (object in PSRAM; not big on the stack).
  char name_[cfg::kToolNameCap];
  char desc_[256];
  char schema_[1536];
};

}  // namespace controlme::mcp
