#include "mcpproto/json_emit.h"
#include <math.h>

namespace controlme::mcp {

namespace {

bool emitNumber(double v, StrSink& out) {
  // Integral within exact-double range -> no point; else %.17g.
  if (v == floor(v) && fabs(v) <= 9007199254740992.0) {
    return out.appendf("%.0f", v);
  }
  return out.appendf("%.17g", v);
}

bool emitString(const char* s, size_t len, StrSink& out) {
  return out.append("\"") && emitEscaped(s, len, out) && out.append("\"");
}

}  // namespace

bool emitEscaped(const char* s, size_t len, StrSink& out) {
  static const char hex[] = "0123456789abcdef";
  size_t start = 0;
  for (size_t i = 0; i < len; ++i) {
    const uint8_t c = (uint8_t)s[i];
    const char* esc = nullptr;
    switch (c) {
      case '"': esc = "\\\""; break;
      case '\\': esc = "\\\\"; break;
      case '\n': esc = "\\n"; break;
      case '\r': esc = "\\r"; break;
      case '\t': esc = "\\t"; break;
      case '\b': esc = "\\b"; break;
      case '\f': esc = "\\f"; break;
      default: break;
    }
    if (esc) {
      if (!out.append(s + start, i - start) || !out.append(esc)) return false;
      start = i + 1;
    } else if (c < 0x20) {
      char u[7] = {'\\', 'u', '0', '0', hex[(c >> 4) & 0xF], hex[c & 0xF], '\0'};
      if (!out.append(s + start, i - start) || !out.append(u, 6)) return false;
      start = i + 1;
    }
    // c >= 0x20 (incl. UTF-8 >=0x80): passes through as-is
  }
  return out.append(s + start, len - start);
}

bool emitJson(const llmproto::JsonValue* v, StrSink& out) {
  using llmproto::JsonType;
  if (!v) return out.append("null");
  switch (v->type) {
    case JsonType::Null: return out.append("null");
    case JsonType::Bool: return out.append(v->boolean ? "true" : "false");
    case JsonType::Number: return emitNumber(v->num, out);
    case JsonType::String: return emitString(v->str, v->strLen, out);
    case JsonType::Array: {
      if (!out.append("[")) return false;
      bool first = true;
      for (const auto* c = v->child; c; c = c->next) {
        if (!first && !out.append(",")) return false;
        first = false;
        if (!emitJson(c, out)) return false;
      }
      return out.append("]");
    }
    case JsonType::Object: {
      if (!out.append("{")) return false;
      bool first = true;
      for (const auto* c = v->child; c; c = c->next) {
        if (!first && !out.append(",")) return false;
        first = false;
        if (!emitString(c->key, c->keyLen, out) || !out.append(":") ||
            !emitJson(c, out)) {
          return false;
        }
      }
      return out.append("}");
    }
  }
  return false;
}

}  // namespace controlme::mcp
