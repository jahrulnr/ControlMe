#pragma once
// Host fixture helper for mcpproto tests — test-only, not lib.
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      printf("CHECK FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                  \
    }                                                            \
  } while (0)
