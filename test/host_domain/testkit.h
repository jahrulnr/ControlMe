#pragma once
// Shared test kit for domain host tests — header-only:
// run_host_tests.sh compiles each test_*.cpp standalone.
#include <cstdio>
#include <cstring>
#include "domain/dedup_cache.h"

using namespace controlme;
using namespace controlme::cfg;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      return 1;                                                         \
    }                                                                   \
  } while (0)
