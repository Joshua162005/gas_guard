// GasGuard v3 - Arduino Uno: the SENSOR half (MQ-2 + local alarm)
//
// The Uno reads the MQ-2, decides whether there is a leak, and sounds the local alarm by itself.
// It works with no WiFi and no internet. Once a second it also sends the result to the ESP32
// (see ../esp32_gateway), which only forwards it to Firebase and your phone.
//
// Upload this sketch to the UNO. Open ONLY this folder in the Arduino IDE.
// No extra libraries are needed (SoftwareSerial is built in).
//
// WIRING (full diagram in README.md)
//   MQ-2 VCC -> 5V        MQ-2 GND -> GND        MQ-2 AO -> A1
//   LED (+ 220 ohm) -> pin 2       Buzzer (active, +) -> pin 3       Buzzer (-) -> GND
//   Optional "silence" button: pin 4 -> button -> GND (set SILENCE_PIN to -1 if you do not use one)
//   Link to ESP32:
//     Uno pin 11 (TX) -> 10k -> node -> ESP32 GPIO 16 (RX2);   node -> 10k + 10k in series -> GND
//     ESP32 GPIO 17 (TX2) -> Uno pin 10 (RX)   (direct; not used yet, kept for later)
//     Uno GND <-> ESP32 GND   (required)
//   Add a 470-1000 uF capacitor across 5V/GND next to the MQ-2.
//
// FIRST-TIME SETUP
//   1. Leave it powered in clean air for 10+ minutes (a brand-new MQ-2 needs hours, ideally 24 h).
//   2. About 30 seconds after power-on the Serial Monitor (9600) prints the clean-air level. On the
//      dashboard it is multiplied by 4. Turn the blue trimmer on the MQ-2 board until the clean-air level
//      is about 200-450 here (800-1800 on the dashboard), then press the Uno reset button.

#include <SoftwareSerial.h>
#include "gas_detector.h"
#include "uno_frame.h"

// ---- Pins -----------------------------------------------------------------------------
const uint8_t SENSOR_PIN  = A1;
const uint8_t LED_PIN     = 2;
const uint8_t BUZZER_PIN  = 3;
const int8_t  SILENCE_PIN = -1;       // e.g. 4 for a push button to GND, -1 = none
const uint8_t LINK_RX_PIN = 10;       // from ESP32 TX2 (not used yet)
const uint8_t LINK_TX_PIN = 11;       // to ESP32 RX2, through the 10k + 20k divider

// ---- Detection tuning (raw 0-1023 ADC scale; the dashboard shows these x 4) ------------
const float ALERT_DELTA     = 100;    // alarm this far above the learned clean-air level (= 400 on the dashboard)
const float CLEAR_DELTA     = 50;     // alarm clears below clean air + 50
const float ABS_ALARM       = 900;    // always alarms above this (backstop if gas was there at power-on)
const int   ALERT_ON_COUNT  = 3;      // seconds in a row above the limit (clean Uno ADC: 3 is enough)
const int   ALERT_OFF_COUNT = 10;     // seconds in a row below the clear level
const unsigned long SAMPLE_MS = 1000UL;
const int   SCALE = 4;                // 0-1023 -> 0-4092 for the web app

SoftwareSerial link(LINK_RX_PIN, LINK_TX_PIN);

DetectorConfig makeConfig() {
  DetectorConfig c;
  c.alertDelta = ALERT_DELTA;
  c.clearDelta = CLEAR_DELTA;
  c.absAlarm = ABS_ALARM;
  c.baselineMax = 800;
  c.baselineWarnLow = 62;
  c.baselineWarnHigh = 650;           // above this the Uno ADC is not the problem, but the sensor has little headroom
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

// 16 quick readings, drop the 4 lowest and 4 highest, average the rest: removes spikes.
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
  if (!alarm) silenced = false;                       // rings again on the next leak
  if (SILENCE_PIN >= 0 && alarm && digitalRead(SILENCE_PIN) == LOW) silenced = true;
  bool beat = ((now / 250) % 2) == 0;                 // 2 beeps per second
  digitalWrite(BUZZER_PIN, (alarm && !silenced && beat) ? HIGH : LOW);
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
      if (detector.warn()[0]) Serial.println(F("Turn the trimmer on the MQ-2 so clean air is about 200-450, then press reset (see top of this file)."));
    }
    lastState = st;
  }
  updateLocalAlarm(now);
}
