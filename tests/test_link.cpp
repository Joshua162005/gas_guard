// Run on a PC:  g++ -std=c++11 -O1 -o /tmp/tl tests/test_link.cpp && /tmp/tl
// Checks that what the Uno builds is exactly what the ESP32 accepts, and that damage is rejected.
#include "../arduino_uno_sensor/uno_frame.h"
#include "../esp32_gateway/uno_link.h"
#include <cstdio>
#include <cstring>
static int fails = 0;
#define CHECK(c,msg) do{ if(!(c)){printf("  FAIL: %s\n",msg);fails++;} else printf("  ok:   %s\n",msg);}while(0)

static bool roundTrip(int g, int t, int b, const char* st, const char* w, UnoFrame& f) {
  char out[96];
  int n = buildFrame(out, sizeof(out), g, t, b, st, w);
  if (n <= 0) return false;
  out[n - 1] = 0;                                   // the ESP32 sees the line without its newline
  return parseUnoFrame(out, f);
}

int main() {
  printf("L1 every state round-trips\n");
  const char* states[] = {"warming_up","calibrating","normal","alert","sensor_error"};
  bool all = true;
  for (auto st : states) { UnoFrame f; all &= roundTrip(1200, 2000, 1600, st, "", f) && !strcmp(f.state, st) && f.gas == 1200 && f.threshold == 2000 && f.baseline == 1600; }
  CHECK(all, "5 states parsed with the right numbers");

  printf("L2 warnings and extremes\n");
  { UnoFrame f; CHECK(roundTrip(4092, 3600, 3200, "normal", "baseline_high", f) && !strcmp(f.warn, "baseline_high") && f.gas == 4092, "baseline_high, max values"); }
  { UnoFrame f; CHECK(roundTrip(0, 0, 0, "sensor_error", "", f) && f.gas == 0 && f.warn[0] == 0, "all zero, empty warning"); }
  { UnoFrame f; CHECK(roundTrip(1000, 1400, 1000, "normal", "baseline_low", f) && !strcmp(f.warn, "baseline_low"), "baseline_low"); }

  printf("L3 damaged lines are rejected\n");
  char ok[96]; int n = buildFrame(ok, sizeof(ok), 1500, 2000, 1600, "alert", ""); ok[n - 1] = 0;
  { UnoFrame f; CHECK(parseUnoFrame(ok, f), "reference frame is valid"); }
  int rejected = 0, tried = 0;
  for (int i = 0; ok[i]; i++) {                      // flip one bit in every position: all must be rejected
    for (int bit = 0; bit < 7; bit++) {
      char bad[96]; strcpy(bad, ok); bad[i] ^= (1 << bit);
      if (bad[i] == 0 || bad[i] == '\n' || bad[i] == '\r') continue;
      UnoFrame f; tried++;
      if (!parseUnoFrame(bad, f)) rejected++;
    }
  }
  printf("  %d of %d single-bit corruptions rejected\n", rejected, tried);
  CHECK(rejected == tried, "no single-bit corruption gets through");
  { UnoFrame f; char t[96]; strncpy(t, ok, strlen(ok) - 1); t[strlen(ok) - 1] = 0; CHECK(!parseUnoFrame(t, f), "truncated line"); }
  { UnoFrame f; CHECK(!parseUnoFrame("", f) && !parseUnoFrame("hello", f) && !parseUnoFrame("$*00", f), "garbage / empty"); }
  { UnoFrame f; CHECK(!parseUnoFrame("$GG,1,2,3,normal,*00", f), "wrong checksum"); }
  { UnoFrame f; char b[96]; const char* body = "GG,9999,2,3,normal,"; uint8_t x = frameChecksum(body, strlen(body)); snprintf(b, sizeof b, "$%s*%02X", body, x); CHECK(!parseUnoFrame(b, f), "number out of range"); }
  { UnoFrame f; char b[96]; const char* body = "GG,1,2,3,exploding,"; uint8_t x = frameChecksum(body, strlen(body)); snprintf(b, sizeof b, "$%s*%02X", body, x); CHECK(!parseUnoFrame(b, f), "unknown state"); }
  { UnoFrame f; char b[96]; const char* body = "GG,1,2,3,normal"; uint8_t x = frameChecksum(body, strlen(body)); snprintf(b, sizeof b, "$%s*%02X", body, x); CHECK(!parseUnoFrame(b, f), "missing field"); }

  printf("L4 a bad frame must not change the previous good data\n");
  { UnoFrame f; roundTrip(1111, 2222, 1000, "normal", "", f); UnoFrame g = f; parseUnoFrame("$GG,1,2,3,alert,*00", g);
    CHECK(g.gas == 1111 && !strcmp(g.state, "normal"), "output untouched on failure"); }

  printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
  return fails ? 1 : 0;
}
