// $GG,<gas>,<limit>,<clean air>,<state>,<warning>*HH
#pragma once
#include <stdint.h>
#include <string.h>

struct UnoFrame {
  int  gas = 0, threshold = 0, baseline = 0;
  char state[16] = "";
  char warn[16]  = "";
};

inline bool unoStateValid(const char* s) {
  static const char* const ok[] = { "calibrating", "normal", "alert", "sensor_error" };
  for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) if (strcmp(s, ok[i]) == 0) return true;
  return false;
}
inline bool unoWarnValid(const char* s) {
  return s[0] == 0 || strcmp(s, "baseline_high") == 0 || strcmp(s, "baseline_low") == 0;
}

inline int unoNumber(const char* s) {
  if (!*s) return -1;
  long v = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') return -1;
    v = v * 10 + (*s - '0');
    if (v > 4095) return -1;
  }
  return (int)v;
}

inline int unoHex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

inline bool parseUnoFrame(const char* line, UnoFrame& f) {
  if (!line || line[0] != '$') return false;
  const char* star = strrchr(line, '*');
  if (!star || strlen(star) != 3) return false;
  int h1 = unoHex(star[1]), h2 = unoHex(star[2]);
  if (h1 < 0 || h2 < 0) return false;

  const char* body = line + 1;
  size_t n = (size_t)(star - body);
  char b[72];
  if (n == 0 || n >= sizeof(b)) return false;

  uint8_t x = 0;
  for (size_t i = 0; i < n; i++) { b[i] = body[i]; x ^= (uint8_t)body[i]; }
  b[n] = 0;
  if (x != (uint8_t)(h1 * 16 + h2)) return false;

  char* fld[6];
  int cnt = 0;
  fld[cnt++] = b;
  for (char* p = b; *p; p++) {
    if (*p == ',') {
      *p = 0;
      if (cnt >= 6) return false;
      fld[cnt++] = p + 1;
    }
  }
  if (cnt != 6 || strcmp(fld[0], "GG") != 0) return false;

  int gas = unoNumber(fld[1]), thr = unoNumber(fld[2]), base = unoNumber(fld[3]);
  if (gas < 0 || thr < 0 || base < 0) return false;
  if (!unoStateValid(fld[4]) || !unoWarnValid(fld[5])) return false;

  f.gas = gas; f.threshold = thr; f.baseline = base;
  strcpy(f.state, fld[4]);
  strcpy(f.warn, fld[5]);
  return true;
}
