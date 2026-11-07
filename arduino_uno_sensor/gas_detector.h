#pragma once
#include <stdint.h>
#include <math.h>

enum State { SENSOR_ERROR, CALIBRATING, NORMAL, ALERT };

struct DetectorConfig {
  int      calSamples       = 30;  // max 32
  float    alertDelta       = 400;
  float    clearDelta       = 150;
  float    absAlarm         = 3600;
  float    baselineMax      = 3200;
  float    baselineWarnLow  = 250;
  float    baselineWarnHigh = 2600;
  float    errLow           = 30;
  float    errHigh          = 4050;
  int      onCount          = 5;
  int      offCount         = 10;
  int      errCount         = 5;
  float    fastAlpha        = 0.02f;
  float    slowAlpha        = 1.0f / 600.0f;
  float    followDelta      = 250;
  float    calSpread        = 120;
  uint32_t calTimeoutMs     = 600000;
};

class GasDetector {
 public:
  explicit GasDetector(const DetectorConfig& c) : cfg(c) {
    if (cfg.calSamples > 32) cfg.calSamples = 32;
    if (cfg.calSamples < 1)  cfg.calSamples = 1;
  }

  void update(float v, uint32_t nowMs) {
    if (v < cfg.errLow || v > cfg.errHigh) {
      if (++errRun >= cfg.errCount) {
        if (st != SENSOR_ERROR) {
          calibrated = false; calCount = 0; calIdx = 0;
        }
        st = SENSOR_ERROR; hi = lo = 0; alertOn = false;
      }
      return;
    }
    errRun = 0;

    if (!calibrated) {
      float level = cfg.absAlarm;
      float clearLevel = level - (cfg.alertDelta - cfg.clearDelta);
      evaluate(v, level, clearLevel);
      if (!alertOn && v < clearLevel && hi == 0) {
        if (calCount == 0) calStart = nowMs;
        cal[calIdx] = v;
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
        calCount = 0; calIdx = 0;
      }
      st = alertOn ? ALERT : (calibrated ? NORMAL : CALIBRATING);
      return;
    }

    float level = alarmLevel();
    evaluate(v, level, level - (cfg.alertDelta - cfg.clearDelta));

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
  State st = CALIBRATING;
  bool alertOn = false, calibrated = false;
  int hi = 0, lo = 0, errRun = 0, calCount = 0, calIdx = 0;
  float baseVal = 0;
  float cal[32];
  uint32_t calStart = 0;

  void evaluate(float v, float level, float clearLevel) {
    if (v > level)            { hi++; lo = 0; }
    else if (v < clearLevel)  { lo++; hi = 0; }
    else                      { hi = 0; lo = 0; }
    if (!alertOn && hi >= cfg.onCount)  alertOn = true;
    if (alertOn && lo >= cfg.offCount)  alertOn = false;
  }

  static float median(float* a, int n) {
    float t[32];
    for (int i = 0; i < n; i++) t[i] = a[i];
    for (int i = 1; i < n; i++) {
      float x = t[i]; int j = i - 1;
      while (j >= 0 && t[j] > x) { t[j + 1] = t[j]; j--; }
      t[j + 1] = x;
    }
    return (n & 1) ? t[n / 2] : (t[n / 2 - 1] + t[n / 2]) / 2;
  }
};
