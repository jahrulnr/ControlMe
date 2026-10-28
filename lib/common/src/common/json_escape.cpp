#include "common/json_escape.h"
#include <string.h>

namespace controlme {

namespace {

// UTF-8 sequence length from the lead byte; 0 = invalid lead (stray
// continuation or lead >0xF7).
inline uint8_t utf8SeqLen(uint8_t c) {
  if (c >= 0xC0 && c <= 0xDF) return 2;
  if (c >= 0xE0 && c <= 0xEF) return 3;
  if (c >= 0xF0 && c <= 0xF7) return 4;
  return 0;
}

}  // namespace

size_t jsonEscapeBounded(const char* s, size_t n, char* out, size_t outCap,
                         size_t* outLen, bool final) {
  static const char hx[] = "0123456789abcdef";
  size_t o = 0, i = 0;
  // Leave 1 byte for NUL.
  const size_t budget = outCap ? outCap - 1 : 0;
  for (; i < n && o < budget;) {
    const uint8_t c = (uint8_t)s[i];
    const char* e = nullptr;
    char u[6];
    size_t need;
    switch (c) {
      case '"': e = "\\\""; need = 2; break;
      case '\\': e = "\\\\"; need = 2; break;
      case '\n': e = "\\n"; need = 2; break;
      case '\r': e = "\\r"; need = 2; break;
      case '\t': e = "\\t"; need = 2; break;
      case '\b': e = "\\b"; need = 2; break;
      case '\f': e = "\\f"; need = 2; break;
      default:
        if (c < 0x20) {
          u[0] = '\\'; u[1] = 'u'; u[2] = '0'; u[3] = '0';
          u[4] = hx[(c >> 4) & 0xF]; u[5] = hx[c & 0xF];
          e = u; need = 6;
        } else {
          need = 0;  // signal: not a simple escape
        }
        break;
    }
    if (need) {
      if (o + need > budget) break;
      memcpy(out + o, e, need);
      o += need;
      ++i;
      continue;
    }
    if (c < 0x80) {
      ++i;
      out[o++] = (char)c;
      continue;
    }
    // Multibyte / byte >=0x80.
    const uint8_t seqLen = utf8SeqLen(c);
    if (!seqLen) {
      // Stray continuation / invalid lead -> �, consume 1 byte
      // (the next continuation byte is judged on its own -> consecutive �s).
      static const char kRep[] = "\\ufffd";
      if (o + 6 > budget) break;
      memcpy(out + o, kRep, 6);
      o += 6;
      ++i;
      continue;
    }
    // Count valid continuations available in input.
    size_t have = 0;
    while (have + 1 < seqLen && i + 1 + have < n &&
           ((uint8_t)s[i + 1 + have] & 0xC0) == 0x80) {
      ++have;
    }
    if (have + 1 < seqLen) {
      // Incomplete sequence: input truncation OR a broken continuation byte.
      const bool truncatedByInput = (i + 1 + have >= n);
      if (truncatedByInput && !final) {
        break;  // caller resumes from the same lead byte
      }
      static const char kRep[] = "\\ufffd";
      if (o + 6 > budget) break;
      memcpy(out + o, kRep, 6);
      o += 6;
      if (truncatedByInput) {
        i = n;  // consume the partial-sequence remainder when final
      } else {
        ++i;  // broken continuation: consume the lead only; the rest � on their own
      }
      continue;
    }
    if (o + seqLen > budget) break;
    memcpy(out + o, s + i, seqLen);
    o += seqLen;
    i += seqLen;
  }
  if (outCap) out[o] = '\0';
  if (outLen) *outLen = o;
  return i;
}

}  // namespace controlme
