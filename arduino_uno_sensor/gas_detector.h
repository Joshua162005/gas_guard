// GasGuard detection logic. No Arduino calls in here, so it can be unit-tested on a PC.
//
// Why this exists: an MQ-2 is not a calibrated instrument. Its clean-air output moves with
// temperature, humidity, supply voltage and sensor age, so a fixed number ("alarm above 3250")
// is wrong sooner or later. This code instead:
//   1. learns the clean-air level (baseline) after warm-up,
//   2. alarms when the reading rises ABOVE that baseline by a set amount,
//   3. follows slow drift (hours) but never follows a leak (minutes),
//   4. needs several readings in a row to start and to clear an alarm (no flapping),
//   5. keeps a hard upper limit as a backstop if gas was already present at power-on.
//
// Values use whatever scale the caller picks (the Uno sketch uses the raw 0-1023 ADC scale; the
// defaults below are for 0-4095). Call update() once per second.
// Buffers are kept small (32 samples) so it fits in the Uno's 2 KB of RAM.
#pragma once
#include <stdint.h>
#include <math.h>

enum State { SENSOR_ERROR, WARMING_UP, CALIBRATING, NORMAL, ALERT };

struct DetectorConfig {
  uint32_t warmupMs         = 180000;   // heater warm-up after power-on, no alerts
  int      calSamples       = 30;       // seconds of clean air used to learn the baseline (max 32)
  float    alertDelta       = 400;      // alarm this far above baseline
  float    clearDelta       = 150;      // alarm clears below baseline + this
  float    absAlarm         = 3600;     // always alarm above this, whatever the baseline
  float    baselineMax      = 3200;     // the baseline is never allowed above this
  float    baselineWarnLow  = 250;      // report "baseline_low" below this
  float    baselineWarnHigh = 2600;     // report "baseline_high" above this (ADC is inaccurate up there)
  float    errLow           = 30;       // below this: sensor unplugged or no power
  float    errHigh          = 4050;     // above this: shorted
  int      onCount          = 5;        // readings in a row above the limit to start the alarm
  int      offCount         = 10;       // readings in a row below the clear level to stop it
  int      errCount         = 5;        // bad readings in a row to declare a sensor fault
  float    fastAlpha        = 0.02f;    // baseline follows a falling reading in ~50 s
  float    slowAlpha        = 1.0f / 600.0f;   // and a slowly rising one in ~10 min
  float    followDelta      = 250;      // ...as long as it is no more than this above the baseline
  float    calSpread        = 120;      // clean-air samples must stay within this band to be trusted
  uint32_t calTimeoutMs     = 600000;   // accept the best estimate after this long even if still noisy
};

class GasDetector {
 public:
  explicit GasDetector(const DetectorConfig& c) : cfg(c) {
    if (cfg.calSamples > 32) cfg.calSamples = 32;
    if (cfg.calSamples < 1)  cfg.calSamples = 1;
    warmUntil = c.warmupMs;
  }

  void update(float v, uint32_t nowMs) {
    // 1. Is the sensor even there?
    if (v < cfg.errLow || v > cfg.errHigh) {
      if (++errRun >= cfg.errCount) {
        if (st != SENSOR_ERROR) {                 // a fault means a cold start when it comes back
          calibrated = false; calCount = 0; calIdx = 0;
          warmUntil = nowMs + cfg.warmupMs;
        }
        st = SENSOR_ERROR; hi = lo = 0; alertOn = false;
      }
      return;
    }
    errRun = 0;

    // 2. Heater warm-up: readings are not meaningful yet.
    if ((int32_t)(nowMs - warmUntil) < 0) { st = WARMING_UP; hi = lo = 0; return; }

    // 3. Learn the clean-air baseline.
    // A sensor that is still settling (rising or falling) must not set the baseline: a baseline
    // learned while the reading is still creeping up makes the normal rise look like a leak.
    // So the last calSamples readings must stay inside a narrow band before they are trusted.
    if (!calibrated) {
      float level = cfg.absAlarm;                 // only the hard limit applies until learned
      float clearLevel = level - (cfg.alertDelta - cfg.clearDelta);
      evaluate(v, level, clearLevel);
      if (!alertOn && v < clearLevel && hi == 0) {
        if (calCount == 0) calStart = nowMs;
        cal[calIdx] = v;                          // never learn from a gassy reading
        calIdx = (calIdx + 1) % cfg.calSamples;
        if (calCount < cfg.calSamples) calCount++;
        if (calCount >= cfg.calSamples) {
          float mn = cal[0], mx = cal[0];
          for (int i = 1; i < calCount; i++) { if (cal[i] < mn) mn = cal[i]; if (cal[i] > mx) mx = cal[i]; }
          bool steady = (mx - mn) <= cfg.calSpread;
          bool tooLong = (uint32_t)(nowMs - calStart) >= cfg.calTimeoutMs;
          if (steady || tooLong) {
            baseVal = median(cal, calCount);
            if (baseVal > cfg.baselineMax) baseVal = cfg.baselineMax;
            calibrated = true;
          }
        }
      } else if (alertOn || v >= clearLevel) {
        calCount = 0; calIdx = 0;                 // gas around: start the learning again
      }
      st = alertOn ? ALERT : (calibrated ? NORMAL : CALIBRATING);
      return;
    }

    // 4. Normal operation.
    float level = alarmLevel();
    evaluate(v, level, level - (cfg.alertDelta - cfg.clearDelta));

    // The baseline follows drift, but is frozen while a leak builds up or is active.
    if (!alertOn && hi == 0) {
      if (v < baseVal)                          baseVal += (v - baseVal) * cfg.fastAlpha;
      else if (v <= baseVal + cfg.followDelta)  baseVal += (v - baseVal) * cfg.slowAlpha;
      if (baseVal > cfg.baselineMax) baseVal = cfg.baselineMax;
    }
    st = alertOn ? ALERT : NORMAL;
  }

  State state() const { return st; }
  bool  alert() const { return alertOn; }
  float baseline() const { return calibrated ? baseVal : 0; }
  float alarmLevel() const {
    if (!calibrated) return cfg.absAlarm;
    float l = baseVal + cfg.alertDelta;
    return l < cfg.absAlarm ? l : cfg.absAlarm;
  }
  const char* warn() const {
    if (!calibrated) return "";
    if (baseVal > cfg.baselineWarnHigh) return "baseline_high";
    if (baseVal < cfg.baselineWarnLow)  return "baseline_low";
    return "";
  }

 private:
  DetectorConfig cfg;
  State st = WARMING_UP;
  bool alertOn = false, calibrated = false;
  int hi = 0, lo = 0, errRun = 0, calCount = 0, calIdx = 0;
  float baseVal = 0;
  float cal[32];
  uint32_t warmUntil = 0, calStart = 0;

  void evaluate(float v, float level, float clearLevel) {
    if (v > level)            { hi++; lo = 0; }
    else if (v < clearLevel)  { lo++; hi = 0; }
    else                      { hi = 0; lo = 0; }   // in between: hold
    if (!alertOn && hi >= cfg.onCount)  alertOn = true;
    if (alertOn && lo >= cfg.offCount)  alertOn = false;
  }

  static float median(float* a, int n) {
    float t[32];
    for (int i = 0; i < n; i++) t[i] = a[i];
    for (int i = 1; i < n; i++) {                   // insertion sort
      float x = t[i]; int j = i - 1;
      while (j >= 0 && t[j] > x) { t[j + 1] = t[j]; j--; }
      t[j + 1] = x;
    }
    return (n & 1) ? t[n / 2] : (t[n / 2 - 1] + t[n / 2]) / 2;
  }
};
