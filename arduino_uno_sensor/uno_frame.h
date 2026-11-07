// $GG,<gas>,<limit>,<clean air>,<state>,<warning>*HH\n
// HH = XOR of the characters between '$' and '*', in hex
// keep in sync with esp32_gateway/uno_link.h
#pragma once
#include <stdio.h>
#include <stdint.h>

inline uint8_t frameChecksum(const char* s, int n) {
  uint8_t x = 0;
  for (int i = 0; i < n; i++) x ^= (uint8_t)s[i];
  return x;
}

inline int buildFrame(char* out, int cap, int gas, int limit, int baseline,
                      const char* state, const char* warn) {
  char body[72];
  int n = snprintf(body, sizeof(body), "GG,%d,%d,%d,%s,%s", gas, limit, baseline, state, warn);
  if (n <= 0 || n >= (int)sizeof(body)) return 0;
  int m = snprintf(out, cap, "$%s*%02X\n", body, (unsigned)frameChecksum(body, n));
  return (m > 0 && m < cap) ? m : 0;
}
