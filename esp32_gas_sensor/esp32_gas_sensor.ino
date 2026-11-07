// GasGuard v2 - ESP32 + MQ-2  (no extra libraries needed)
//
// WIRING
//   MQ-2 VCC -> ESP32 VIN (5V)         MQ-2 GND -> ESP32 GND
//   MQ-2 AO  -> 10k -> GPIO 34,  and GPIO 34 -> 20k -> GND   (divider: the pin only tolerates 3.3V)
//   Optional local alarm (works with no WiFi): buzzer -> GPIO 26, LED + 220 ohm -> GPIO 27
//   Add a 470-1000 uF capacitor across 5V/GND close to the sensor. The MQ-2 heater and the WiFi
//   radio both pull current in bursts; without it the ESP32 can brown-out and reset at random.
//
// FIRST-TIME SETUP (this is what makes it accurate)
//   1. Let the sensor run 10+ minutes in clean air (a fresh sensor needs hours, ideally 24 h).
//   2. Watch the Serial Monitor (115200). It prints the clean-air level ("baseline").
//   3. Turn the blue trimmer on the MQ-2 board until the baseline is about 800-1800.
//      A baseline above ~2600 sits in the part of the ESP32 ADC that is badly compressed, so
//      gas barely changes the reading. The old sketch ran at 3000-3150, which is why it was
//      inaccurate. Reboot after turning the trimmer so the level is learned again.
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_system.h>
#include "gas_detector.h"

const char* WIFI_SSID = "HUERTE WIFI";
const char* WIFI_PASS = "sfvigv7580";

// Firebase Realtime Database config (copy these values from the Firebase console)
const char* DB_URL    = "https://gasguard-0950-default-rtdb.firebaseio.com"; // Realtime Database URL
const char* API_KEY  = "AIzaSyCn-B4xQGRisNe4U0WDOs6q2fvUAKf9CtE";                  // Web API key (same as in app.js, not secret)
const char* DEVICE_EMAIL    = "gasguard1@gmail.com";                                 // Authentication > Users > Add user (a dedicated account for the ESP32)
const char* DEVICE_PASSWORD = "gasguard1";                              // avoid " and \ characters in this password
const char* DEVICE_ID = "gas-001";                                           // type the same ID in the web app

// Free phone push: install the "ntfy" app, subscribe to this topic (make it unique!)
const char* NTFY_TOPIC = "gasguard-joshua-change-me";

const int MQ2_PIN    = 34;   // must be an ADC1 pin (32-39). ADC2 stops working while WiFi is on.
const int BUZZER_PIN = 26;   // local alarm, works without WiFi. Set to -1 if you do not use one.
const int LED_PIN    = 27;   // set to -1 if you do not use one.

// ---- Detection tuning (0-4095 scale, see gas_detector.h) -------------------------------
// The alarm level is the learned clean-air level + ALERT_DELTA, so it follows your sensor
// instead of being a fixed number.  More sensitive: lower ALERT_DELTA.  Fewer false alarms: raise it.
const float ALERT_DELTA       = 300;
const float CLEAR_DELTA       = 150;      // alarm clears below clean air + 150
const float ABS_ALARM         = 3600;     // hard limit, always alarms (backstop)
const int   ALERT_ON_COUNT    = 3;        // seconds in a row above the limit
const int   ALERT_OFF_COUNT   = 10;       // seconds in a row below the clear level
const unsigned long WARMUP_MS = 120000UL; // no alerts while the heater warms up after power-on

// ---- Timing / network -----------------------------------------------------------------
const unsigned long SAMPLE_MS        = 1000UL;     // sensor is read every second, independent of WiFi
const unsigned long SEND_INTERVAL_MS = 5000UL;     // Firebase update (also sent at once when the state changes)
const unsigned long PUSH_REPEAT_MS   = 60000UL;    // phone push repeats once a minute during a leak
const unsigned long RELOGIN_MS       = 3000000UL;  // Firebase tokens last 60 min, refresh at 50
const unsigned long HTTP_TIMEOUT_MS  = 4000UL;
const unsigned long WIFI_RESTART_MS  = 1800000UL;  // reboot if WiFi is gone 30 min (never during a leak)
const unsigned long SENSOR_FAULT_PUSH_MS = 120000UL;

// ---- Shared between the sensor loop (core 1) and the network task (core 0) --------------
struct Snapshot {
  int gas = 0, threshold = 0, baseline = 0;
  State state = WARMING_UP;
  const char* warn = "";
  uint32_t seq = 0;
};
Snapshot snap;
SemaphoreHandle_t snapLock;

GasDetector* detector;

const char* stateName(State s) {
  switch (s) {
    case SENSOR_ERROR: return "sensor_error";
    case WARMING_UP:   return "warming_up";
    case CALIBRATING:  return "calibrating";
    case NORMAL:       return "normal";
    case ALERT:        return "alert";
  }
  return "unknown";
}

// ===== Sensor =============================================================================
// Why not plain analogRead(): the ESP32 ADC is noisy and non-linear. We take 32 readings in
// calibrated millivolts, drop the 8 lowest and 8 highest (spikes from the WiFi radio), and
// average the rest. The result is mapped to 0-4095 so the web app's gauge still works.
float readGas() {
  const int N = 32;
  uint16_t s[N];
  for (int i = 0; i < N; i++) { s[i] = analogReadMilliVolts(MQ2_PIN); delayMicroseconds(400); }
  for (int i = 1; i < N; i++) {
    uint16_t x = s[i]; int j = i - 1;
    while (j >= 0 && s[j] > x) { s[j + 1] = s[j]; j--; }
    s[j + 1] = x;
  }
  long sum = 0;
  for (int i = N / 4; i < N - N / 4; i++) sum += s[i];
  float mv = (float)sum / (N - 2 * (N / 4));
  return mv * (4095.0f / 3300.0f);
}

void updateLocalAlarm(uint32_t now) {
  bool beat = ((now / 250) % 2) == 0;                       // 2 beeps per second
  bool alarm = detector->alert();
  bool fault = detector->state() == SENSOR_ERROR;
  if (BUZZER_PIN >= 0) digitalWrite(BUZZER_PIN, alarm && beat);
  if (LED_PIN >= 0)    digitalWrite(LED_PIN, alarm ? beat : (fault && ((now / 1000) % 2) == 0));
}

// ===== Network (runs on its own core, so a slow WiFi never delays the alarm) ================
String idToken = "";
WiFiClientSecure fbClient;                 // kept open between updates: much faster and lighter than a new TLS handshake each time
unsigned long lastLogin = 0, lastSend = 0, nextLoginTry = 0, nextWifiKick = 0, wifiLostAt = 0;
unsigned long nextPushAt = 0, sensorFaultSince = 0;
bool alertWasOn = false, faultPushed = false;
State lastSentState = WARMING_UP;
int failStreak = 0;

// Tiny JSON helper so no extra library is needed: returns the string value of "key".
String jsonString(const String& json, const char* key) {
  String pat = String("\"") + key + "\"";
  int k = json.indexOf(pat);
  if (k < 0) return "";
  int colon = json.indexOf(':', k + pat.length());
  if (colon < 0) return "";
  int s = json.indexOf('"', colon + 1);
  if (s < 0) return "";
  int e = json.indexOf('"', s + 1);
  if (e < 0) return "";
  return json.substring(s + 1, e);
}

Snapshot getSnapshot() {
  Snapshot c;
  xSemaphoreTake(snapLock, portMAX_DELAY);
  c = snap;
  xSemaphoreGive(snapLock);
  return c;
}

bool wifiOk() {
  if (WiFi.status() == WL_CONNECTED) { wifiLostAt = 0; return true; }
  unsigned long now = millis();
  if (!wifiLostAt) wifiLostAt = now;
  if (now - nextWifiKick > 15000UL) {      // WiFi.begin again if the router did not take us back
    nextWifiKick = now;
    Serial.println("WiFi down, reconnecting...");
    WiFi.disconnect(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
  if (now - wifiLostAt > WIFI_RESTART_MS && getSnapshot().state != ALERT) ESP.restart();
  return false;
}

bool firebaseLogin() {
  WiFiClientSecure client; client.setInsecure();   // NOTE: skips certificate checks. Fine for a hobby project, see README.
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + API_KEY;
  String body = String("{\"email\":\"") + DEVICE_EMAIL + "\",\"password\":\"" + DEVICE_PASSWORD +
                "\",\"returnSecureToken\":true}";
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  String resp = http.getString();
  http.end();
  if (code != 200) {
    Serial.printf("Firebase login failed: %d\n", code);
    Serial.println(resp);
    return false;
  }
  idToken = jsonString(resp, "idToken");
  lastLogin = millis();
  Serial.println(idToken.length() ? "Firebase login OK" : "Login reply had no idToken");
  return idToken.length() > 0;
}

// Returns true if Firebase accepted the reading.
bool sendReading(const Snapshot& s) {
  char body[420];
  snprintf(body, sizeof(body),
    "{\"gas\":%d,\"threshold\":%d,\"baseline\":%d,\"state\":\"%s\",\"sensorOk\":%s,\"warmingUp\":%s,"
    "\"alert\":%s,\"warn\":\"%s\",\"rssi\":%d,\"uptime\":%lu,\"fw\":2,\"updated\":{\".sv\":\"timestamp\"}}",
    s.gas, s.threshold, s.baseline, stateName(s.state),
    s.state != SENSOR_ERROR ? "true" : "false",
    s.state == WARMING_UP ? "true" : "false",
    s.state == ALERT ? "true" : "false",
    s.warn, WiFi.RSSI(), millis() / 1000UL);

  String url = String(DB_URL) + "/devices/" + DEVICE_ID + ".json?auth=" + idToken + "&print=silent";
  for (int attempt = 0; attempt < 2; attempt++) {
    HTTPClient http;
    http.setReuse(true);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(fbClient, url)) { fbClient.stop(); continue; }
    http.addHeader("Content-Type", "application/json");
    int code = http.PUT((uint8_t*)body, strlen(body));
    String resp = (code > 0 && code != 200 && code != 204) ? http.getString() : String("");
    http.end();
    if (code == 200 || code == 204) { failStreak = 0; return true; }
    if (code == 401 || code == 403) {
      if (resp.indexOf("ermission denied") >= 0) {
        // The login worked but the database rules refuse the write. Logging in again will not fix it.
        Serial.println("Firebase: PERMISSION DENIED. Publish database.rules.json in the Firebase console.");
        nextLoginTry = millis() + 30000UL;
      } else {
        Serial.println("Firebase: token rejected, logging in again");
        idToken = "";
      }
      return false;
    }
    Serial.printf("Firebase send failed: %d (attempt %d)\n", code, attempt + 1);
    fbClient.stop();                        // connection went stale: drop it and retry once on a fresh one
  }
  failStreak++;
  if (failStreak >= 6) {                    // 6 failed rounds in a row: restart the WiFi link
    failStreak = 0;
    Serial.println("Too many failures, restarting WiFi");
    WiFi.disconnect(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
  return false;
}

bool sendPush(const char* title, const char* msg, const char* priority, const char* tags) {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, String("https://ntfy.sh/") + NTFY_TOPIC)) return false;
  http.addHeader("Title", title);
  http.addHeader("Priority", priority);
  http.addHeader("Tags", tags);
  int code = http.POST(msg);
  Serial.printf("ntfy: %d\n", code);         // 200 = push sent
  http.end();
  return code == 200;
}

void handlePushes(const Snapshot& s) {
  unsigned long now = millis();
  if (s.state == ALERT) {
    if (!alertWasOn) { alertWasOn = true; nextPushAt = now; }
    if ((long)(now - nextPushAt) >= 0) {
      char msg[120];
      snprintf(msg, sizeof(msg), "Gas level %d (limit %d). Open windows and turn off your LPG tank now.", s.gas, s.threshold);
      bool ok = sendPush("GAS LEAK DETECTED!", msg, "urgent", "rotating_light");
      nextPushAt = now + (ok ? PUSH_REPEAT_MS : 10000UL);   // a failed push is retried after 10 s, not a minute
    }
  } else if (alertWasOn) {
    alertWasOn = false;
    sendPush("Gas level back to normal", "The reading dropped below the leak limit.", "default", "white_check_mark");
  }

  // A dead sensor means no protection at all, so say so.
  if (s.state == SENSOR_ERROR) {
    if (!sensorFaultSince) sensorFaultSince = now;
    if (!faultPushed && now - sensorFaultSince > SENSOR_FAULT_PUSH_MS) {
      faultPushed = sendPush("GasGuard: sensor not working", "The MQ-2 is not detected. Check its wiring and power. You are NOT protected.", "high", "warning");
    }
  } else { sensorFaultSince = 0; faultPushed = false; }
}

void netLoop() {
  if (!wifiOk()) return;
  unsigned long now = millis();

  if (idToken.length() == 0 || now - lastLogin > RELOGIN_MS) {
    if ((long)(now - nextLoginTry) < 0) return;
    if (!firebaseLogin()) { nextLoginTry = now + 10000UL; return; }
  }

  Snapshot s = getSnapshot();
  if (s.state != lastSentState || now - lastSend >= SEND_INTERVAL_MS) {
    if ((long)(now - nextLoginTry) >= 0) {
      bool ok = sendReading(s);
      Serial.printf("Firebase: %s\n", ok ? "ok" : "failed");
      if (ok) { lastSentState = s.state; lastSend = now; }
      else    { lastSend = now - SEND_INTERVAL_MS + 2000UL; }    // retry in 2 s
    }
  }
  handlePushes(s);
}

void netTask(void*) {
  for (;;) { netLoop(); vTaskDelay(pdMS_TO_TICKS(200)); }
}

// ===== Arduino entry points ================================================================
void setup() {
  Serial.begin(115200);
  if (BUZZER_PIN >= 0) { pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW); }
  if (LED_PIN >= 0)    { pinMode(LED_PIN, OUTPUT);    digitalWrite(LED_PIN, LOW); }

  analogReadResolution(12);
  analogSetPinAttenuation(MQ2_PIN, ADC_11db);   // full 0-3.3 V range
  pinMode(MQ2_PIN, INPUT);

  DetectorConfig cfg;
  cfg.alertDelta = ALERT_DELTA;
  cfg.clearDelta = CLEAR_DELTA;
  cfg.absAlarm   = ABS_ALARM;
  cfg.onCount    = ALERT_ON_COUNT;
  cfg.offCount   = ALERT_OFF_COUNT;
  cfg.warmupMs   = WARMUP_MS;
  // After a software restart the heater never lost power, so it is already warm.
  if (esp_reset_reason() == ESP_RST_SW) cfg.warmupMs = 20000UL;
  detector = new GasDetector(cfg);
  snapLock = xSemaphoreCreateMutex();
  fbClient.setInsecure();                       // the long-lived Firebase connection (see the note in firebaseLogin)

  // WiFi is started here but NOT waited for: sensing and the local alarm must work without it.
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);                         // modem sleep causes dropped connections
  WiFi.setTxPower(WIFI_POWER_15dBm);            // 11 dBm was too weak for reliable range; lower it again only if you still get brown-outs
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  xTaskCreatePinnedToCore(netTask, "net", 16384, NULL, 1, NULL, 0);
  Serial.printf("GasGuard v2 started. Warming up the MQ-2 for %lu s, then learning clean air for 30 s.\n", cfg.warmupMs / 1000UL);
}

void loop() {
  static unsigned long lastSample = 0;
  static State lastState = WARMING_UP;
  unsigned long now = millis();

  if (now - lastSample >= SAMPLE_MS) {
    lastSample = now;
    float gas = readGas();
    detector->update(gas, now);
    State st = detector->state();

    xSemaphoreTake(snapLock, portMAX_DELAY);
    snap.gas = (int)(gas + 0.5f);
    snap.threshold = (int)(detector->alarmLevel() + 0.5f);
    snap.baseline = (int)(detector->baseline() + 0.5f);
    snap.state = st;
    snap.warn = detector->warn();
    snap.seq++;
    xSemaphoreGive(snapLock);

    Serial.printf("Gas: %4d | clean air: %4d | alarm at: %4d | %s%s%s\n",
                  (int)(gas + 0.5f), (int)(detector->baseline() + 0.5f), (int)(detector->alarmLevel() + 0.5f),
                  stateName(st), detector->warn()[0] ? " | " : "", detector->warn());
    if (lastState == CALIBRATING && st == NORMAL) {
      Serial.printf("Clean-air level learned: %d.\n", (int)(detector->baseline() + 0.5f));
      if (detector->warn()[0]) Serial.println("Turn the trimmer on the MQ-2 so the clean-air level is about 800-1800, then reboot (see top of this file).");
    }
    lastState = st;
  }
  updateLocalAlarm(now);
  delay(10);
}
