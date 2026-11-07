// GasGuard - ESP32 + MQ-2  (no extra libraries needed)
// Wiring: MQ-2 VCC -> ESP32 5V (VIN), GND -> GND,
//         AO -> voltage divider (10k to AO, 20k to GND) -> GPIO 34
//         (MQ-2 AO can reach 5V, but the ESP32 pin tolerates only 3.3V)
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

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

const int MQ2_PIN = 34;
const unsigned long ALERT_GAP = 60000;   // one phone push per minute at most

// Fixed leak limit (0-4095). On this sensor clean air reads about 3000-3150 and methane 3600+.
// More sensitive: lower THRESHOLD.  Fewer false alarms: raise it.
const int THRESHOLD       = 3250;
const int CLEAR_MARGIN    = 100;          // alarm clears when the reading drops below THRESHOLD - 100
const int ALERT_ON_COUNT  = 2;            // readings in a row above the limit (5 s apart)
const int ALERT_OFF_COUNT = 3;            // readings in a row below the clear level
const unsigned long WARMUP_MS = 30000UL; // no alerts while the heater warms up after power-on
const unsigned long SEND_INTERVAL_MS = 5000UL;
const unsigned long RELOGIN_MS = 3000000UL; // Firebase tokens last 60 min, refresh at 50

enum State { SENSOR_ERROR, WARMING_UP, NORMAL, ALERT };

String idToken = "";
unsigned long lastLogin = 0, lastSend = 0, lastAlert = 0;
State state = WARMING_UP;
int  highCount = 0, lowCount = 0;
bool alertActive = false;

const char* stateName(State s) {
  switch (s) {
    case SENSOR_ERROR: return "sensor_error";
    case WARMING_UP:   return "warming_up";
    case NORMAL:       return "normal";
    case ALERT:        return "alert";
  }
  return "unknown";
}

int readGasAverage() {
  long sum = 0;
  for (int i = 0; i < 10; i++) { sum += analogRead(MQ2_PIN); delay(5); }
  return (int)(sum / 10);
}

// A disconnected or shorted sensor reads about 0 or about 4095.
bool sensorLooksValid(int v) { return v > 20 && v < 4090; }

void updateState(int gas) {
  if (!sensorLooksValid(gas)) {            // no sensor: never alarm
    state = SENSOR_ERROR;
    highCount = lowCount = 0;
    alertActive = false;
    return;
  }
  if (millis() < WARMUP_MS) { state = WARMING_UP; return; }

  if (gas > THRESHOLD)                      { highCount++; lowCount = 0; }
  else if (gas < THRESHOLD - CLEAR_MARGIN)  { lowCount++;  highCount = 0; }
  else                                      { highCount = 0; lowCount = 0; }   // in between: hold

  if (!alertActive && highCount >= ALERT_ON_COUNT) alertActive = true;
  if (alertActive  && lowCount  >= ALERT_OFF_COUNT) alertActive = false;

  state = alertActive ? ALERT : NORMAL;
}

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

bool firebaseLogin() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
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

void sendReading(int gas) {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  String url = String(DB_URL) + "/devices/" + DEVICE_ID + ".json?auth=" + idToken;
  // The ESP32 decides the state; the web app only displays it.
  // ".sv":"timestamp" makes Firebase stamp the time on its server
  String body = String("{\"gas\":") + gas +
                ",\"threshold\":" + THRESHOLD +
                ",\"state\":\"" + stateName(state) + "\"" +
                ",\"sensorOk\":" + (state != SENSOR_ERROR ? "true" : "false") +
                ",\"warmingUp\":" + (state == WARMING_UP ? "true" : "false") +
                ",\"alert\":" + (state == ALERT ? "true" : "false") +
                ",\"updated\":{\".sv\":\"timestamp\"}}";
  if (!http.begin(client, url)) return;
  http.addHeader("Content-Type", "application/json");
  int code = http.PUT(body);
  Serial.printf("Firebase: %d\n", code);
  if (code == 401) idToken = "";          // token rejected: log in again next loop
  http.end();
}

void sendPush() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, String("https://ntfy.sh/") + NTFY_TOPIC);
  http.addHeader("Title", "GAS LEAK DETECTED!");
  http.addHeader("Priority", "urgent");
  http.addHeader("Tags", "rotating_light");
  int code = http.POST("Open windows and turn off your LPG tank now.");
  Serial.printf("ntfy: %d\n", code);       // 200 = push sent
  http.end();
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetPinAttenuation(MQ2_PIN, ADC_11db);   // full 0-3.3 V range
  pinMode(MQ2_PIN, INPUT);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_11dBm);            // lower current peaks, helps with brownouts
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi connected");
  Serial.println("Warming up MQ-2 (2 min). Alerts start after that.");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); delay(1000); return; }
  if (idToken.length() == 0 || millis() - lastLogin > RELOGIN_MS) {
    if (!firebaseLogin()) { delay(5000); return; }
  }
  if (millis() - lastSend < SEND_INTERVAL_MS) return;
  lastSend = millis();

  int gas = readGasAverage();
  updateState(gas);
  Serial.printf("Gas: %d | limit: %d | %s\n", gas, THRESHOLD, stateName(state));

  sendReading(gas);
  if (state == ALERT && (lastAlert == 0 || millis() - lastAlert > ALERT_GAP)) {
    sendPush();
    lastAlert = millis();
  }
}
