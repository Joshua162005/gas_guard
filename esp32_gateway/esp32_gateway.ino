// GasGuard v3 - ESP32 GATEWAY (the online half: WiFi -> Firebase + phone push)
//
// The ESP32 does NOT read the gas sensor any more. The Arduino Uno (see ../arduino_uno_sensor)
// reads the MQ-2, decides about leaks and sounds the local alarm. Once a second it sends the
// result over a 3-wire serial link. This sketch checks each message, uploads it to Firebase for the
// web app and sends ntfy pushes. If WiFi or the internet is down, the Uno's alarm still works.
//
// Upload this sketch to the ESP32. Open ONLY this folder in the Arduino IDE
// (board "ESP32 Dev Module"). No extra libraries are needed.
//
// WIRING (full diagram in README.md)
//   Uno pin 11 (TX) -> 10k -> node -> ESP32 GPIO 16 (RX2);   node -> 10k + 10k in series -> GND
//   ESP32 GPIO 17 (TX2) -> Uno pin 10 (RX)    (direct; not used yet, kept for later)
//   Uno GND <-> ESP32 GND                     (required)
//   The divider is required: the Uno sends 5V and the ESP32 pin only tolerates 3.3V.
//   Power both boards from one good 5V supply (1-2 A) with the grounds joined.
//
// If a WROVER module is used, GPIO 16/17 are taken by its PSRAM: change LINK_RX_PIN/LINK_TX_PIN.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_system.h>
#include "uno_link.h"

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

// ---- Link to the Uno ----------------------------------------------------------------------
const int           LINK_RX_PIN     = 16;       // RX2  <- Uno pin 11 through the 10k + 20k divider
const int           LINK_TX_PIN     = 17;       // TX2  -> Uno pin 10 (not used yet)
const unsigned long LINK_BAUD       = 9600;
const unsigned long LINK_TIMEOUT_MS = 10000UL;  // no valid message for 10 s = Uno link lost

// ---- Timing / network ---------------------------------------------------------------------
const unsigned long SEND_INTERVAL_MS = 5000UL;     // Firebase update (also sent at once when the state changes)
const unsigned long PUSH_REPEAT_MS   = 60000UL;    // phone push repeats once a minute during a leak
const unsigned long RELOGIN_MS       = 3000000UL;  // Firebase tokens last 60 min, refresh at 50
const unsigned long HTTP_TIMEOUT_MS  = 4000UL;
const unsigned long WIFI_RESTART_MS  = 1800000UL;  // reboot if WiFi is gone 30 min (never during a leak)
const unsigned long SENSOR_FAULT_PUSH_MS = 120000UL;

// ---- Latest message from the Uno, shared between loop() (core 1) and the network task (core 0)
struct Snapshot {
  int  gas = 0, threshold = 0, baseline = 0;
  char state[16] = "warming_up";
  char warn[16]  = "";
  bool hasFrame = false;                 // at least one valid message received since boot
  unsigned long lastFrameMs = 0;
};
Snapshot snap;
SemaphoreHandle_t snapLock;

Snapshot getSnapshot() {
  Snapshot c;
  xSemaphoreTake(snapLock, portMAX_DELAY);
  c = snap;
  xSemaphoreGive(snapLock);
  return c;
}
bool isAlert(const Snapshot& s)       { return strcmp(s.state, "alert") == 0; }
bool isSensorError(const Snapshot& s) { return strcmp(s.state, "sensor_error") == 0; }
bool linkLost(const Snapshot& s, unsigned long now) {
  return !s.hasFrame || (now - s.lastFrameMs) > LINK_TIMEOUT_MS;
}

// ===== Reading the Uno (non-blocking, called from loop) =====================================
unsigned long badFrames = 0;
void readUnoLink() {
  static char line[80];
  static size_t n = 0;
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '\n' || c == '\r') {
      if (n == 0) continue;
      line[n] = 0;
      n = 0;
      UnoFrame f;
      if (parseUnoFrame(line, f)) {
        xSemaphoreTake(snapLock, portMAX_DELAY);
        snap.gas = f.gas; snap.threshold = f.threshold; snap.baseline = f.baseline;
        strcpy(snap.state, f.state); strcpy(snap.warn, f.warn);
        snap.hasFrame = true; snap.lastFrameMs = millis();
        xSemaphoreGive(snapLock);
        Serial.printf("Uno: gas %4d | clean air %4d | alarm at %4d | %s%s%s\n",
                      f.gas, f.baseline, f.threshold, f.state, f.warn[0] ? " | " : "", f.warn);
      } else {
        badFrames++;
        Serial.printf("Uno link: ignored a damaged message (%lu so far)\n", badFrames);
      }
    } else if (n < sizeof(line) - 1) {
      line[n++] = c;
    } else {
      n = 0;                              // too long to be ours: start over
    }
  }
}

// ===== Network (runs on its own core, so a slow WiFi never blocks reading the Uno) ==========
String idToken = "";
WiFiClientSecure fbClient;                 // kept open between updates: much faster and lighter than a new TLS handshake each time
unsigned long lastLogin = 0, lastSend = 0, nextLoginTry = 0, nextWifiKick = 0, wifiLostAt = 0;
unsigned long nextPushAt = 0, sensorFaultSince = 0;
bool alertWasOn = false, faultPushed = false;
char lastSentState[16] = "";
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
  if (now - wifiLostAt > WIFI_RESTART_MS && !isAlert(getSnapshot())) ESP.restart();
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
    "\"alert\":%s,\"warn\":\"%s\",\"rssi\":%d,\"uptime\":%lu,\"fw\":3,\"updated\":{\".sv\":\"timestamp\"}}",
    s.gas, s.threshold, s.baseline, s.state,
    isSensorError(s) ? "false" : "true",
    strcmp(s.state, "warming_up") == 0 ? "true" : "false",
    isAlert(s) ? "true" : "false",
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

  // No messages from the Uno: we do not know the air quality. The Uno still sounds its own alarm.
  // Do not announce "back to normal" here: silence is not proof that a leak is over.
  if (linkLost(s, now)) {
    if (!sensorFaultSince) sensorFaultSince = now;
    if (!faultPushed && now - sensorFaultSince > SENSOR_FAULT_PUSH_MS) {
      faultPushed = sendPush("GasGuard: sensor not reporting",
                             "The ESP32 is not receiving data from the Arduino Uno. Check the 3 link wires and the Uno power. The Uno alarm still works locally.",
                             "high", "warning");
    }
    return;
  }

  if (isAlert(s)) {
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

  // A dead MQ-2 means no protection at all, so say so.
  if (isSensorError(s)) {
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
  // Only upload live data. With the link down nothing is sent, so the web app shows "Sensor offline"
  // (or keeps a leak alarm on if the last report was a leak) instead of an old reading.
  if (!linkLost(s, now) && (strcmp(s.state, lastSentState) != 0 || now - lastSend >= SEND_INTERVAL_MS)) {
    if ((long)(now - nextLoginTry) >= 0) {
      bool ok = sendReading(s);
      Serial.printf("Firebase: %s\n", ok ? "ok" : "failed");
      if (ok) { strcpy(lastSentState, s.state); lastSend = now; }
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
  Serial2.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
  snapLock = xSemaphoreCreateMutex();
  fbClient.setInsecure();                       // the long-lived Firebase connection (see the note in firebaseLogin)

  // WiFi is started here but NOT waited for: reading the Uno must never wait for the router.
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);                         // modem sleep causes dropped connections
  WiFi.setTxPower(WIFI_POWER_15dBm);            // lower it again only if you still get brown-outs
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  xTaskCreatePinnedToCore(netTask, "net", 16384, NULL, 1, NULL, 0);
  Serial.println("GasGuard v3 gateway started. Waiting for data from the Arduino Uno...");
}

void loop() {
  readUnoLink();
  delay(10);
}
