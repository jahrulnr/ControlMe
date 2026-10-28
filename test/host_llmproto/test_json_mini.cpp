// JsonDoc fixture: Chat Completions chunk shape, escapes, explicit limits.
#include "llmproto/json_mini.h"
#include "fixture.h"

using controlme::llmproto::JsonDoc;
using controlme::llmproto::JsonErr;
using controlme::llmproto::JsonType;
using controlme::llmproto::JsonValue;
using controlme::llmproto::jsonAt;
using controlme::llmproto::jsonLen;
using controlme::llmproto::jsonMember;
using controlme::llmproto::jsonStrEq;

int main() {
  // 1) Typical provider tool_calls chunk shape.
  {
    const char* js =
        "{\"id\":\"chatcmpl-1\",\"object\":\"chat.completion.chunk\","
        "\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\","
        "\"tool_calls\":[{\"index\":0,\"id\":\"call_abc\",\"type\":\"function\","
        "\"function\":{\"name\":\"sd_list\",\"arguments\":\"{\\\"pa\"}}]},"
        "\"finish_reason\":null}]}";
    JsonDoc d;
    CHECK(d.parse(js, strlen(js)) == JsonErr::Ok);
    const JsonValue* root = d.root();
    CHECK(root && root->type == JsonType::Object);
    CHECK(jsonStrEq(jsonMember(root, "id"), "chatcmpl-1"));
    const JsonValue* choices = jsonMember(root, "choices");
    CHECK(choices && jsonLen(choices) == 1);
    const JsonValue* c0 = jsonAt(choices, 0);
    const JsonValue* delta = jsonMember(c0, "delta");
    const JsonValue* tc = jsonMember(delta, "tool_calls");
    CHECK(tc && jsonLen(tc) == 1);
    const JsonValue* t0 = jsonAt(tc, 0);
    const JsonValue* idx = jsonMember(t0, "index");
    CHECK(idx && idx->type == JsonType::Number && idx->num == 0);
    CHECK(jsonStrEq(jsonMember(t0, "id"), "call_abc"));
    const JsonValue* fn = jsonMember(t0, "function");
    CHECK(jsonStrEq(jsonMember(fn, "name"), "sd_list"));
    // arguments is a nested JSON string — escapes decoded.
    const JsonValue* args = jsonMember(fn, "arguments");
    CHECK(args && args->type == JsonType::String);
    CHECK(args->strLen == 4 && memcmp(args->str, "{\"pa", 4) == 0);
    // finish_reason null -> not a String.
    const JsonValue* fr = jsonMember(c0, "finish_reason");
    CHECK(fr && fr->type == JsonType::Null);
  }

  // 2) Escape dasar + \uXXXX (BMP) + surrogate pair + \n \/ \t.
  {
    const char* js = "{\"s\":\"a\\nb\\t\\\"q\\\" \\/ \\u0041 \\ud83d\\ude00\"}";
    JsonDoc d;
    CHECK(d.parse(js, strlen(js)) == JsonErr::Ok);
    const JsonValue* s = jsonMember(d.root(), "s");
    const char* want = "a\nb\t\"q\" / A \xF0\x9F\x98\x80";
    CHECK(s && s->strLen == strlen(want));
    CHECK(memcmp(s->str, want, s->strLen) == 0);
  }

  // 3) Lone surrogate -> U+FFFD (3 bytes), no crash.
  {
    const char* js = "{\"s\":\"\\ud800\"}";
    JsonDoc d;
    CHECK(d.parse(js, strlen(js)) == JsonErr::Ok);
    const JsonValue* s = jsonMember(d.root(), "s");
    CHECK(s && s->strLen == 3);
    CHECK((unsigned char)s->str[0] == 0xEF);
  }

  // 4) Malformed: unterminated string, trailing garbage, raw control.
  {
    JsonDoc d;
    const char* bad1 = "{\"a\":\"x";
    CHECK(d.parse(bad1, strlen(bad1)) == JsonErr::Malformed);
    const char* bad2 = "{} x";
    CHECK(d.parse(bad2, strlen(bad2)) == JsonErr::Malformed);
    const char* bad3 = "{\"a\":\"x\ny\"}";  // raw \n inside a string
    CHECK(d.parse(bad3, strlen(bad3)) == JsonErr::Malformed);
  }

  // 5) Explicit limits: giant input and too-deep nesting.
  {
    JsonDoc d;
    std::string huge(controlme::llmproto::kJsonInputMax + 1, ' ');
    CHECK(d.parse(huge.data(), huge.size()) == JsonErr::InputTooLarge);
    std::string deep;
    for (int i = 0; i < 40; ++i) deep += '[';
    for (int i = 0; i < 40; ++i) deep += ']';
    CHECK(d.parse(deep.data(), deep.size()) == JsonErr::TooDeep);
  }

  // 6) Node pool: object with >320 members -> NodePoolFull.
  {
    JsonDoc d;
    std::string js = "{";
    for (int i = 0; i < 400; ++i) {
      if (i) js += ',';
      char buf[16];
      snprintf(buf, sizeof(buf), "\"k%d\":1", i);
      js += buf;
    }
    js += '}';
    CHECK(d.parse(js.data(), js.size()) == JsonErr::NodePoolFull);
  }

  // 7) Numbers: integer & fraction; a non-integer index still parses
  //    (the assembler is what rejects it).
  {
    JsonDoc d;
    const char* js = "{\"i\":7,\"f\":-2.5e2}";
    CHECK(d.parse(js, strlen(js)) == JsonErr::Ok);
    CHECK(jsonMember(d.root(), "i")->num == 7);
    CHECK(jsonMember(d.root(), "f")->num == -250.0);
  }

  printf("PASS test_json_mini\n");
  return 0;
}
