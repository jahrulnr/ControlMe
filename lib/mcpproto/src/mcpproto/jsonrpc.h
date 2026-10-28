#pragma once
// JSON-RPC 2.0 on top of json_mini — parses client
// request/notification/response messages and writes result/error/id heads.
// No heap; JsonDoc owned by the caller.
#include <stdint.h>
#include "common/config.h"
#include "llmproto/json_mini.h"
#include "mcpproto/str_sink.h"

namespace controlme::mcp {

struct RpcId {
  enum class Kind : uint8_t { None, Null, Number, String } kind = Kind::None;
  char str[cfg::kRequestIdCap] = {};
  double num = 0;
};

struct RpcRequest {
  RpcId id;
  bool notification = false;    // no id -> no response
  bool clientResponse = false;  // result/error message from the client -> ignore
  const char* method = nullptr; // points into the JsonDoc pool (valid until the next parse)
  uint32_t methodLen = 0;
  const llmproto::JsonValue* params = nullptr;
};

enum class RpcParse : uint8_t { Ok, ParseError, InvalidRequest };

// Parse one message. doc is used as the scratch pool (reset per parse).
// Malformed -> ParseError; other JsonErr (size/depth/pool) -> InvalidRequest.
// On InvalidRequest, out.id still carries the id when one was parsed.
RpcParse parseRpc(llmproto::JsonDoc& doc, const char* body, size_t len, RpcRequest& out);

// {"jsonrpc":"2.0","id":<id>,"result":  — payload + "}" follows.
bool writeRpcResultHead(StrSink& out, const RpcId& id);
// Whole error object: {"jsonrpc":"2.0","id":<id>,"error":{"code":X,"message":"M"}}
bool writeRpcError(StrSink& out, const RpcId& id, int code, const char* message);
// None/Null -> null; Number -> emit rules; String -> quoted+escaped.
bool writeRpcId(StrSink& out, const RpcId& id);

bool methodIs(const RpcRequest& r, const char* m);

}  // namespace controlme::mcp
