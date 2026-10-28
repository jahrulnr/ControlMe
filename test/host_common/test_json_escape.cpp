// Tests jsonEscapeBounded — escapes, budget cuts, partial/invalid UTF-8.
#include <stdio.h>
#include <string.h>
#include "common/json_escape.h"

using controlme::jsonEscapeBounded;

#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      printf("CHECK FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                  \
    }                                                            \
  } while (0)

static size_t esc(const char* s, size_t n, char* out, size_t cap, size_t* ol,
                  bool final) {
  return jsonEscapeBounded(s, n, out, cap, ol, final);
}

int main() {
  char buf[64];
  size_t ol = 999;

  // ASCII + escapes
  size_t c = esc("a\"b\\c\nd", 7, buf, sizeof(buf), &ol, true);
  CHECK(c == 7 && ol == 10 && strcmp(buf, "a\\\"b\\\\c\\nd") == 0);
  // NUL -> �0
  c = esc("p\0q", 3, buf, sizeof(buf), &ol, true);
  CHECK(c == 3 && strcmp(buf, "p\\u0000q") == 0);

  // Budget cut right before an escape that doesn't fit: "ab\nc" with
  // cap 5 (4 content + NUL) -> "ab" + "\\n" (2) = 4 fits, 'c' stops.
  c = esc("ab\ncd", 5, buf, 5, &ol, true);
  CHECK(c == 3 && ol == 4 && strcmp(buf, "ab\\n") == 0);  // a,b,\\n = 3 input bytes
  // cap 4: "ab" (2) + "\\n" doesn't fit -> stop before '\n'
  c = esc("ab\ncd", 5, buf, 4, &ol, true);
  CHECK(c == 2 && ol == 2 && strcmp(buf, "ab") == 0);

  // UTF-8: em dash E2 80 94, input-truncated with final=false -> consumed
  // stops before E2, no partial bytes in output.
  const char* em = "x\xE2\x80\x94";
  c = esc(em, 3, buf, sizeof(buf), &ol, false);  // n=3: "x" + E2 80 (1 short)
  CHECK(c == 1 && ol == 1 && strcmp(buf, "x") == 0);
  // final=true on a truncated sequence -> �, consume the rest.
  c = esc(em, 3, buf, sizeof(buf), &ol, true);
  CHECK(c == 3 && ol == 7 && strcmp(buf, "x\\ufffd") == 0);
  // intact sequence -> passes as-is.
  c = esc(em, 4, buf, sizeof(buf), &ol, false);
  CHECK(c == 4 && ol == 4 && memcmp(buf, "x\xE2\x80\x94", 4) == 0);

  // Stray continuation 0x80 -> �
  c = esc("a\x80" "b", 3, buf, sizeof(buf), &ol, true);
  CHECK(c == 3 && strcmp(buf, "a\\ufffdb") == 0);

  // Broken continuation mid-sequence (E2 41 94): lead -> �, 'A' ASCII,
  // 0x94 stray -> �
  c = esc("\xE2" "A\x94", 3, buf, sizeof(buf), &ol, true);
  CHECK(c == 3 && strcmp(buf, "\\ufffdA\\ufffd") == 0);

  // 4-byte emoji F0 9F 98 80 intact
  const char* emo = "z\xF0\x9F\x98\x80!";
  c = esc(emo, 6, buf, sizeof(buf), &ol, true);
  CHECK(c == 6 && ol == 6 && memcmp(buf, "z\xF0\x9F\x98\x80!", 6) == 0);

  // Budget stops right at a multibyte sequence: cap 3 ("z" + 1 byte) ->
  // c==1? seqLen 4 > remaining budget -> break before the lead.
  c = esc(emo, 6, buf, 3, &ol, true);
  CHECK(c == 1 && ol == 1 && strcmp(buf, "z") == 0);

  printf("PASS test_json_escape\n");
  return 0;
}
