// Builds the one-line message the Uno sends to the ESP32 once per second.
//
//   $GG,<gas>,<limit>,<clean air>,<state>,<warning>*HH\n
//
// The three numbers use the 0-4095 scale the web app expects (the Uno's 0-1023 reading x 4).
// HH is the XOR of every character between '$' and '*', in hex. The ESP32 drops any line whose
// checksum does not match, so noise on the wire can never look like a real reading.
// Keep this file identical in meaning to esp32_gateway/uno_link.h (that one reads the frame).
#pragma once
#include <stdio.h>
#include <stdint.h>

inline uint8_t frameChecksum(const char* s, int n) {
  uint8_t x = 0;
  for (int i = 0; i < n; i++) x ^= (uint8_t)s[i];
  return x;
}

// Returns the length written to out, or 0 if it did not fit.
inline int buildFrame(char* out, int cap, int gas, int limit, int baseline,
                      const char* state, const char* warn) {
  char body[72];
  int n = snprintf(body, sizeof(body), "GG,%d,%d,%d,%s,%s", gas, limit, baseline, state, warn);
  if (n <= 0 || n >= (int)sizeof(body)) return 0;
  int m = snprintf(out, cap, "$%s*%02X\n", body, (unsigned)frameChecksum(body, n));
  return (m > 0 && m < cap) ? m : 0;
}
