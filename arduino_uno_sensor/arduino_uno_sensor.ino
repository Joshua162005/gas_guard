#include <SoftwareSerial.h>
#include "gas_detector.h"
#include "uno_frame.h"

const uint8_t SENSOR_PIN  = A1;
const uint8_t LED_PIN     = 2;
const uint8_t BUZZER_PIN  = 3;
const int8_t  SILENCE_PIN = -1;
const uint8_t LINK_RX_PIN = 10;
const uint8_t LINK_TX_PIN = 11;

const float ALERT_DELTA     = 100;
const float CLEAR_DELTA     = 50;
const float ABS_ALARM       = 900;
const int   ALERT_ON_COUNT  = 3;
const int   ALERT_OFF_COUNT = 10;
const unsigned long SAMPLE_MS = 1000UL;
const int   SCALE = 4;

SoftwareSerial link(LINK_RX_PIN, LINK_TX_PIN);

DetectorConfig makeConfig() {
  DetectorConfig c;
  c.alertDelta = ALERT_DELTA;
  c.clearDelta = CLEAR_DELTA;
  c.absAlarm = ABS_ALARM;
  c.baselineMax = 800;
  c.baselineWarnLow = 62;
  c.baselineWarnHigh = 650;
  c.errLow = 8;
  c.errHigh = 1012;
  c.onCount = ALERT_ON_COUNT;
  c.offCount = ALERT_OFF_COUNT;
  c.followDelta = 62;
  c.calSpread = 30;
  return c;
}
GasDetector detector(makeConfig());

const char* stateName(State s) {
  switch (s) {
    case SENSOR_ERROR: return "sensor_error";
    case CALIBRATING:  return "calibrating";
    case NORMAL:       return "normal";
    case ALERT:        return "alert";
  }
  return "unknown";
}

float readGas() {
  int s[16];
  for (int i = 0; i < 16; i++) { s[i] = analogRead(SENSOR_PIN); delayMicroseconds(300); }
  for (int i = 1; i < 16; i++) {
    int x = s[i], j = i - 1;
    while (j >= 0 && s[j] > x) { s[j + 1] = s[j]; j--; }
    s[j + 1] = x;
  }
  long sum = 0;
  for (int i = 4; i < 12; i++) sum += s[i];
  return sum / 8.0f;
}

bool silenced = false;
void updateLocalAlarm(unsigned long now) {
  bool alarm = detector.alert();
  bool fault = detector.state() == SENSOR_ERROR;
  if (!alarm) silenced = false;
  if (SILENCE_PIN >= 0 && alarm && digitalRead(SILENCE_PIN) == LOW) silenced = true;
  bool beat = ((now / 250) % 2) == 0;
  digitalWrite(BUZZER_PIN, (alarm && !silenced) ? HIGH : LOW);
  digitalWrite(LED_PIN, alarm ? beat : (fault && ((now / 1000) % 2) == 0));
}

void setup() {
  Serial.begin(9600);
  link.begin(9600);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  if (SILENCE_PIN >= 0) pinMode(SILENCE_PIN, INPUT_PULLUP);
  Serial.println(F("GasGuard Uno started. Learning the clean-air level for about 30 s: keep gas away from the sensor."));
}

void loop() {
  static unsigned long lastSample = 0;
  static State lastState = CALIBRATING;
  unsigned long now = millis();

  if (now - lastSample >= SAMPLE_MS) {
    lastSample = now;
    float gas = readGas();
    detector.update(gas, now);
    State st = detector.state();
    int g = (int)(gas + 0.5f);
    int limit = (int)(detector.alarmLevel() + 0.5f);
    int base = (int)(detector.baseline() + 0.5f);

    char frame[72];
    int n = buildFrame(frame, sizeof(frame), g * SCALE, limit * SCALE, base * SCALE,
                       stateName(st), detector.warn());
    if (n > 0) link.write((const uint8_t*)frame, n);

    Serial.print(F("Gas: ")); Serial.print(g);
    Serial.print(F(" | clean air: ")); Serial.print(base);
    Serial.print(F(" | alarm at: ")); Serial.print(limit);
    Serial.print(F(" | ")); Serial.println(stateName(st));
    if (lastState == CALIBRATING && st == NORMAL) {
      Serial.print(F("Clean-air level learned: ")); Serial.println(base);
      if (detector.warn()[0]) Serial.println(F("Turn the trimmer on the MQ-2 so clean air is about 200-450, then press reset (README step 5)."));
    }
    lastState = st;
  }
  updateLocalAlarm(now);
}
