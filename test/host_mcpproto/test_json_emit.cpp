// Tests emitEscaped + emitJson (parse->emit->parse round-trip).
#include "fixture.h"
#include "llmproto/json_mini.h"
#include "mcpproto/str_sink.h"
#include "mcpproto/json_emit.h"

using controlme::Status;
using controlme::llmproto::JsonDoc;
using controlme::llmproto::JsonErr;
using controlme::llmproto::JsonType;
using controlme::llmproto::JsonValue;
using controlme::llmproto::jsonMember;
using controlme::mcp::StrSink;
using controlme::mcp::emitEscaped;
using controlme::mcp::emitJson;

static bool esc(const char* s, size_t n, std::string& out) {
  char buf[4096];
  StrSink sink(buf, sizeof(buf));
  bool ok = emitEscaped(s, n, sink);
  out.assign(buf, sink.len());
  return ok;
}

// Round-trip: parse src -> emitJson -> re-parse -> emitJson -> same string.
static bool roundTrip(const char* src, std::string& emitted) {
  static JsonDoc doc;
  if (doc.parse(src, strlen(src)) != JsonErr::Ok) return false;
  static char buf[8192];
  StrSink s1(buf, sizeof(buf));
  if (!emitJson(doc.root(), s1)) return false;
  emitted.assign(buf, s1.len());
  static JsonDoc doc2;
  if (doc2.parse(buf, s1.len()) != JsonErr::Ok) return false;
  StrSink s2(buf, sizeof(buf));
  if (!emitJson(doc2.root(), s2)) return false;
  return emitted.size() == s2.len() && memcmp(emitted.data(), buf, s2.len()) == 0;
}

int main() {
  // --- escape table ---------------------------------------------------------
  {
    std::string o;
    CHECK(esc("a\"b\\c\nd\re\tf\bg\fh", 15, o));
    CHECK(o == "a\\\"b\\\\c\\nd\\re\\tf\\bg\\fh");
    // other control chars -> \u00XX
    CHECK(esc("x\x01y\x1f", 4, o));
    CHECK(o == "x\\u0001y\\u001f");
    // embedded NUL via length (not NUL-terminated)
    CHECK(esc("p\0q", 3, o));
    CHECK(o == "p\\u0000q");
    // UTF-8 >=0x80 passes through as-is
    const char* u8 = "\xC3\xA9\xE2\x82\xAC";  // é €
    CHECK(esc(u8, 5, o));
    CHECK(o == std::string(u8, 5));
  }

  // --- round-trip emitJson ---------------------------------------------------
  {
    std::string e;
    CHECK(roundTrip("{\"a\":[1,2.5,-3,true,false,null],\"s\":\"x\\ny\\u0001z\",\"o\":{\"k\":\"v\"}}", e));
    CHECK(roundTrip("9007199254740993", e));   // 2^53+1 -> %.17g
    CHECK(roundTrip("[]", e) && e == "[]");
    CHECK(roundTrip("{}", e) && e == "{}");
    // integral -> %.0f
    static JsonDoc d;
    CHECK(d.parse("{\"i\":7,\"f\":-250.0}", strlen("{\"i\":7,\"f\":-250.0}")) == JsonErr::Ok);
    char buf[256];
    StrSink s(buf, sizeof(buf));
    CHECK(emitJson(d.root(), s));
    CHECK(std::string(buf, s.len()).find("\"i\":7") != std::string::npos);
    CHECK(std::string(buf, s.len()).find("\"f\":-250") != std::string::npos);
    CHECK(strchr(buf, '\n') == nullptr);
  }

  // --- overflow -> false + sink.overflow() ------------------------------------
  {
    char tiny[8];
    StrSink s(tiny, sizeof(tiny));
    static JsonDoc d;
    CHECK(d.parse("{\"abc\":123456}", 14) == JsonErr::Ok);
    CHECK(!emitJson(d.root(), s));
    CHECK(s.overflow());
  }
  {
    char tiny[4];
    StrSink s(tiny, sizeof(tiny));
    CHECK(!emitEscaped("123456789", 9, s));
    CHECK(s.overflow());
  }

  printf("PASS test_json_emit\n");
  return 0;
}
