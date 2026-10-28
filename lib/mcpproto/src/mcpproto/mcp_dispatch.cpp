#include "mcpproto/mcp_dispatch.h"
#include "mcpproto/json_emit.h"
#include <string.h>

namespace controlme::mcp {

using llmproto::JsonType;
using llmproto::JsonValue;
using llmproto::jsonMember;

namespace {

// ctx.requestId from RpcId: strings copied, numbers formatted per emit
// rules, Null/None -> empty.
void idToRequestId(const RpcId& id, char* out, size_t cap) {
  out[0] = '\0';
  if (id.kind == RpcId::Kind::String) {
    strncpy(out, id.str, cap - 1);
  } else if (id.kind == RpcId::Kind::Number) {
    StrSink s(out, cap);
    JsonValue v{};
    v.type = JsonType::Number;
    v.num = id.num;
    emitJson(&v, s);
  }
}

}  // namespace

// Close the result payload with "}"; overflow anywhere -> reset + -32603.
bool McpServer::finishOrTooLarge(const RpcId& id, StrSink& out) {
  if (!out.overflow()) return true;
  out.reset();
  writeRpcError(out, id, -32603, "response too large");
  return true;
}

bool McpServer::emitInitialize(StrSink& out, const RpcRequest& r) {
  const char* ver = kProtocolVersions[0];
  if (r.params && r.params->type == JsonType::Object) {
    const JsonValue* pv = jsonMember(r.params, "protocolVersion");
    if (pv && pv->type == JsonType::String) {
      for (uint8_t i = 0; i < kProtocolVersionCount; ++i) {
        if (pv->strLen == strlen(kProtocolVersions[i]) &&
            memcmp(pv->str, kProtocolVersions[i], pv->strLen) == 0) {
          ver = kProtocolVersions[i];
          break;
        }
      }
    }
  }
  return out.append("{\"protocolVersion\":\"") && out.append(ver) &&
         out.append("\",\"capabilities\":{\"tools\":{\"listChanged\":false}},"
                    "\"serverInfo\":{\"name\":\"") &&
         out.append(kServerName) && out.append("\",\"version\":\"") &&
         out.append(kServerVersion) && out.append("\"}}");
}

bool McpServer::emitToolsList(StrSink& out) {
  if (!out.append("{\"tools\":[")) return false;
  const uint8_t n = tools_.toolCount();
  for (uint8_t i = 0; i < n; ++i) {
    if (!tools_.toolSpec(i, name_, sizeof(name_), desc_, sizeof(desc_), schema_,
                         sizeof(schema_))) {
      return false;  // caller -> -32603
    }
    if (i > 0 && !out.append(",")) return false;
    if (!out.append("{\"name\":") ||
        !(out.append("\"") && emitEscaped(name_, strlen(name_), out) && out.append("\"")) ||
        !out.append(",\"description\":") ||
        !(out.append("\"") && emitEscaped(desc_, strlen(desc_), out) && out.append("\"")) ||
        !out.append(",\"inputSchema\":") || !out.append(schema_) ||
        !out.append("}")) {
      return false;
    }
  }
  return out.append("]}");
}

Status McpServer::findTool(const char* name) {
  const uint8_t n = tools_.toolCount();
  for (uint8_t i = 0; i < n; ++i) {
    if (!tools_.toolSpec(i, name_, sizeof(name_), desc_, sizeof(desc_), schema_,
                         sizeof(schema_))) {
      return Status::Fail(Err::Internal, "toolSpec failed");
    }
    if (strcmp(name_, name) == 0) return Status::Ok();
  }
  return Status::Fail(Err::NotFound);
}

bool McpServer::toolCall(const RpcRequest& r, StrSink& out) {
  const RpcId& id = r.id;
  if (!r.params || r.params->type != JsonType::Object) {
    writeRpcError(out, id, -32602, "invalid params");
    return finishOrTooLarge(id, out);
  }
  const JsonValue* name = jsonMember(r.params, "name");
  if (!name || name->type != JsonType::String || name->strLen >= sizeof(name_)) {
    writeRpcError(out, id, -32602, "invalid params");
    return finishOrTooLarge(id, out);
  }
  char toolName[cfg::kToolNameCap];
  memcpy(toolName, name->str, name->strLen);
  toolName[name->strLen] = '\0';

  // The wired catalog is the source of truth — DeviceActions returns
  // InvalidArgument for foreign tools, indistinguishable from bad args.
  Status known = findTool(toolName);
  if (!known.ok()) {
    if (known.code == Err::Internal) {
      writeRpcError(out, id, -32603, "Internal error");
    } else {
      // raw "Unknown tool: <name>" — writeRpcError does the escaping.
      char msg[96];
      StrSink m(msg, sizeof(msg));
      m.append("Unknown tool: ");
      m.append(toolName, strlen(toolName));
      writeRpcError(out, id, -32602, msg);
    }
    return finishOrTooLarge(id, out);
  }

  const JsonValue* args = jsonMember(r.params, "arguments");
  if (!args) {
    memcpy(b_.args, "{}", 3);
  } else {
    if (args->type != JsonType::Object) {
      writeRpcError(out, id, -32602, "invalid params");
      return finishOrTooLarge(id, out);
    }
    StrSink as(b_.args, b_.argsCap);
    if (!emitJson(args, as) || as.overflow()) {
      writeRpcError(out, id, -32602, "arguments too large");
      return finishOrTooLarge(id, out);
    }
  }

  ToolCallCtx ctx{};
  idToRequestId(id, ctx.requestId, sizeof(ctx.requestId));
  StrSink ts(b_.toolOut, b_.toolOutCap);
  Status st = tools_.dispatch(toolName, b_.args, ctx, ts);

  if (!writeRpcResultHead(out, id)) return finishOrTooLarge(id, out);
  if (st.ok()) {
    bool ok = out.append("{\"content\":[{\"type\":\"text\",\"text\":\"") &&
              emitEscaped(b_.toolOut, ts.len(), out) &&
              out.append("\"}],\"isError\":false}") && out.append("}");
    (void)ok;
    return finishOrTooLarge(id, out);
  }

  // Failed Status -> isError:true; text = escaped {"code":"X","detail":"Y"}.
  // Partial toolOut is dropped — the buffer is reused for the error JSON.
  {
    StrSink es(b_.toolOut, b_.toolOutCap);
    const char* detail = st.detail ? st.detail : errTitle(st.code);
    bool ok = es.append("{\"code\":\"") && es.append(errName(st.code)) &&
              es.append("\",\"detail\":\"") &&
              emitEscaped(detail, strlen(detail), es) && es.append("\"}");
    if (!ok) {
      out.reset();
      writeRpcError(out, id, -32603, "response too large");
      return true;
    }
    bool ok2 = out.append("{\"content\":[{\"type\":\"text\",\"text\":\"") &&
               emitEscaped(b_.toolOut, es.len(), out) &&
               out.append("\"}],\"isError\":true}") && out.append("}");
    (void)ok2;
  }
  return finishOrTooLarge(id, out);
}

bool McpServer::handle(const char* body, size_t len, StrSink& out) {
  out.reset();
  RpcRequest r;
  RpcParse p = parseRpc(*b_.doc, body, len, r);
  if (p == RpcParse::ParseError) {
    RpcId nullId;
    nullId.kind = RpcId::Kind::Null;
    writeRpcError(out, nullId, -32700, "Parse error");
    return true;
  }
  if (p == RpcParse::InvalidRequest) {
    writeRpcError(out, r.id, -32600, "Invalid Request");
    finishOrTooLarge(r.id, out);
    return true;
  }
  if (r.notification || r.clientResponse) return false;

  bool wrote = false;
  if (methodIs(r, "initialize")) {
    wrote = writeRpcResultHead(out, r.id) && emitInitialize(out, r) && out.append("}");
  } else if (methodIs(r, "ping")) {
    wrote = writeRpcResultHead(out, r.id) && out.append("{}}");
  } else if (methodIs(r, "tools/list")) {
    wrote = writeRpcResultHead(out, r.id) && emitToolsList(out) && out.append("}");
    if (!out.overflow() && !wrote) {
      // toolSpec failed (not a sink overflow) -> -32603
      out.reset();
      writeRpcError(out, r.id, -32603, "Internal error");
      return true;
    }
  } else if (methodIs(r, "tools/call")) {
    return toolCall(r, out);
  } else {
    writeRpcError(out, r.id, -32601, "Method not found");
    return true;
  }
  (void)wrote;
  return finishOrTooLarge(r.id, out);
}

}  // namespace controlme::mcp
