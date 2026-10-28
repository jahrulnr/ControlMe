#pragma once
// JSON string-body escaping (no quotes) — length-based + budget-bounded.
// Bounded version of transport::jsonEscape for serialized sources (log
// file): stops BEFORE an escape char that doesn't fit, never emits a partial
// UTF-8 sequence, invalid UTF-8 bytes -> � so output stays valid UTF-8 for
// strict decoders.
#include <stddef.h>
#include <stdint.h>

namespace controlme {

// Escape s[0..n) into out (NUL-terminated, outCap includes NUL).
// *outLen = escaped bytes written. Return = INPUT bytes consumed.
// final=false (more data after s, e.g. file not at EOF): a UTF-8 sequence
// cut at the end of input is NOT emitted — consumed stops before its lead
// byte so the caller resumes from the same byte. final=true: cut/invalid
// sequences are emitted as � and consumed.
size_t jsonEscapeBounded(const char* s, size_t n, char* out, size_t outCap,
                         size_t* outLen, bool final);

}  // namespace controlme
