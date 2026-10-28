// Tests the errName inverse (common/error.cpp): every catalog code must
// round-trip; foreign/empty names are rejected. The "failed:<NAME>" dedup
// replay (agent_runner/sessions) depends on this mapping.
#include <cstdio>
#include "common/error.h"

using namespace controlme;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      return 1;                                                    \
    }                                                              \
  } while (0)

int main() {
  for (uint8_t i = 0; i <= (uint8_t)Err::Internal; ++i) {
    const Err e = (Err)i;
    Err back = Err::None;
    CHECK(errFromName(errName(e), back));
    CHECK(back == e);
  }
  Err out = Err::None;
  CHECK(!errFromName("PIN", out));      // legacy non-catalog name
  CHECK(!errFromName("", out));
  CHECK(!errFromName(nullptr, out));

  std::printf("PASS test_err_name\n");
  return 0;
}
