// Tests McpServer: JSON-RPC dispatch -> mocked IToolDispatcher catalog.
#include "fixture.h"
#include "common/config.h"
#include "ports/actions.h"
#include "llmproto/json_mini.h"
#include "mcpproto/str_sink.h"
#include "mcpproto/json_emit.h"
#include "mcpproto/mcp_dispatch.h"

using namespace controlme;
using controlme::llmproto::JsonDoc;
using controlme::llmproto::JsonErr;
using controlme::llmproto::JsonType;
using controlme::llmproto::JsonValue;
using controlme::llmproto::jsonAt;
using controlme::llmproto::jsonLen;
using controlme::llmproto::jsonMember;
using controlme::llmproto::jsonStrEq;
using controlme::mcp::McpBuffers;
using controlme::mcp::McpServer;
using controlme::mcp::StrSink;
using controlme::mcp::emitJson;

struct MockTools : IToolDispatcher {
  bool specFail = false;
  std::string lastName;
  std::string lastArgs;
  std::string lastReqId;
  int calls = 0;

  uint8_t toolCount() override { return 4; }
  bool toolSpec(uint8_t i, char* name, size_t ncap, char* desc, size_t dcap,
                char* schema, size_t scap) override {
    if (specFail) return false;
    const char* n = i == 0 ? "echo" : i == 1 ? "fail" : i == 2 ? "img" : "imglink";
    const char* d = i == 0 ? "echo args" : i == 1 ? "always fails" : "image tool";
    const char* s = i == 0 ? "{\"type\":\"object\"}"
                           : "{\"type\":\"object\",\"properties\":{}}";
    if (strlen(n) >= ncap || strlen(d) >= dcap || strlen(s) >= scap) return false;
    strcpy(name, n);
    strcpy(desc, d);
    strcpy(schema, s);
    return true;
  }
  Status dispatch(const char* toolName, const char* argsJson,
                  const ToolCallCtx& ctx, IByteSink& resultJson,
                  ToolAttach* att) override {
    ++calls;
    lastName = toolName;
    lastArgs = argsJson;
    lastReqId = ctx.requestId;
    if (strcmp(toolName, "echo") == 0) {
      return resultJson.write(reinterpret_cast<const uint8_t*>(argsJson), strlen(argsJson));
    }
    if (strncmp(toolName, "img", 3) == 0) {
      const char* meta = "{\"imageId\":\"img-1\",\"bytes\":3}";
      Status w = resultJson.write(reinterpret_cast<const uint8_t*>(meta), strlen(meta));
      if (!w.ok()) return w;
      if (att) {
        strcpy(att->uri, "http://192.0.2.1/api/v1/images/img-1");
        strcpy(att->name, "img-1");
        strcpy(att->mime, "image/jpeg");
        // "img" writes the base64 payload too; "imglink" stays link-only.
        if (att->data && strcmp(toolName, "img") == 0) {
          att->data->write(reinterpret_cast<const uint8_t*>("aGk="), 4);
          att->hasData = true;
        }
      }
      return Status::Ok();
    }
    return Status::Fail(Err::SdUnavailable, "no card");
  }
};

static JsonDoc g_doc;

struct Env {
  MockTools tools;
  JsonDoc* doc = &g_doc;
  char args[2048];
  char toolOut[8192];
  char attach[8192];
  McpServer srv{tools, McpBuffers{doc, args, sizeof(args), toolOut,
                                  sizeof(toolOut), attach, sizeof(attach)}};
};

// handle -> parse envelope -> root (static doc; don't use alongside srv).
static const JsonValue* handle(Env& e, const char* body, char* out, size_t cap,
                               bool& responded) {
  StrSink s(out, cap);
  responded = e.srv.handle(body, strlen(body), s);
  static JsonDoc d;
  if (!responded) return nullptr;
  if (d.parse(out, s.len()) != JsonErr::Ok) return nullptr;
  return d.root();
}

static const JsonValue* handle(Env& e, const std::string& body, char* out, size_t cap,
                               bool& responded) {
  return handle(e, body.c_str(), out, cap, responded);
}

static int errCode(const JsonValue* root) {
  const JsonValue* e = jsonMember(root, "error");
  if (!e) return 0;
  return (int)jsonMember(e, "code")->num;
}

int main() {
  char out[64 * 1024];
  bool rsp;

  // --- initialize -------------------------------------------------------------
  for (const char* v : {"2024-11-05", "2025-03-26", "2025-06-18"}) {
    Env e;
    std::string b = std::string("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"") + v + "\"}}";
    const JsonValue* r = handle(e, b, out, sizeof(out), rsp);
    CHECK(rsp && r);
    CHECK(jsonStrEq(jsonMember(jsonMember(r, "result"), "protocolVersion"), v));
    CHECK(jsonStrEq(jsonMember(jsonMember(jsonMember(r, "result"), "serverInfo"), "name"), "controlme"));
    CHECK(jsonStrEq(jsonMember(jsonMember(jsonMember(r, "result"), "serverInfo"), "version"), "0.1.0"));
  }
  {
    Env e;
    const JsonValue* r = handle(e, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"1999-01-01\"}}", out, sizeof(out), rsp);
    CHECK(r && jsonStrEq(jsonMember(jsonMember(r, "result"), "protocolVersion"), "2025-06-18"));
    // missing params still succeed (lenient)
    r = handle(e, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"initialize\"}", out, sizeof(out), rsp);
    CHECK(r && jsonMember(r, "result"));
  }

  // --- ping ---------------------------------------------------------------------
  {
    Env e;
    const JsonValue* r = handle(e, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}", out, sizeof(out), rsp);
    CHECK(r && jsonMember(r, "result") && jsonMember(r, "result")->type == JsonType::Object);
  }

  // --- tools/list -----------------------------------------------------------------
  {
    Env e;
    const JsonValue* r = handle(e, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/list\"}", out, sizeof(out), rsp);
    const JsonValue* ts = jsonMember(jsonMember(r, "result"), "tools");
    CHECK(ts && jsonLen(ts) == 4);
    const JsonValue* t0 = jsonAt(ts, 0);
    CHECK(jsonStrEq(jsonMember(t0, "name"), "echo"));
    CHECK(jsonStrEq(jsonMember(t0, "description"), "echo args"));
    const JsonValue* sch = jsonMember(t0, "inputSchema");
    CHECK(sch && sch->type == JsonType::Object);
    CHECK(jsonStrEq(jsonMember(sch, "type"), "object"));
    // toolSpec failed -> -32603
    Env e2;
    e2.tools.specFail = true;
    r = handle(e2, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/list\"}", out, sizeof(out), rsp);
    CHECK(r && errCode(r) == -32603);
  }

  // --- tools/call -------------------------------------------------------------------
  {
    Env e;
    const JsonValue* r = handle(e,
        "{\"jsonrpc\":\"2.0\",\"id\":\"rid-1\",\"method\":\"tools/call\",\"params\":{\"name\":\"echo\",\"arguments\":{\"a\":[1,2],\"s\":\"x\"}}}",
        out, sizeof(out), rsp);
    CHECK(r);
    const JsonValue* res = jsonMember(r, "result");
    CHECK(res);
    const JsonValue* c0 = jsonAt(jsonMember(res, "content"), 0);
    CHECK(jsonStrEq(jsonMember(c0, "type"), "text"));
    const JsonValue* txt = jsonMember(c0, "text");
    // text = raw args JSON (escaped inside the envelope) — re-parse
    static JsonDoc d2;
    CHECK(d2.parse(txt->str, txt->strLen) == JsonErr::Ok);
    CHECK(jsonLen(jsonMember(d2.root(), "a")) == 2);
    CHECK(jsonStrEq(jsonMember(d2.root(), "s"), "x"));
    const JsonValue* ie = jsonMember(res, "isError");
    CHECK(ie && ie->type == JsonType::Bool && !ie->boolean);
    // requestId forwarded from the string id
    CHECK(e.tools.lastReqId == "rid-1");
  }
  {
    Env e;
    const JsonValue* r = handle(e,
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\"}}",
        out, sizeof(out), rsp);
    const JsonValue* txt = jsonMember(jsonAt(jsonMember(jsonMember(r, "result"), "content"), 0), "text");
    CHECK(jsonStrEq(txt, "{}"));
    CHECK(e.tools.lastReqId == "5");  // number id formatted
  }
  {
    Env e;
    const JsonValue* r = handle(e,
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"fail\",\"arguments\":{}}}",
        out, sizeof(out), rsp);
    const JsonValue* res = jsonMember(r, "result");
    const JsonValue* ie = jsonMember(res, "isError");
    CHECK(ie && ie->type == JsonType::Bool && ie->boolean);
    const JsonValue* txt = jsonMember(jsonAt(jsonMember(res, "content"), 0), "text");
    static JsonDoc d2;
    CHECK(d2.parse(txt->str, txt->strLen) == JsonErr::Ok);
    CHECK(jsonStrEq(jsonMember(d2.root(), "code"), "SD_UNAVAILABLE"));
    CHECK(jsonStrEq(jsonMember(d2.root(), "detail"), "no card"));
  }
  {
    Env e;
    const JsonValue* r = handle(e,
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"nope\",\"arguments\":{}}}",
        out, sizeof(out), rsp);
    CHECK(r && errCode(r) == -32602);
    CHECK(e.tools.calls == 0);  // dispatch not called
  }
  {
    Env e;
    // arguments not an object -> -32602
    const JsonValue* r = handle(e,
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\",\"arguments\":[1]}}",
        out, sizeof(out), rsp);
    CHECK(r && errCode(r) == -32602);
    // missing params -> -32602
    r = handle(e, "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\"}", out, sizeof(out), rsp);
    CHECK(r && errCode(r) == -32602);
  }
  {
    // tool output contains quote+newline -> double-escape correct
    Env e;
    const JsonValue* r = handle(e,
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\",\"arguments\":{\"s\":\"a\\\"b\\nc\"}}}",
        out, sizeof(out), rsp);
    CHECK(r);
    const JsonValue* txt = jsonMember(jsonAt(jsonMember(jsonMember(r, "result"), "content"), 0), "text");
    static JsonDoc d2;
    CHECK(d2.parse(txt->str, txt->strLen) == JsonErr::Ok);
    CHECK(jsonStrEq(jsonMember(d2.root(), "s"), "a\"b\nc"));
  }
  {
    // toolSpec failed during the tools/call catalog scan -> -32603
    Env e;
    e.tools.specFail = true;
    const JsonValue* r = handle(e,
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\",\"arguments\":{}}}",
        out, sizeof(out), rsp);
    CHECK(r && errCode(r) == -32603);
  }
  {
    // attachment: content = text + image (base64, no escaping) + resource_link
    Env e;
    const JsonValue* r = handle(e,
        "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"tools/call\",\"params\":{\"name\":\"img\",\"arguments\":{}}}",
        out, sizeof(out), rsp);
    CHECK(r);
    const JsonValue* res = jsonMember(r, "result");
    const JsonValue* cont = jsonMember(res, "content");
    CHECK(cont && jsonLen(cont) == 3);
    const JsonValue* c1 = jsonAt(cont, 1);
    CHECK(jsonStrEq(jsonMember(c1, "type"), "image"));
    CHECK(jsonStrEq(jsonMember(c1, "data"), "aGk="));
    CHECK(jsonStrEq(jsonMember(c1, "mimeType"), "image/jpeg"));
    const JsonValue* c2 = jsonAt(cont, 2);
    CHECK(jsonStrEq(jsonMember(c2, "type"), "resource_link"));
    CHECK(jsonStrEq(jsonMember(c2, "uri"), "http://192.0.2.1/api/v1/images/img-1"));
    CHECK(jsonStrEq(jsonMember(c2, "name"), "img-1"));
    CHECK(jsonStrEq(jsonMember(c2, "mimeType"), "image/jpeg"));
  }
  {
    // link-only attachment (payload skipped/failed): text + resource_link
    Env e;
    const JsonValue* r = handle(e,
        "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"tools/call\",\"params\":{\"name\":\"imglink\",\"arguments\":{}}}",
        out, sizeof(out), rsp);
    CHECK(r);
    const JsonValue* cont = jsonMember(jsonMember(r, "result"), "content");
    CHECK(cont && jsonLen(cont) == 2);
    const JsonValue* c1 = jsonAt(cont, 1);
    CHECK(jsonStrEq(jsonMember(c1, "type"), "resource_link"));
    CHECK(jsonStrEq(jsonMember(c1, "uri"), "http://192.0.2.1/api/v1/images/img-1"));
  }
  {
    // b_.attach == nullptr: callee can't stream inline -> link-only still.
    MockTools tools;
    JsonDoc* doc = &g_doc;
    char a2[256], t2[1024];
    McpServer srv{tools, McpBuffers{doc, a2, sizeof(a2), t2, sizeof(t2),
                                  nullptr, 0}};
    const char* body =
        "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"tools/call\",\"params\":{\"name\":\"img\",\"arguments\":{}}}";
    StrSink s(out, sizeof(out));
    CHECK(srv.handle(body, strlen(body), s));
    static JsonDoc d;
    CHECK(d.parse(out, s.len()) == JsonErr::Ok);
    const JsonValue* cont = jsonMember(jsonMember(d.root(), "result"), "content");
    CHECK(cont && jsonLen(cont) == 2);
    CHECK(jsonStrEq(jsonMember(jsonAt(cont, 1), "type"), "resource_link"));
  }

  // --- unknown method / notification / response / error -------------------------
  {
    Env e;
    const JsonValue* r = handle(e, "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"resources/list\"}", out, sizeof(out), rsp);
    CHECK(r && errCode(r) == -32601);
    // notification -> no response
    const char* notif = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    StrSink s(out, sizeof(out));
    CHECK(!e.srv.handle(notif, strlen(notif), s));
    CHECK(s.len() == 0);
    s.reset();
    const char* cres = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}";
    CHECK(!e.srv.handle(cres, strlen(cres), s));
    CHECK(s.len() == 0);
    // malformed -> -32700 null id
    const JsonValue* rr = handle(e, "{oops", out, sizeof(out), rsp);
    CHECK(rr && errCode(rr) == -32700);
    CHECK(jsonMember(rr, "id")->type == JsonType::Null);
  }
  {
    // small out buffer -> overflow -> -32603 "response too large"
    Env e;
    char tiny[96];
    const char* tl = "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/list\"}";
    StrSink s(tiny, sizeof(tiny));
    CHECK(e.srv.handle(tl, strlen(tl), s));
    static JsonDoc d;
    CHECK(d.parse(tiny, s.len()) == JsonErr::Ok);
    CHECK(errCode(d.root()) == -32603);
    CHECK(!s.overflow());
  }
  {
    // body > kJsonInputMax -> -32600
    Env e;
    std::string big(llmproto::kJsonInputMax + 1, ' ');
    const JsonValue* r = handle(e, big, out, sizeof(out), rsp);
    CHECK(r && errCode(r) == -32600);
  }

  printf("PASS test_mcp_dispatch\n");
  return 0;
}
