#pragma once
// Serializes the json_mini DOM -> compact JSON (no newlines — one line,
// compatible with SSE "data: ..." frames). Everything via the caller's
// StrSink.
#include <stddef.h>
#include "llmproto/json_mini.h"
#include "mcpproto/str_sink.h"

namespace controlme::mcp {

// JSON string body (no enclosing quotes), length-BASED — s may contain
// NUL. Escapes: " \\ \n \r \t \b \f, <0x20 -> \u00XX, >=0x80 passes
// through.
bool emitEscaped(const char* s, size_t len, StrSink& out);

// Re-serialize one JsonValue (object/array/string/number/bool/null).
// Number: integral and |v| <= 2^53 -> %.0f, else %.17g.
bool emitJson(const llmproto::JsonValue* v, StrSink& out);

}  // namespace controlme::mcp
