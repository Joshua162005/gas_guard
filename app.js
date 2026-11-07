// ===== Firebase Config =====
const firebaseConfig = {
  apiKey: "AIzaSyCn-B4xQGRisNe4U0WDOs6q2fvUAKf9CtE",
  authDomain: "gasguard-0950.firebaseapp.com",
  databaseURL: "https://gasguard-0950-default-rtdb.firebaseio.com",
  projectId: "gasguard-0950",
  storageBucket: "gasguard-0950.firebasestorage.app",
  messagingSenderId: "420448010396",
  appId: "1:420448010396:web:4c1d27eb60eb1b2cc3ad59",
  measurementId: "G-2P95W1K2F7"
};

firebase.initializeApp(firebaseConfig);
const auth = firebase.auth(), db = firebase.database();

// Keep the user signed in across refreshes and app restarts (stored on this device until Log out).
auth.setPersistence(firebase.auth.Auth.Persistence.LOCAL).catch(() => {});

const $ = id => document.getElementById(id);
let signup = false, unsub = null, wasDanger = false, audioCtx = null, alarmTimer = null;
let silenced = false, lastGas = 0;

// ===== Server clock + connection state =====
// "How long ago did the sensor report" must be measured on Firebase's clock, not on this
// phone's. A phone whose clock is a minute fast used to show "Sensor offline" all the time.
let serverOffset = 0, online = true;
const serverNow = () => Date.now() + serverOffset;
db.ref('.info/serverTimeOffset').on('value', s => { serverOffset = s.val() || 0; });
db.ref('.info/connected').on('value', s => { online = s.val() === true; tick(); });

// ===== Auth =====
$('swap').onclick = () => {
  signup = !signup;
  $('nameWrap').classList.toggle('hidden', !signup);
  $('go').textContent = signup ? 'Create account' : 'Log in';
  $('swap').textContent = signup ? 'Log in' : 'Sign up';
  $('swapText').textContent = signup ? 'Already have an account?' : 'No account yet?';
  $('authSub').textContent = signup ? 'Create an account to start monitoring.' : 'Log in to monitor your gas sensor.';
  $('err').textContent = '';
};

$('go').onclick = async () => {
  $('err').textContent = '';
  const email = $('email').value.trim(), pass = $('pass').value;
  try {
    if (signup) {
      const name = $('name').value.trim();
      if (!name) return $('err').textContent = 'Enter your full name.';
      const c = await auth.createUserWithEmailAndPassword(email, pass);
      await c.user.updateProfile({ displayName: name });
      await db.ref('users/' + c.user.uid).update({ name, email });
    } else {
      await auth.signInWithEmailAndPassword(email, pass);
    }
  } catch (e) {
    const m = {
      'auth/invalid-credential': 'Wrong email or password.',
      'auth/email-already-in-use': 'This email already has an account. Log in instead.',
      'auth/weak-password': 'Use a password with at least 6 characters.',
      'auth/invalid-email': 'Enter a valid email address.'
    };
    $('err').textContent = m[e.code] || e.message;
  }
};

$('out').onclick = () => auth.signOut();

auth.onAuthStateChanged(user => {
  $('auth').classList.toggle('hidden', !!user);
  $('app').classList.toggle('hidden', !user);
  if (unsub) { unsub(); unsub = null; }
  if (!user) return;
  $('hello').textContent = user.displayName || user.email;
  db.ref('users/' + user.uid + '/deviceId').once('value').then(s => {
    if (s.val()) { $('dev').value = s.val(); watch(s.val()); }
  });
});

// ===== Device =====
$('saveDev').onclick = async () => {
  const id = $('dev').value.trim().replace(/[.#$[\]\/]/g, '');
  if (!id) return;
  await db.ref('users/' + auth.currentUser.uid + '/deviceId').set(id);
  $('devMsg').textContent = 'Saved. Showing live data for ' + id + '.';
  watch(id);
};

function watch(id) {
  if (unsub) unsub();
  const ref = db.ref('devices/' + id);
  const cb = ref.on('value', s => render(s.val()));
  unsub = () => ref.off('value', cb);
}

// ===== Render =====
let lastSeen = 0;
const OFFLINE_AFTER_S = 40;
const STALE_AFTER_S = 120;
const HINTS = {
  baseline_high: 'Clean-air level is too high for accurate readings. Turn the blue trimmer on the MQ-2 board slowly anticlockwise until it is about 800-1800, then restart the ESP32.',
  baseline_low: 'Clean-air level is very low. Turn the blue trimmer on the MQ-2 board slightly clockwise until it is about 800-1800, then restart the ESP32.'
};

function render(d) {
  const box = $('status');
  if (!d) {
    box.className = 'card status offline';
    $('statusMain').textContent = 'No data';
    $('statusSub').textContent = 'Waiting for sensor';
    return;
  }
  const gas = d.gas || 0;
  const hasState = typeof d.state === 'string';
  const state = hasState ? d.state : (gas > (d.threshold || 1500) ? 'alert' : 'normal');
  // The ESP32 already ignores single spikes (several readings in a row) and uses hysteresis,
  // so its state is trusted as it is. Comparing the gas value to the limit here a second time
  // made one noisy reading raise a false alarm. Only old firmware without a state field
  // falls back to the plain comparison (done two lines above).
  // Data that is already old when it arrives (page opened while the sensor is off, or the last
  // report was a leak hours ago) is history, not a live leak: show it as offline, never alarm.
  const ageS = d.updated ? (serverNow() - d.updated) / 1000 : 0;
  const stale = ageS > STALE_AFTER_S;   // generous, so a phone with a wrong clock cannot hide a live leak
  const reportedLeak = state === 'alert' || d.alert === true;
  const danger = reportedLeak && (!stale || wasDanger);
  const waiting = !danger && (state === 'warming_up' || state === 'calibrating' || state === 'sensor_error');
  lastSeen = d.updated || 0;
  lastGas = gas;
  $('popGas').textContent = gas;
  $('val').textContent = gas;
  $('lim').textContent = d.threshold || '--';
  $('base').textContent = d.baseline > 0 ? d.baseline : '--';
  $('hint').textContent = HINTS[d.warn] || '';
  const pct = Math.min(100, gas / 4095 * 100);
  $('fill').style.width = pct + '%';
  $('fill').style.background = danger ? 'var(--red)' : 'var(--blue)';
  box.className = 'card status ' + (danger ? 'danger' : waiting ? 'offline' : 'safe');
  const text = {
    warming_up:   ['Sensor warming up', 'Readings are not reliable yet. This takes a few minutes.'],
    calibrating:  ['Calibrating', 'Learning clean-air level. Keep gas away from the sensor.'],
    sensor_error: ['Sensor not detected', 'Check the MQ-2 wiring and power.'],
    alert:        ['GAS LEAK!', 'Open windows. Turn off the stove and LPG tank. Do not use switches.']
  }[danger ? 'alert' : state] || ['Safe', 'Air looks normal'];
  $('statusMain').textContent = text[0];
  $('statusSub').textContent = text[1];
  if (danger && !wasDanger) alarm();
  if (!danger) { stopAlarm(); hidePopup(); silenced = false; }
  wasDanger = danger;
  if (stale) tick();                    // old data: let the offline card replace the normal one
}

function tick() {
  if (!lastSeen) return;
  const s = Math.max(0, Math.round((serverNow() - lastSeen) / 1000));
  $('upd').textContent = s < 60 ? 'updated ' + s + 's ago' : 'updated ' + Math.round(s / 60) + 'm ago';
  if (online && s <= OFFLINE_AFTER_S) return;

  if (wasDanger && !silenced) {
    // The last report was a leak. Losing contact is not proof that it is over, so the alarm
    // and the red card stay until the sensor reports again or you press Silence.
    $('status').className = 'card status danger';
    $('statusMain').textContent = 'GAS LEAK! (contact lost)';
    $('statusSub').textContent = online
      ? 'The sensor stopped reporting during a leak. Treat it as a leak.'
      : 'This phone lost its connection during a leak. Treat it as a leak.';
    return;
  }
  wasDanger = false;
  $('status').className = 'card status offline';
  if (online) {
    $('statusMain').textContent = 'Sensor offline';
    $('statusSub').textContent = 'No update in the last ' + OFFLINE_AFTER_S + ' seconds. Check the sensor power and WiFi.';
  } else {
    $('statusMain').textContent = 'No connection';
    $('statusSub').textContent = 'This phone cannot reach the server, so the sensor status is unknown.';
  }
  stopAlarm();
  hidePopup();
  silenced = false;
}
setInterval(tick, 1000);

// ===== Alerts =====
// Browsers cannot "un-grant" notification permission from code, so the on/off switch is
// our own setting (saved on this device). Alerts are ON by default. When they are OFF the
// page stays quiet: no popup, no sound, no vibration, no system notification.
// The red status card always shows a leak, whatever this setting is.
const PREF_KEY = 'gasguard-alerts';

function prefOn() {
  try { return localStorage.getItem(PREF_KEY) !== 'off'; } catch (e) { return true; }
}
function setPref(on) {
  try { localStorage.setItem(PREF_KEY, on ? 'on' : 'off'); } catch (e) {}
}
function notifSupported() { return 'Notification' in window; }
function canNotify() { return notifSupported() && Notification.permission === 'granted'; }

// Browsers only allow sound after a tap, so arm the audio on the first tap anywhere.
function armAudio() {
  try {
    audioCtx = audioCtx || new (window.AudioContext || window.webkitAudioContext)();
    if (audioCtx.state === 'suspended') audioCtx.resume();
  } catch (e) { console.warn('Audio not available', e); }
}
document.addEventListener('click', armAudio);
document.addEventListener('touchstart', armAudio);

function refreshAlertUi() {
  const on = prefOn();
  const needPermission = notifSupported() && Notification.permission === 'default';

  $('notif').textContent = on ? 'Turn off alerts' : 'Turn on alerts';
  $('notif').classList.toggle('secondary', on);

  let msg;
  if (!on) msg = 'Alerts are OFF. No popup, sound or notification will appear when a leak is detected.';
  else if (!notifSupported()) msg = 'Alerts are ON (popup, sound, vibration). This browser cannot show system notifications.';
  else if (Notification.permission === 'denied') msg = 'Alerts are ON (popup, sound, vibration). System notifications are blocked: allow them in your browser settings.';
  else if (needPermission) msg = 'Alerts are ON. Tap the red banner to also allow system notifications.';
  else msg = 'Alerts are ON: popup, sound, vibration and system notification.';
  $('alertState').textContent = msg;

  let b = $('alertBanner');
  if (!b) {
    b = document.createElement('div');
    b.id = 'alertBanner';
    b.className = 'card';
    b.style.cssText = 'background:#fee2e2;border-color:#dc2626;color:#7f1d1d;cursor:pointer;font-weight:600';
    b.onclick = turnOnAlerts;
    const main = document.querySelector('main');
    main.insertBefore(b, main.firstChild);
  }
  b.textContent = !on ? 'Alerts are OFF. Tap here to turn them on.'
                      : 'Tap here to allow leak notifications and sound.';
  b.classList.toggle('hidden', on && !needPermission);
}

async function turnOnAlerts() {
  setPref(true);
  armAudio();
  if (notifSupported() && Notification.permission === 'default') {
    try { await Notification.requestPermission(); } catch (e) { console.warn(e); }
  }
  refreshAlertUi();
  if (wasDanger && !silenced) alarm();            // a leak is happening right now
  else notify('GasGuard alerts are on', 'You will be notified when a leak is detected.', 'gasguard-test');
}

function turnOffAlerts() {
  setPref(false);
  stopAlarm();
  hidePopup();
  closeNotifications();
  refreshAlertUi();
}

$('notif').onclick = () => prefOn() ? turnOffAlerts() : turnOnAlerts();
refreshAlertUi();

// Wait for the service worker, but never hang forever if it is not available.
function swReady() {
  if (!('serviceWorker' in navigator)) return Promise.resolve(null);
  return Promise.race([
    navigator.serviceWorker.ready,
    new Promise(res => setTimeout(() => res(null), 2000))
  ]);
}

// Android Chrome (and installed PWAs) do not allow "new Notification()".
// The service worker version works everywhere.
async function notify(title, body, tag) {
  if (!prefOn() || !canNotify()) return;
  const opts = { body, tag: tag || 'gasguard-leak', renotify: true, requireInteraction: true,
                 vibrate: [300, 150, 300, 150, 600] };
  try {
    const reg = await swReady();
    if (reg) return await reg.showNotification(title, opts);
  } catch (e) { console.warn('SW notification failed', e); }
  try { new Notification(title, opts); } catch (e) { console.warn('Notification failed', e); }
}

async function closeNotifications() {
  try {
    const reg = await swReady();
    if (reg) (await reg.getNotifications()).forEach(n => n.close());
  } catch (e) { console.warn(e); }
}

// ----- In-page popup -----
function showPopup() {
  $('popGas').textContent = lastGas;
  $('leakPopup').classList.remove('hidden');
  $('popSilence').focus();
}
function hidePopup() { $('leakPopup').classList.add('hidden'); }

$('popSilence').onclick = silenceAlarm;
$('silence').onclick = silenceAlarm;
$('popClose').onclick = hidePopup;

// ----- Sound -----
function beep() {
  if (!audioCtx) return;
  const o = audioCtx.createOscillator(), g = audioCtx.createGain();
  o.type = 'square';
  o.frequency.value = 880;
  g.gain.value = 0.2;
  o.connect(g);
  g.connect(audioCtx.destination);
  o.start();
  o.stop(audioCtx.currentTime + 0.35);
}

function alarm() {
  if (!prefOn()) return;                          // alerts switched off: stay quiet
  silenced = false;
  showPopup();
  notify('GAS LEAK DETECTED!', 'Open windows and turn off your LPG tank now.');
  stopAlarm();
  beep();
  alarmTimer = setInterval(() => {
    beep();
    if (navigator.vibrate) navigator.vibrate([300, 150, 300]);
  }, 800);
  $('silence').classList.remove('hidden');
}

function stopAlarm() {
  clearInterval(alarmTimer);
  alarmTimer = null;
  if (navigator.vibrate) navigator.vibrate(0);
  $('silence').classList.add('hidden');
}

// Stops the sound and vibration for this leak. It rings again on the next leak.
function silenceAlarm() {
  silenced = true;
  stopAlarm();
  hidePopup();
  closeNotifications();
}

// ===== Register Service Worker (PWA Install Support) =====
if ('serviceWorker' in navigator) {
  navigator.serviceWorker.register('./sw.js')
    .then(() => console.log('GasGuard SW registered'))
    .catch(err => console.warn('SW registration failed:', err));
}

