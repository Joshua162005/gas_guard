# GasGuard setup (all free)

ESP32 + MQ-2 -> WiFi -> Firebase Realtime Database -> Web app (login, live gauge, alarm)
ESP32 -> ntfy.sh -> push notification on your phone, even when the app is closed.

## 1. Firebase (free Spark plan)
1. console.firebase.google.com > Add project.
2. Build > Authentication > Get started > enable **Email/Password**.
3. Build > Realtime Database > Create database. Copy its URL.
4. Realtime Database > Rules, paste, then Publish:
```json
{
  "rules": {
    "users": { "$uid": { ".read": "auth.uid === $uid", ".write": "auth.uid === $uid" } },
    "devices": { ".read": "auth != null", ".write": false }
  }
}
```
The ESP32 writes with the database secret, which bypasses these rules, so `devices` stays read-only for browsers.
5. Project settings > General > Your apps > Web (</>): copy the config into `index.html`.
6. Project settings > Service accounts > Database secrets: copy the secret into the `.ino` file.

## 2. Host the web app (free)
Firebase Hosting, Netlify Drop or GitHub Pages. HTTPS is required for notifications.
On your phone, open the link and choose "Add to Home screen".
Add your hosted domain in Authentication > Settings > Authorized domains.

## 3. Phone push when the app is closed
Install **ntfy** (Play Store), subscribe to the topic in the `.ino` file. Use a long, unique topic name because anyone who knows it can read it.

## 4. Flash the ESP32
Arduino IDE > board "ESP32 Dev Module" > fill in the top of the `.ino` > Upload.
Open Serial Monitor at 115200 to see readings.

## 5. Test
Wait 30 s for warm-up. Hold an unlit lighter near the sensor and release gas without a flame. The gauge rises past the limit, the page turns red with sound, and ntfy pushes an alert.

## Safety
This is a learning project, not a certified gas alarm. Do not rely on it as your only protection.
