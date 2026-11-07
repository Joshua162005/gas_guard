# GasGuard setup (all free)

ESP32 + MQ-2 -> WiFi -> Firebase Realtime Database -> Web app (login, live gauge, alarm)
ESP32 -> ntfy.sh -> push notification on your phone, even when the app is closed.
ESP32 -> buzzer + LED -> local alarm that keeps working when WiFi or the internet is down.

## 0. Wiring
| MQ-2 | ESP32 |
|---|---|
| VCC | VIN (5V) |
| GND | GND |
| AO | 10k resistor -> GPIO 34, and GPIO 34 -> 20k resistor -> GND |

The divider is required: AO can reach 5V and the ESP32 pin only tolerates 3.3V.
Optional: buzzer on GPIO 26, LED + 220 ohm on GPIO 27 (set the pin to -1 in the sketch to disable).
**Add a 470-1000 uF capacitor across 5V/GND next to the sensor.** The MQ-2 heater and the WiFi radio
draw current in bursts, and without it the ESP32 can brown-out and reset at random.

## 1. Firebase (free Spark plan)
1. console.firebase.google.com > Add project.
2. Build > Authentication > Get started > enable **Email/Password**.
   Add one user for the ESP32 (Authentication > Users > Add user) and put its email and password in the `.ino`.
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
Install **ntfy** (Play Store), subscribe to the topic in the `.ino`. Use a long, random topic name
because anyone who knows it can read it. A browser tab cannot ring reliably once the phone sleeps or
the app is closed, so treat ntfy and the local buzzer as your real alarms and the web page as a dashboard.

## 4. Flash the ESP32
Arduino IDE > board "ESP32 Dev Module" > fill in the top of `esp32_gas_sensor.ino` > Upload.
`esp32_gas_sensor.ino` and `gas_detector.h` must stay in the same folder. Works with the arduino-esp32
core 2.0.x and 3.x. Open the Serial Monitor at 115200.

## 5. Calibrate once (this is what makes it accurate)
1. Power the sensor in clean air and leave it running 10+ minutes (a brand-new MQ-2 needs hours, ideally 24 h).
2. The Serial Monitor prints `clean air: NNNN` after about 3.5 minutes (3 min warm-up + 30 s learning).
3. Turn the blue trimmer on the MQ-2 board until the clean-air value is **about 800-1800**, then reboot the ESP32.
   Above ~2600 the ESP32's ADC is badly compressed and gas barely changes the reading, and the sketch/web app
   will show a `baseline_high` warning. Very low values (<250) lose resolution and show `baseline_low`.

## 6. Test
Hold an unlit lighter near the sensor and release gas without a flame. Within a few seconds the reading rises,
the buzzer sounds, the page turns red with sound, and ntfy pushes an alert. Move the gas away: the alarm clears
after about 10 seconds below the clear level, and ntfy sends "back to normal".

## How the alarm decides
- The sensor is read every second, on its own CPU core, so slow WiFi can never delay the alarm.
- Each reading is 32 calibrated samples with spikes removed (the plain ESP32 `analogRead` is noisy and non-linear).
- After warm-up the sketch learns the **clean-air level**. Alarm = clean air + 400 (`ALERT_DELTA`). The clean-air
  level follows slow drift from temperature and humidity (tens of minutes) but not a leak (seconds to minutes).
- 5 readings in a row above the limit start the alarm, 10 in a row below the clear level stop it (no flapping).
- Above 3600 (`ABS_ALARM`) it always alarms, even if gas was already present at power-on.
- A disconnected or shorted sensor shows "Sensor not detected" and sends a push after 2 minutes instead of looking "Safe".
- When the sensor stops reporting during a leak, the app keeps the alarm on and says contact was lost.

## Troubleshooting
| You see | Cause / fix |
|---|---|
| `Firebase: PERMISSION DENIED` | Publish `database.rules.json` (step 1.4) and check the ESP32 account email matches the rule. |
| `Firebase login failed: 400` | Wrong `DEVICE_EMAIL` / `DEVICE_PASSWORD` or Email/Password sign-in is not enabled. |
| Random resets / `Brownout detector` | Add the capacitor (step 0), use a good 5V 1-2 A supply and a short, thick USB cable. |
| `baseline_high` | Turn the trimmer anticlockwise (step 5). |
| False alarms | Raise `ALERT_DELTA`, wait for a longer warm-up on a new sensor, keep it away from cooking fumes, aerosols and alcohol. |
| Misses a small leak | Lower `ALERT_DELTA` (not below ~150). |
| `sensor_error` | AO wire, divider, VIN or GND is loose. The reading is ~0 (unplugged / no power) or maxed out (short). |

Run the detector tests on a PC: `g++ -std=c++11 -O1 -o /tmp/t tests/test_detector.cpp && /tmp/t`

## Safety
This is a learning project, not a certified gas alarm. A very slow leak (hundreds of counts over hours) can be
absorbed by the drift tracking, and the MQ-2 also reacts to smoke, alcohol and humidity. Do not rely on it as
your only protection; use a certified alarm as well.
