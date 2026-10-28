#pragma once
// Minimal internal JSON parser (no dependencies, no heap).
// Enough to read one Chat Completions chunk object per SSE event:
//   choices[].delta{content?, tool_calls[]?}, choices[].finish_reason,
//   tool_calls[i]{index, id?, function{name?, arguments?}}
// Small DOM in fixed pools: nodes + decoded strings (basic escapes + \uXXXX).
// Explicit limits — giant input / too deep / full pools -> JsonErr.
// Not thread-safe; one JsonDoc per document per parse().
#include <stddef.h>
#include <stdint.h>

namespace controlme::llmproto {

inline constexpr size_t kJsonInputMax = 16 * 1024;    // bytes per one SSE event
inline constexpr size_t kJsonStrPoolMax = 16 * 1024;  // decoded <= raw input
inline constexpr uint16_t kJsonNodeMax = 320;
inline constexpr uint8_t kJsonDepthMax = 24;

enum class JsonType : uint8_t { Null, Bool, Number, String, Array, Object };

enum class JsonErr : uint8_t {
  Ok = 0,
  Malformed,      // broken syntax / unknown token
  InputTooLarge,  // len > kJsonInputMax
  TooDeep,        // nesting > kJsonDepthMax
  NodePoolFull,   // node > kJsonNodeMax
  StrPoolFull     // decoded strings > kJsonStrPoolMax
};

struct JsonValue {
  JsonType type = JsonType::Null;
  bool boolean = false;
  double num = 0.0;              // Number
  const char* str = nullptr;     // String: decoded, NOT NUL-terminated
  uint32_t strLen = 0;
  const char* key = nullptr;     // Object member: decoded key
  uint32_t keyLen = 0;
  JsonValue* child = nullptr;    // first child (Array/Object), siblings via next
  JsonValue* next = nullptr;
};

class JsonDoc {
 public:
  // Parse one whole document. Pool state is reset every parse().
  JsonErr parse(const char* data, size_t len);
  const JsonValue* root() const { return root_; }

 private:
  JsonValue* allocNode();
  JsonValue* parseValue(uint8_t depth);
  JsonValue* parseLiteral(JsonType t, const char* word, size_t wordLen, bool b);
  JsonValue* parseNumber();
  JsonValue* parseArray(uint8_t depth);
  JsonValue* parseObject(uint8_t depth);
  JsonErr parseStringRaw(const char*& out, uint32_t& outLen);
  bool parseHex4(uint16_t& out);
  void skipWs();
  bool pushChar(char c);          // false = StrPoolFull
  bool pushUtf8(uint32_t cp);     // encode 1-4 bytes into the pool

  JsonValue nodes_[kJsonNodeMax];
  uint16_t nodeUsed_ = 0;
  char strPool_[kJsonStrPoolMax];
  size_t strUsed_ = 0;
  const char* p_ = nullptr;
  const char* end_ = nullptr;
  JsonValue* root_ = nullptr;
  JsonErr err_ = JsonErr::Ok;
};

// Small accessors over the DOM (all safe for nullptr/wrong-type args).
const JsonValue* jsonMember(const JsonValue* obj, const char* key);
size_t jsonLen(const JsonValue* arr);                       // child count
const JsonValue* jsonAt(const JsonValue* arr, size_t i);    // i-th child
bool jsonStrEq(const JsonValue* v, const char* s);          // v String == s (NUL-term)

}  // namespace controlme::llmproto
