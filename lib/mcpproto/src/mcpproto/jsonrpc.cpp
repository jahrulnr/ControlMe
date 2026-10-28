#include "mcpproto/jsonrpc.h"
#include "mcpproto/json_emit.h"
#include <string.h>

namespace controlme::mcp {

using llmproto::JsonType;
using llmproto::JsonValue;
using llmproto::jsonMember;

namespace {

bool readId(const JsonValue* id, RpcId& out) {
  // true = valid id filled; false = invalid type (caller -> InvalidRequest).
  if (!id) {
    out.kind = RpcId::Kind::None;
    return true;
  }
  switch (id->type) {
    case JsonType::Null:
      out.kind = RpcId::Kind::Null;
      return true;
    case JsonType::Number:
      out.kind = RpcId::Kind::Number;
      out.num = id->num;
      return true;
    case JsonType::String:
      if (id->strLen >= sizeof(out.str)) return false;
      memcpy(out.str, id->str, id->strLen);
      out.str[id->strLen] = '\0';
      out.kind = RpcId::Kind::String;
      return true;
    default:
      return false;
  }
}

}  // namespace

RpcParse parseRpc(llmproto::JsonDoc& doc, const char* body, size_t len, RpcRequest& out) {
  out = RpcRequest{};
  llmproto::JsonErr e = doc.parse(body, len);
  if (e == llmproto::JsonErr::Malformed) return RpcParse::ParseError;
  if (e != llmproto::JsonErr::Ok) return RpcParse::InvalidRequest;

  const JsonValue* root = doc.root();
  if (!root || root->type != JsonType::Object) return RpcParse::InvalidRequest;  // batches etc.

  // id first — later errors still echo the id when one was parsed.
  if (!readId(jsonMember(root, "id"), out.id)) return RpcParse::InvalidRequest;
  out.notification = (out.id.kind == RpcId::Kind::None);

  const JsonValue* method = jsonMember(root, "method");
  if (method) {
    const JsonValue* jr = jsonMember(root, "jsonrpc");
    if (method->type != JsonType::String || !jr || !llmproto::jsonStrEq(jr, "2.0")) {
      return RpcParse::InvalidRequest;
    }
    out.method = method->str;
    out.methodLen = method->strLen;
    out.params = jsonMember(root, "params");
    return RpcParse::Ok;
  }
  if (jsonMember(root, "result") || jsonMember(root, "error")) {
    out.clientResponse = true;
    return RpcParse::Ok;
  }
  return RpcParse::InvalidRequest;
}

bool writeRpcId(StrSink& out, const RpcId& id) {
  switch (id.kind) {
    case RpcId::Kind::None:
    case RpcId::Kind::Null:
      return out.append("null");
    case RpcId::Kind::Number: {
      JsonValue v{};
      v.type = JsonType::Number;
      v.num = id.num;
      return emitJson(&v, out);
    }
    case RpcId::Kind::String:
      return out.append("\"") && emitEscaped(id.str, strlen(id.str), out) && out.append("\"");
  }
  return false;
}

bool writeRpcResultHead(StrSink& out, const RpcId& id) {
  return out.append("{\"jsonrpc\":\"2.0\",\"id\":") && writeRpcId(out, id) &&
         out.append(",\"result\":");
}

bool writeRpcError(StrSink& out, const RpcId& id, int code, const char* message) {
  return out.append("{\"jsonrpc\":\"2.0\",\"id\":") && writeRpcId(out, id) &&
         out.append(",\"error\":{\"code\":") && out.appendf("%d", code) &&
         out.append(",\"message\":") && out.append("\"") &&
         emitEscaped(message, strlen(message), out) && out.append("\"}}");
}

bool methodIs(const RpcRequest& r, const char* m) {
  size_t n = strlen(m);
  return r.method && r.methodLen == n && memcmp(r.method, m, n) == 0;
}

}  // namespace controlme::mcp
