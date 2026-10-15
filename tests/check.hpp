#pragma once
// Tiny check macro shared by the dependency-free test drivers. Replaced by
// GoogleTest once vcpkg is wired in on the user's machine.
#include <cstdio>

inline int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)
