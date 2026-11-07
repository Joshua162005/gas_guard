# GasGuard setup (all free)

Two boards, each with one job:

```
MQ-2 --> ARDUINO UNO --(3 wires)--> ESP32 --WiFi--> Firebase --> Web app (login, live gauge, alarm)
          |                          |
          +-> buzzer + LED + LCD     +-> ntfy.sh --> push notification on your phone, even when the app is closed
              (local alarm, works with no WiFi and no internet)
```

- **Arduino Uno** (`arduino_uno_sensor/`): reads the MQ-2 (the Uno's ADC is clean and linear), decides about
  leaks, sounds the buzzer/LED and shows the value on the LCD. Once a second it sends the result to the ESP32.
- **ESP32** (`esp32_gateway/`): does no sensing. It checks each message from the Uno, uploads it to Firebase and
  sends the phone push. If it loses WiFi, the Uno alarm still works.

The two sketches live in **separate folders**. Open and upload each one on its own: each folder has everything it
needs, and the Arduino IDE only compiles the folder you opened, so nothing collides.

## 0. Wiring

**Uno (sensor side)**

| Part | Uno pin | Note |
|---|---|---|
| MQ-2 VCC / GND | 5V / GND | |
| MQ-2 AO | A1 | direct, no divider needed on the Uno |
| LED (+ 220 ohm) | D2 | long leg through the resistor |
| Active buzzer (+) | D3 | other leg to GND |
| LCD I2C (SDA / SCL) | A4 / A5 | VCC to 5V, GND to GND. Address `0x27` (change in the sketch if yours differs) |
| Silence button (optional) | D4 -> button -> GND | set `SILENCE_PIN = 4` in the sketch to enable |

**Link between the boards (3 wires + a divider)**

| From | To | Note |
|---|---|---|
| Uno D11 (SoftwareSerial TX) | 10k resistor, then the **node** | Uno sends 5V |
| the **node** | ESP32 GPIO 16 (RX2) | |
| the **node** | 10k + 10k in series (= 20k) -> GND | divider: 5V x 20k / 30k = 3.3V |
| ESP32 GPIO 17 (TX2) | Uno D10 (SoftwareSerial RX) | direct, no resistor. Not used yet, wired for later |
| Uno GND | ESP32 GND | **required**, the boards must share ground |

```
            ARDUINO UNO                                         ESP32
        +------------------+                              +--------------+
 MQ-2 AO| A1               |                              |              |
 LED    | D2               |                              |              |
 BUZZER | D3               |      10k                     |              |
 LCD SDA| A4        D11 (TX)---[====]---+---------------> GPIO16 (RX2)    |
 LCD SCL| A5                |           |                 |              |
        |           D10 (RX)<-----------|---------------- GPIO17 (TX2)    |
        |                   |         [====] 10k          |              |
        |                   |         [====] 10k          |              |
        |              GND -+-----------+---------------- GND            |
        +------------------+                              +--------------+
```

Power: feed both boards from one good 5V supply (1-2 A) with the grounds joined. Do not power the ESP32 from the
Uno's 5V pin: the WiFi radio plus the MQ-2 heater can overload the Uno regulator and reset it.
**Add a 470-1000 uF capacitor across 5V/GND next to the MQ-2.**
A WROVER ESP32 module uses GPIO 16/17 internally: change `LINK_RX_PIN` / `LINK_TX_PIN` in `esp32_gateway.ino`.

## 1. Firebase (free Spark plan)
1. console.firebase.google.com > Add project.
2. Build > Authentication > Get started > enable **Email/Password**.
   Add one user for the ESP32 (Authentication > Users > Add user) and put its email and password in `esp32_gateway.ino`.
3. Build > Realtime Database > Create database. Copy its URL.
4. Realtime Database > Rules: paste the contents of `database.rules.json` and Publish.
   If your ESP32 account email is not `gasguard1@gmail.com`, change it in the rules first.
   The ESP32 signs in as that normal user (it does not use a database secret), so the rules must
   allow that user to write. If they do not, the Serial Monitor shows `PERMISSION DENIED`.
5. Project settings > General > Your apps > Web (</>): copy the config into `app.js`.

## 2. Host the web app (free)
Firebase Hosting, Netlify Drop or GitHub Pages. HTTPS is required for notifications.
On your phone, open the link and choose "Add to Home screen".
Add your hosted domain in Authentication > Settings > Authorized domains.

## 3. Phone push when the app is closed
Install **ntfy** (Play Store), subscribe to the topic in `esp32_gateway.ino`. Use a long, random topic name
because anyone who knows it can read it. A browser tab cannot ring reliably once the phone sleeps or
the app is closed, so treat ntfy and the local buzzer as your real alarms and the web page as a dashboard.

## 4. Flash the two boards
Flash them one at a time. You can leave the link wires connected while uploading, because the link uses Uno D10/D11,
not the USB pins 0/1.
1. **Uno:** Arduino IDE > File > Open > `arduino_uno_sensor/arduino_uno_sensor.ino`. Board "Arduino Uno".
   Install the library **LiquidCrystal I2C** (Library Manager). Upload. Serial Monitor at 9600.
2. **ESP32:** open `esp32_gateway/esp32_gateway.ino`. Board "ESP32 Dev Module". Fill in the WiFi name and password
   at the top. Upload. Serial Monitor at 115200. Works with the arduino-esp32 core 2.0.x and 3.x.
   (If the upload does not start, hold the BOOT button while "Connecting..." shows.)

## 5. Calibrate once (this is what makes it accurate)
1. Power the Uno in clean air and leave it running 10+ minutes (a brand-new MQ-2 needs hours, ideally 24 h).
2. After about 3.5 minutes (3 min warm-up + 30 s learning) the Uno Serial Monitor prints `Clean-air level learned: NNN`.
3. Turn the blue trimmer on the MQ-2 board until that value is **about 200-450**, then press the Uno reset button.
   The dashboard shows the same value times 4 (about 800-1800). A clean-air level above ~650 here (2600 on the
   dashboard) leaves the sensor little headroom, and the Uno and web app show a `baseline_high` warning.
   Very low values (<62) lose resolution and show `baseline_low`.

## 6. Test
Hold an unlit lighter near the sensor and release gas without a flame. Within about 3 seconds the LCD shows
"GAS DETECTED!", the buzzer sounds and the LED flashes. A moment later the page turns red with sound and ntfy
pushes an alert. Move the gas away: the alarm clears after about 10 seconds below the clear level, and ntfy sends
"back to normal". Then unplug the ESP32 power: the Uno alarm must still work.

## How the alarm decides (all on the Uno)
- The sensor is read every second as 16 quick samples with the highest and lowest dropped.
- After warm-up the sketch learns the **clean-air level**, but only once the readings are steady. Alarm = clean air + 100
  (`ALERT_DELTA`, raw 0-1023 scale; 400 on the dashboard). The clean-air level follows slow drift from temperature and
  humidity (tens of minutes) but not a leak (seconds to minutes).
- 3 readings in a row above the limit start the alarm, 10 in a row below the clear level stop it (no flapping).
- Above 900 (`ABS_ALARM`) it always alarms, even if gas was already present at power-on.
- A disconnected or shorted sensor shows "Sensor error" on the LCD and in the app, and sends a push after 2 minutes
  instead of looking "Safe".
- Every message to the ESP32 carries a checksum. The ESP32 ignores anything damaged, so noise on the wire cannot create a
  fake leak.
- If the ESP32 hears nothing from the Uno for 10 seconds it stops uploading (the app shows "Sensor offline", or keeps the
  alarm on and says contact was lost if the last report was a leak) and sends a push after 2 minutes.

## Troubleshooting
| You see | Cause / fix |
|---|---|
| ESP32 Serial: `Uno link: ignored a damaged message` repeatedly | Loose link wire, no common GND, or the divider is wrong (check 10k on top and 20k to GND). |
| ESP32 prints nothing from the Uno, app says "Sensor offline" | Uno TX (D11) is not reaching GPIO 16. Check the node wiring and that both GNDs are joined. Uno Serial Monitor must show readings. |
| LCD blank | Wrong I2C address (try `0x3F`), or turn the blue contrast screw on the back of the LCD. |
| `Firebase: PERMISSION DENIED` | Publish `database.rules.json` (step 1.4) and check the ESP32 account email matches the rule. |
| `Firebase login failed: 400` | Wrong `DEVICE_EMAIL` / `DEVICE_PASSWORD` or Email/Password sign-in is not enabled. |
| Random resets / `Brownout detector` | Add the capacitor (step 0), use a good 5V 1-2 A supply and a short, thick USB cable. |
| `baseline_high` | Turn the trimmer anticlockwise (step 5). |
| False alarms | Raise `ALERT_DELTA` in the Uno sketch, wait for a longer warm-up on a new sensor, keep it away from cooking fumes, aerosols and alcohol. |
| Misses a small leak | Lower `ALERT_DELTA` (not below ~40). |
| Buzzer is quiet or buzzes strangely | The sketch expects an **active** buzzer (it beeps by itself when powered). A passive buzzer needs `tone()`. |
| `sensor_error` | AO wire or the sensor VCC/GND is loose. The reading is ~0 (unplugged / no power) or maxed out (short). |

Run the tests on a PC:
```
g++ -std=c++11 -O1 -o /tmp/t  tests/test_detector.cpp && /tmp/t     # alarm logic
g++ -std=c++11 -O1 -o /tmp/tl tests/test_link.cpp     && /tmp/tl    # Uno -> ESP32 messages
```

## Safety
This is a learning project, not a certified gas alarm. A very slow leak (hundreds of counts over hours) can be
absorbed by the drift tracking, and the MQ-2 also reacts to smoke, alcohol and humidity. Do not rely on it as
your only protection; use a certified alarm as well.
