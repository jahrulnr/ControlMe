// Tests parseRpc + head/error/id writers.
#include "fixture.h"
#include "llmproto/json_mini.h"
#include "mcpproto/str_sink.h"
#include "mcpproto/jsonrpc.h"

using controlme::llmproto::JsonDoc;
using controlme::llmproto::JsonErr;
using controlme::llmproto::JsonType;
using controlme::llmproto::jsonMember;
using controlme::llmproto::jsonStrEq;
using controlme::mcp::RpcId;
using controlme::mcp::RpcParse;
using controlme::mcp::RpcRequest;
using controlme::mcp::StrSink;
using controlme::mcp::methodIs;
using controlme::mcp::parseRpc;
using controlme::mcp::writeRpcError;

static RpcParse parse(const char* js, RpcRequest& out) {
  static JsonDoc doc;
  return parseRpc(doc, js, strlen(js), out);
}

int main() {
  RpcRequest r;

  CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":\"abc-1\",\"method\":\"ping\"}", r) == RpcParse::Ok);
  CHECK(r.id.kind == RpcId::Kind::String && strcmp(r.id.str, "abc-1") == 0);
  CHECK(!r.notification && methodIs(r, "ping") && !methodIs(r, "pong"));

  CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}", r) == RpcParse::Ok);
  CHECK(r.id.kind == RpcId::Kind::Number && r.id.num == 7);

  CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"ping\"}", r) == RpcParse::Ok);
  CHECK(r.id.kind == RpcId::Kind::Null && !r.notification);

  CHECK(parse("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", r) == RpcParse::Ok);
  CHECK(r.notification && r.id.kind == RpcId::Kind::None);

  CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":{},\"method\":\"ping\"}", r) == RpcParse::InvalidRequest);

  {
    std::string longId = "{\"jsonrpc\":\"2.0\",\"id\":\"";
    longId.append(60, 'x');
    longId += "\",\"method\":\"ping\"}";
    CHECK(parse(longId.c_str(), r) == RpcParse::InvalidRequest);
  }

  CHECK(parse("{\"id\":1,\"method\":\"ping\"}", r) == RpcParse::InvalidRequest);
  CHECK(parse("{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"ping\"}", r) == RpcParse::InvalidRequest);
  CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":5}", r) == RpcParse::InvalidRequest);
  CHECK(parse("{not json", r) == RpcParse::ParseError);
  CHECK(parse("[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}]", r) == RpcParse::InvalidRequest);

  // response from a client (not a request)
  CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}", r) == RpcParse::Ok);
  CHECK(r.clientResponse && !r.notification);
  CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-1}}", r) == RpcParse::Ok);
  CHECK(r.clientResponse);

  // no method + no result/error -> InvalidRequest
  CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":1}", r) == RpcParse::InvalidRequest);

  // InvalidRequest still carries the parsed id
  CHECK(parse("{\"jsonrpc\":\"1.0\",\"id\":\"keep-me\",\"method\":\"x\"}", r) == RpcParse::InvalidRequest);
  CHECK(r.id.kind == RpcId::Kind::String && strcmp(r.id.str, "keep-me") == 0);

  // --- writeRpcError shape (parse balik) ---------------------------------------
  {
    char buf[512];
    StrSink s(buf, sizeof(buf));
    RpcId id;
    id.kind = RpcId::Kind::String;
    strcpy(id.str, "req\"9");  // needs escaping
    CHECK(writeRpcError(s, id, -32602, "bad \"params\""));
    static JsonDoc d;
    CHECK(d.parse(buf, s.len()) == JsonErr::Ok);
    const auto* root = d.root();
    CHECK(jsonStrEq(jsonMember(root, "jsonrpc"), "2.0"));
    CHECK(jsonStrEq(jsonMember(root, "id"), "req\"9"));
    const auto* err = jsonMember(root, "error");
    CHECK(err && err->type == JsonType::Object);
    CHECK(jsonMember(err, "code")->num == -32602);
    CHECK(jsonStrEq(jsonMember(err, "message"), "bad \"params\""));
    CHECK(strchr(buf, '\n') == nullptr);
  }

  printf("PASS test_jsonrpc\n");
  return 0;
}
