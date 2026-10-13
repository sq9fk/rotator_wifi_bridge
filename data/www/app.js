'use strict';

const $ = (id) => document.getElementById(id);
let socket = null;
let state = null;
let jogHeld = null;
let jogTimer = null;
let favorites = [];
let favDirty = false;
// Antenna 1..6 names for the legend/button tooltips - fetched by the bridge
// straight from ant-sw-2x6's own EEPROM (see AntennaSwitch.h) and carried in
// the live status stream (s.antenna.names), not typed in twice here.
let antNames = ['1', '2', '3', '4', '5', '6'];
// Which antenna this rotator physically turns (Settings - Przełącznik
// antenowy) - 0 means "not configured". No TRX of its own: the same antenna
// number is one physical port shared by both TRX rows. Loaded once from
// /api/config, used by renderAntenna() to outline that number in both rows
// and bold it in the legend; the switch itself has no concept of this.
let rotorAnt = 0;
// Which configured account this browser's cookie belongs to, and the full
// account list - both come from /api/session, fetched once at init().
let myUser = null;
let knownUsers = [];

const POINTS = ['N', 'NNE', 'NE', 'ENE', 'E', 'ESE', 'SE', 'SSE',
                'S', 'SSW', 'SW', 'WSW', 'W', 'WNW', 'NW', 'NNW'];
const pointName = (az) => POINTS[Math.round(((az % 360) + 360) % 360 / 22.5) % 16];

// SVG elements do not carry the HTML `hidden` IDL property, so `el.hidden =
// false` only sets a dead JS expando and leaves the DOM attribute in place -
// which the global [hidden] rule keeps invisible. Toggle the attribute itself.
function svgShow(el, visible) {
  if (visible) el.removeAttribute('hidden');
  else el.setAttribute('hidden', '');
}

// The session is server-side state only (in-memory, per Auth.h) - a reboot
// (a settings save's own "Restart" button, a crash, a power cycle) wipes
// every session at once with no warning, so a cookie this tab has been
// holding since before that can go stale mid-session, not just after being
// idle 15 minutes. Every 401 from here reloads: a fresh /api/session check
// is what actually knows the account is logged out and shows the login
// screen - without this, a click after that point looked like the button
// (or the whole panel) had simply stopped responding, one silently-rejected
// request at a time, forever.
// /api/login's own 401 means "wrong password", not "your session died" -
// it must reach the login form's own error message, never trigger a reload.
function handleUnauthorized(path, status) {
  if (status === 401 && path !== '/api/login') {
    location.reload();
  }
}

async function post(path, params) {
  const res = await fetch(path, { method: 'POST', body: new URLSearchParams(params || {}) });
  handleUnauthorized(path, res.status);
  return { ok: res.ok, status: res.status, data: await res.json().catch(() => ({})) };
}
const getJson = async (path) => {
  const r = await fetch(path);
  handleUnauthorized(path, r.status);
  return r.ok ? r.json() : null;
};

// --- dial geometry ---------------------------------------------------------

const C = 220;            // centre
const R_RING = 168;       // white ring
const R_DEG = 205;        // degree labels, outside the ring
const R_ROSE = 132;       // NE / SW letters
const R_FAV = 168;        // favourite markers, sitting on the ring

// A soft categorical palette that sweeps the wheel at even saturation and
// lightness, so up to ten favourites stay distinguishable yet read as one set
// against the dark teal. Deliberately off the pure accent and overlap colours,
// which mean other things.
const FAV_COLORS = [
  '#ffd15c', '#f6a06a', '#ef6f6c', '#e77fb3', '#b98cdb',
  '#7c9cf0', '#5cc6e8', '#4fc9a8', '#8ed081', '#ccd45f',
];
const favColor = (i) => FAV_COLORS[i % FAV_COLORS.length];
const NEEDLE = 148;

const polar = (deg, r) => {
  const a = (deg - 90) * Math.PI / 180;
  return [C + r * Math.cos(a), C + r * Math.sin(a)];
};

// Sweeps clockwise from -> to, wrapping through north when to < from, which is
// how a band like 270 -> 90 is expressed.
function arcPath(fromDeg, toDeg, r) {
  const sweep = ((toDeg - fromDeg) % 360 + 360) % 360;
  const [sx, sy] = polar(fromDeg, r);
  const [ex, ey] = polar(fromDeg + sweep, r);
  return `M ${sx} ${sy} A ${r} ${r} 0 ${sweep > 180 ? 1 : 0} 1 ${ex} ${ey}`;
}

function buildDial() {
  const parts = [`<circle class="face" cx="${C}" cy="${C}" r="${R_RING - 5}"/>`,
                 `<circle class="ring" cx="${C}" cy="${C}" r="${R_RING}"/>`];

  for (let deg = 0; deg < 360; deg += 5) {
    const major = deg % 30 === 0;
    const [x1, y1] = polar(deg, R_RING - (major ? 16 : 9));
    const [x2, y2] = polar(deg, R_RING - 4);
    parts.push(`<line class="${major ? 'tick-major' : 'tick'}" x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}"/>`);
  }

  for (let deg = 0; deg < 360; deg += 30) {
    const [x, y] = polar(deg, R_DEG);
    parts.push(`<text class="deg-label" x="${x}" y="${y}">${deg}°</text>`);
  }

  // Heavier marks at the cardinals, so N/E/S/W are findable without reading.
  [0, 90, 180, 270].forEach((deg) => {
    const [x1, y1] = polar(deg, R_RING - 12);
    const [x2, y2] = polar(deg, R_RING + 12);
    parts.push(`<line class="cardinal-mark" x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}"/>`);
  });

  ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'].forEach((label, i) => {
    const [x, y] = polar(i * 45, R_ROSE);
    parts.push(`<text class="rose" x="${x}" y="${y}">${label}</text>`);
  });

  parts.push(`<line class="cross" x1="${C - R_ROSE}" y1="${C}" x2="${C + R_ROSE}" y2="${C}"/>`);
  parts.push(`<line class="cross" x1="${C}" y1="${C - R_ROSE}" x2="${C}" y2="${C + R_ROSE}"/>`);

  $('dialStatic').innerHTML = parts.join('');
}

// A tapered needle rather than a line: the wide end reads as "here", the point
// as "pointing at", which a plain stroke does not convey.
function needlePoints(deg, length, width) {
  const [tx, ty] = polar(deg, length);
  const [lx, ly] = polar(deg + 90, width);
  const [rx, ry] = polar(deg - 90, width);
  return `${tx},${ty} ${lx},${ly} ${rx},${ry}`;
}

function renderFavMarks() {
  $('favMarks').innerHTML = favorites.map((f, i) => {
    const [x, y] = polar(f.az, R_FAV);
    return `<circle class="fav-mark" cx="${x}" cy="${y}" r="8" fill="${favColor(i)}"/>`;
  }).join('');
}

// --- rendering -------------------------------------------------------------

function render(s) {
  state = s;
  const p = s.position;

  $('needle').setAttribute('points', needlePoints(p.azimuth, NEEDLE, 15));

  const target = $('targetNeedle');
  svgShow(target, p.hasTarget);
  if (p.hasTarget) {
    target.setAttribute('points', needlePoints(p.target, NEEDLE - 18, 13));
  }

  // The hub reads the real bearing, so it agrees with where the needle points
  // on the 0-360 ring.
  $('hubPoint').textContent = p.fresh ? pointName(p.azimuth) : '—';
  $('hubValue').textContent = p.fresh ? Math.round(p.azimuth) + '°' : '---°';
  // The rotor makes one 405-degree sweep, not two laps: a bearing has one raw
  // value everywhere except the overlap (180-225), where the same bearing is
  // two mechanical positions. So raw only tells the operator anything there -
  // elsewhere it is fixed by the bearing and would be noise.
  $('hubRaw').textContent = (p.fresh && p.overlap) ? 'raw ' + Math.round(p.raw) + '°' : '';
  $('posValue').textContent = p.fresh ? Math.round(p.azimuth) + '°' : '---°';
  $('targetValue').textContent = p.hasTarget ? Math.round(p.target) + '°' : '—';

  // The arc appears only while the rotator is actually in the band reachable
  // two ways. Drawn permanently it is scenery the eye stops seeing; drawn on
  // entry it is a state change, which is what it needs to be - the bearing
  // alone cannot tell you the antenna is on its second lap.
  const arc = $('overlapArc');
  const from = s.controller.overlapFrom, to = s.controller.overlapTo;
  const showArc = p.overlap && from !== to;
  svgShow(arc, showArc);
  if (showArc) {
    arc.setAttribute('d', arcPath(from, to, R_RING));
  }
  $('olBadge').hidden = !p.overlap;  // OL badge is an HTML span, so .hidden is fine

  // Three states: green when talking to the controller (fresh position),
  // yellow when a poll is not getting through but the link is not yet declared
  // dead, red when the watchdog has given up (link severed).
  const dot = $('linkDot');
  const line = s.controller.line || 'attached';
  if (line !== 'attached') dot.className = 'dot warn';
  else if (!s.controller.linkHealthy) dot.className = 'dot bad';
  else if (!p.fresh) dot.className = 'dot warn';
  else dot.className = 'dot';

  const banner = $('banner');
  if (!s.controller.linkHealthy) {
    banner.hidden = false;
    banner.className = 'banner bad';
    banner.innerHTML = 'Brak łączności ze sterownikiem<span>sprawdź zasilanie i okablowanie</span>';
  } else if (s.sources.remoteConnected) {
    const bits = [];
    if (s.sources.rotctld.clients) bits.push('rotctld ' + s.sources.rotctld.addresses);
    if (s.sources.raw.clients) bits.push('raw ' + s.sources.raw.addresses);
    banner.hidden = false;
    banner.className = 'banner';
    banner.innerHTML = 'Zdalne sterowanie podłączone<span>' + bits.join(' • ') + '</span>';
  } else if (s.controller.bootLockout) {
    banner.hidden = false;
    banner.className = 'banner';
    banner.innerHTML = 'Sterownik po restarcie<span>komendy obrotu ignorowane przez 5 s</span>';
  } else {
    banner.hidden = true;
  }

  const rc = s.sources.rotctld, rw = s.sources.raw;
  $('rotctldRow').textContent = rc.clients ? `${rc.addresses} (${rc.clients}/${rc.max})` : `port ${rc.port} (0/${rc.max})`;
  $('rotctldRow').className = rc.clients ? 'on' : '';
  $('rawRow').textContent = rw.clients ? `${rw.addresses} (${rw.clients}/${rw.max})` : `port ${rw.port} (0/${rw.max})`;
  $('rawRow').className = rw.clients ? 'on' : '';
  renderSessions(s.sessions);

  $('motionRow').innerHTML = s.lastMotion ? formatMotion(s.lastMotion) : '—';

  $('ccwBtn').classList.toggle('active', s.jogging && jogHeld === 'ccw');
  $('cwBtn').classList.toggle('active', s.jogging && jogHeld === 'cw');

  $('sysNet').textContent = `${s.network.mode} · ${s.network.ssid} · ${s.network.rssi} dBm`;
  $('sysAddr').textContent = s.network.address;
  $('sysRange').textContent = `${s.controller.rawMin}..${s.controller.rawMax}°`;
  $('sysHeap').textContent = Math.round(s.heapFree / 1024) + ' kB';
  $('sysUptime').textContent = Math.floor(s.uptimeMs / 60000) + ' min';

  renderLine(s.controller);
  renderAntenna(s.antenna);
  appendDebugLog(s.debugLog, s.debugOverflow);
  renderUpdate(s.update);
}

// Names the account behind a web-sourced motion instead of the generic "web"
// (raw/rotctld/poller show as-is - there is no account to name there). The
// bridge only knows the real date once it has synced from the internet (see
// Net.h); until then this falls back to a relative age, same as before.
// What and who on one line, when under it: "→ 245° · sq9fk" / "2026-10-13
// 12:34:56 UTC". HTML - the account name is escaped.
function formatMotion(m) {
  const who = escapeHtml(String((m.source === 'web' && m.user) ? m.user : m.source));
  const what = m.kind === 'cw' ? '↻ ręcznie' : m.kind === 'ccw' ? '↺ ręcznie'
    : (m.target !== undefined ? `→ ${Math.round(m.target)}°` : '→ ?');
  if (!m.epochS) {
    return `${what} · ${who}<small>${Math.round(m.ageMs / 1000)} s temu</small>`;
  }
  // UTC, matching the panel's own clock elsewhere on this page - a rotator
  // log is compared against schedules, beacons and other stations' logs, all
  // in UTC, so local time would invite an off-by-an-hour mismatch.
  const iso = new Date(m.epochS * 1000).toISOString();
  return `${what} · ${who}<small>${iso.substring(0, 10)} ${iso.substring(11, 19)} UTC</small>`;
}

// One row per configured account (not just "this browser"), so an operator
// can see who else is logged in and from where - the same "who is in
// control" reasoning as the rotctld/raw client rows above it.
function renderSessions(sessions) {
  if (!Array.isArray(sessions)) return;
  $('sessRows').innerHTML = sessions.map((sess) => {
    const mine = sess.name === myUser;
    const label = mine ? `${sess.name} (ta przeglądarka)` : sess.name;
    const value = sess.active ? (mine ? 'Zalogowano' : sess.address) : '—';
    return `<div class="row"><span>${escapeHtml(label)}</span>` +
      `<strong class="${sess.active ? 'on' : ''}">${escapeHtml(value)}</strong></div>`;
  }).join('');
}

// --- antenna switch (ant-sw-2x6) --------------------------------------------
// A separate device on the LAN, reached over plain HTTP (not OTRSP, which
// stays reserved for the logging program) - see AntennaSwitch.h on the
// firmware side. The card is hidden entirely while the feature is off.

// At most one /api/antenna request in flight per bank - rapid clicks collapse
// to whichever antenna was clicked last, sent as soon as the in-flight
// request finishes, instead of each click firing its own overlapping
// fetch(). The browser allows up to 6 concurrent connections per origin, but
// the ESP32 has only a handful of TCP sockets total (shared with the
// WebSocket, rotctld/raw clients and the antenna switch's own outgoing
// connection) - a burst of clicks could exhaust that pool with requests that
// then sit stuck long enough to need a page reload to clear, with nothing to
// see in the firmware's own log because the failure happens below the level
// anything here would log. Same "last command wins" idea as the firmware's
// own per-bank queue (see AntennaSwitch.cpp), just applied a layer earlier.
const antPending = [false, false];
const antWanted = [null, null];

async function sendAntenna(bank, ant) {
  antWanted[bank] = ant;
  if (antPending[bank]) return;
  antPending[bank] = true;
  while (antWanted[bank] !== null) {
    const next = antWanted[bank];
    antWanted[bank] = null;
    await post('/api/antenna', { bank: bank + 1, ant: next });
  }
  antPending[bank] = false;
}

// Same one-in-flight-per-bank coalescing as sendAntenna() above, for that
// device's "Radio Flex"/PWR output - false is a real, legitimate value here,
// distinct from the null "nothing queued" sentinel, so it collapses rapid
// toggling the same way.
const pwrPending = [false, false];
const pwrWanted = [null, null];

async function sendPower(bank, on) {
  pwrWanted[bank] = on;
  if (pwrPending[bank]) return;
  pwrPending[bank] = true;
  while (pwrWanted[bank] !== null) {
    const next = pwrWanted[bank];
    pwrWanted[bank] = null;
    await post('/api/antenna/power', { bank: bank + 1, on: next ? 1 : 0 });
  }
  pwrPending[bank] = false;
}

function buildAntennaButtons() {
  const labels = ['OFF', '1', '2', '3', '4', '5', '6'];
  for (let bank = 0; bank < 2; bank++) {
    const container = $('antButtons' + bank);
    container.innerHTML = labels.map((label, ant) =>
      `<button type="button" class="ghost small" data-ant="${ant}">${label}</button>`).join('');
    container.querySelectorAll('button').forEach((b) => {
      b.onclick = () => sendAntenna(bank, b.dataset.ant);
    });
    const pwrBtn = $('pwrBtn' + bank);
    pwrBtn.onclick = () => sendPower(bank, !pwrBtn.classList.contains('on'));
  }
  updateAntennaTitles();
}

function updateAntennaTitles() {
  for (let bank = 0; bank < 2; bank++) {
    $('antButtons' + bank).querySelectorAll('button').forEach((b) => {
      const ant = Number(b.dataset.ant);
      b.title = ant === 0 ? '' : (antNames[ant - 1] || '');
    });
  }
}

const escapeHtml = (s) => s.replace(/[&<>"']/g, (c) => ({
  '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
})[c]);

// A single wrapping line - "1 ANT1 · 2 ANT2 · ..." - rather than a legend
// card of its own (there is no room for one under the dial): enough to look
// a number up, without competing for space with the buttons above it. Each
// "N name" pair is a non-breaking span, so a line break can only ever land on
// a " · " separator - never splitting a number from its own name.
function renderAntennaLegend() {
  $('antLegend').innerHTML = antNames.map((n, i) => {
    const cls = 'ant-legend-item' + (rotorAnt >= 1 && i + 1 === rotorAnt ? ' rotor' : '');
    return `<span class="${cls}">${i + 1} ${escapeHtml(n)}</span>`;
  }).join(' · ');
}

function renderAntenna(ant) {
  if (!ant) return;
  $('antCard').hidden = !ant.enabled;
  // Shown in the topbar whenever the switch is active and this rotor's own
  // antenna is configured (Settings - Przełącznik antenowy), independently of
  // whether the Anteny card itself is currently in view - which antenna this
  // rotor is aimed at is useful at a glance from anywhere in the panel.
  $('topRotorAnt').textContent = (ant.enabled && rotorAnt >= 1 && antNames[rotorAnt - 1])
    ? antNames[rotorAnt - 1] + ' ·' : '';
  if (!ant.enabled) return;

  // Straight from the switch's own EEPROM (see AntennaSwitch.h on the
  // firmware side), not something typed in here - so this can never disagree
  // with that device's own panel.
  if (Array.isArray(ant.names) && ant.names.length === 6) {
    antNames = ant.names;
    renderAntennaLegend();
    updateAntennaTitles();
  }

  // Same three-state idea as the controller link's own dot (linkDot): solid
  // green only once connected AND recently confirmed, amber once connected
  // but stale, red otherwise (never connected, or explicitly disconnected).
  $('antDot').className = !ant.connected ? 'dot bad' : (!ant.fresh ? 'dot warn' : 'dot');
  // Straight from the switch's own EEPROM too (see above) - empty until the
  // first successful fetch, or on older ant-sw-2x6 firmware that doesn't send
  // it yet.
  $('antDeviceName').textContent = ant.deviceName ? '· ' + ant.deviceName : '';

  const active = [0, 1].map((bank) => {
    const bankState = ant.banks && ant.banks[bank];
    return bankState && bankState.ant !== undefined ? bankState.ant : -1;
  });

  for (let bank = 0; bank < 2; bank++) {
    // rotorAnt of 0 means "not configured", not the OFF button (which is also
    // numbered 0) - guard on >= 1 so an unconfigured rotor antenna never
    // outlines OFF. No TRX check: the same antenna number is one physical
    // port shared by both rows, so both get the outline.
    const bankState = ant.banks && ant.banks[bank];
    // Real, asymmetric flag from ant-sw-2x6's own updateCollisions()
    // (port[i][3], via /?J's 5th/6th field) - only the TRX that actually
    // lost the collision (its output forced off) lights up red, not both.
    // Used to be derived here from active[0] === active[1], which can't
    // tell winner from loser and marked both TRX every time.
    const collided = !!(bankState && bankState.col);
    $('antButtons' + bank).querySelectorAll('button').forEach((b) => {
      const isActive = Number(b.dataset.ant) === active[bank];
      b.classList.toggle('on', isActive);
      b.classList.toggle('collision', isActive && collided);
      b.classList.toggle('rotor', rotorAnt >= 1 && Number(b.dataset.ant) === rotorAnt);
    });
    $('pwrBtn' + bank).classList.toggle('on', !!(bankState && bankState.pwr));
  }
}

// --- monitor (rotctld/raw/antenna switch/controller traffic) ---------------
// Session colour comes from the same FAV_COLORS palette as the favourites,
// assigned the first time a given stream+slot pair is seen and kept for that
// connection's lifetime. Direction (see --mon-tx/--mon-rx in style.css) is a
// separate dimension on purpose - "who said this" and "which way" are two
// different questions, and one colour cannot answer both at once.
const monSessionColors = new Map();
const kMonMaxLines = 500;  // a live tail, not an unbounded log

function monSessionColor(key) {
  if (!monSessionColors.has(key)) {
    monSessionColors.set(key, favColor(monSessionColors.size));
  }
  return monSessionColors.get(key);
}

function appendDebugLog(entries, overflow) {
  if (!Array.isArray(entries) || entries.length === 0) {
    return;
  }
  const log = $('monLog');
  const atBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 40;

  const html = entries.map((e) => {
    const color = monSessionColor(`${e.proto}#${e.session}`);
    const dirClass = e.dir === 'tx' ? 'mon-tx' : 'mon-rx';
    const arrow = e.dir === 'tx' ? '→' : '←';
    const time = new Date().toISOString().substring(11, 19);
    const label = e.label ? ' ' + escapeHtml(e.label) : '';
    return `<div class="mon-line ${dirClass}">` +
      `<span class="mon-time">${time}</span>` +
      `<span class="mon-badge" style="background:${color}">${escapeHtml(e.proto)} #${e.session}${label}</span>` +
      `<span class="mon-text">${arrow} ${escapeHtml(e.text)}</span></div>`;
  }).join('') + (overflow ? '<div class="mon-line mon-empty">… część linii pominięta (zbyt dużo naraz)</div>' : '');

  log.insertAdjacentHTML('beforeend', html);
  while (log.childElementCount > kMonMaxLines) {
    log.removeChild(log.firstElementChild);
  }
  if (atBottom) {
    log.scrollTop = log.scrollHeight;
  }
}

$('monClear').onclick = () => { $('monLog').innerHTML = ''; };

// UTC, and labelled as such. A rotator log is compared against schedules,
// beacons and other stations' logs, all of which are in UTC; showing browser
// local time invites an off-by-an-hour that nothing on screen would reveal.
setInterval(() => {
  $('clock').textContent = new Date().toISOString().substring(11, 19);
}, 1000);

// --- websocket -------------------------------------------------------------

function connectSocket() {
  // Match the page scheme: wss when the panel is served over https (e.g. via a
  // TLS-terminating reverse proxy), ws otherwise. The session cookie rides
  // along in the handshake, so no token is sent in-band.
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  socket = new WebSocket(`${proto}://${location.host}/ws`);
  socket.onmessage = (e) => render(JSON.parse(e.data));
  socket.onclose = () => {
    // The dead-man timer is about to stop the rotator anyway; show that rather
    // than leaving the buttons looking live.
    jogHeld = null;
    $('linkDot').className = 'dot bad';
    // Mid-update, a closed socket almost always means the bridge is rebooting
    // into the new version - which also wipes every session, so reconnecting
    // the socket would only be refused. Wait for it and reload instead.
    if (updWatching) { waitForReboot(); return; }
    setTimeout(connectSocket, 1500);
  };
}

// --- controller line hand-over (ControllerPort.h) ---------------------------
// The bridge shares the Nano's D0/D1 with its USB-serial chip, which blocks a
// USB upload to the controller. While released the bridge sends nothing and
// keeps GPIO17 high-impedance; the banner says so on every open panel, since
// rotctld/raw clients are cut off for as long as it lasts.

function renderLine(c) {
  const line = c.line || 'attached';
  const banner = $('lineBanner');
  banner.hidden = line === 'attached';
  if (line === 'releasing') {
    $('lineBannerText').innerHTML = 'Zwalnianie linii do sterownika…';
  } else if (line === 'released') {
    const min = Math.ceil((c.autoAttachInS || 0) / 60);
    $('lineBannerText').innerHTML = 'Linia do sterownika zwolniona — można wgrywać firmware przez USB' +
      `<small>${c.releasedBy ? escapeHtml(String(c.releasedBy)) + ' · ' : ''}` +
      `rotator nie przyjmuje poleceń · powrót automatycznie za ${min} min</small>`;
  }
  $('lineAttachBtn').hidden = line !== 'released';
  $('lineBtn').textContent = line === 'attached' ? 'Zwolnij linię do sterownika' : 'Przywróć łącze ze sterownikiem';
  $('lineBtn').className = line === 'attached' ? 'ghost' : 'primary';
  $('lineBtn').disabled = line === 'releasing';
}

$('lineBtn').onclick = async () => {
  const line = state && state.controller.line;
  if (line === 'released') { await post('/api/controller/attach'); return; }
  if (!confirm('Zwolnić linię do sterownika?\n\nMost zatrzyma rotator i przestanie z nim rozmawiać — ' +
    'panel, rotctld i raw nie będą nim sterować. Wgraj firmware sterownika przez jego USB, potem kliknij ' +
    '„Przywróć łącze” (albo poczekaj 15 min / zrestartuj most).')) return;
  await post('/api/controller/release');
};
$('lineAttachBtn').onclick = () => post('/api/controller/attach');

// --- updates from GitHub Releases --------------------------------------------
// The bridge checks for a newer release on its own (Updater.h) and its state
// rides every status broadcast (s.update). Installing always needs an
// operator to accept it here; from then on every open panel - not just this
// one - shows the progress overlay, since the reboot at the end hits them all.

const INSTALL_PHASES = ['panel', 'firmware', 'writing', 'rebooting'];
const PHASE_TEXT = {
  panel: ['Pobieranie panelu', 1],
  firmware: ['Pobieranie i zapis firmware', 2],
  writing: ['Zapis panelu', 3],
  rebooting: ['Restart mostu', 3],
};
let updDetails = null;        // GET /api/update - release notes, sizes
let updPrevPhase = null;
let updPrevLatest = null;
let updWatching = false;      // this tab saw an install running - reload once the bridge is back
let updFailClosed = null;     // the failure text the operator already dismissed

// sessionStorage, not localStorage: "hide until next login", and a reload
// after the update must still know which version it was waiting for.
function sessionGet(key) { try { return sessionStorage.getItem(key); } catch (e) { return null; } }
function sessionSet(key, value) {
  try { if (value === null) sessionStorage.removeItem(key); else sessionStorage.setItem(key, value); } catch (e) { /* private mode */ }
}

// Read once, at page load: only a reload *after* the install (see
// waitForReboot) may judge whether it took - the tab that just clicked
// "Zainstaluj" has the key set too, before the bridge even reports progress.
const updTargetAtLoad = sessionGet('updTarget');
let updResultShown = false;

function agoText(s) {
  if (s < 60) return 'przed chwilą';
  if (s < 3600) return `${Math.floor(s / 60)} min temu`;
  return `${Math.floor(s / 3600)} h temu`;
}

function formatSize(bytes) { return (bytes / 1024 / 1024).toFixed(2) + ' MB'; }

// GitHub release notes are markdown. Just enough of it to read well - headings
// and **bold** as bold, list dashes as bullets - over already-escaped text, so
// nothing from the release can inject markup.
function renderNotes(markdown) {
  return escapeHtml(markdown)
    .replace(/^#{1,6}\s*(.+)$/gm, '<strong>$1</strong>')
    .replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>')
    .replace(/^\s*[-*]\s+/gm, '• ');
}

async function loadUpdateDetails() {
  const d = await getJson('/api/update');
  if (!d) return;
  updDetails = d;
  const notes = $('updNotes');
  if (!d.latest) { notes.hidden = true; return; }
  const date = d.publishedAt ? d.publishedAt.substring(0, 10) : '';
  const size = d.firmwareSize ? `firmware ${formatSize(d.firmwareSize)}` +
    (d.panelSize ? `, panel ${formatSize(d.panelSize)}` : ', bez panelu') : '';
  notes.innerHTML =
    `<div class="upd-notes-head">${escapeHtml(String(d.name || d.latest))}` +
    `<small>${escapeHtml(date)}${size ? ' · ' + escapeHtml(size) : ''}</small></div>` +
    renderNotes(String(d.notes || 'Brak opisu wydania.').trim()) +
    (d.url ? `<a href="${escapeHtml(String(d.url))}" target="_blank" rel="noopener">Pokaż na GitHubie</a>` : '');
  notes.hidden = false;
}

function renderUpdate(u) {
  if (!u) return;
  const installing = INSTALL_PHASES.includes(u.phase);

  // After a reload following an update this tab started: did it take?
  const target = updTargetAtLoad;
  const banner = $('updBanner');
  if (target && !installing && !updResultShown) {
    updResultShown = true;
    sessionSet('updTarget', null);
    banner.className = 'banner upd-banner ' + (u.current === target ? 'done' : 'failed');
    $('updBannerText').innerHTML = u.current === target
      ? `Zaktualizowano do <b>${escapeHtml(u.current)}</b>`
      : `Aktualizacja do ${escapeHtml(target)} nie powiodła się<small>działa ${escapeHtml(u.current)}` +
        `${u.rolledBack ? ' — nowa wersja nie wystartowała i została wycofana' : ''}</small>`;
    $('updBannerShow').hidden = true;
    banner.hidden = false;
    banner.dataset.sticky = '1';
  } else if (!banner.dataset.sticky) {
    // A local/dev build gets no nagging banner - it is usually ahead of the
    // release it would be compared with - only the tile below.
    const show = u.available && !u.dev && !installing && sessionGet('updHide') !== u.latest;
    banner.className = 'banner upd-banner';
    banner.hidden = !show;
    if (show) {
      $('updBannerText').innerHTML = `Dostępna nowa wersja <b>${escapeHtml(u.latest)}</b>` +
        `<small>zainstalowana: ${escapeHtml(u.current)}</small>`;
      $('updBannerShow').hidden = false;
    }
  }

  $('updCurrent').textContent = u.current + (u.dev ? ' (lokalna)' : '');
  $('updLatest').textContent = u.latest || (u.noReleases ? 'brak wydań' : '—');
  $('updChecked').textContent = u.phase === 'checking' ? 'sprawdzanie…'
    : (u.checkedAgoS !== undefined ? agoText(u.checkedAgoS) : 'jeszcze nie');

  const info = [];
  if (u.rolledBack) info.push('Poprzednia aktualizacja nie wystartowała i most wrócił do wcześniejszej wersji.');
  if (u.dev && u.latest) info.push('To wersja zbudowana lokalnie — powiadomienia są wyłączone, ale wydanie można zainstalować.');
  $('updInfo').hidden = info.length === 0;
  $('updInfo').textContent = info.join(' ');

  const err = $('updErr');
  err.hidden = !(u.error && u.phase === 'idle');
  if (!err.hidden) err.textContent = 'Sprawdzanie nie powiodło się: ' + u.error;

  const install = $('updInstallBtn');
  install.hidden = !u.latest || installing || u.phase === 'checking';
  if (u.latest) {
    install.textContent = u.relation === 'same' ? `Zainstaluj ponownie ${u.latest}`
      : u.relation === 'older' ? `Zainstaluj ${u.latest} (starsza)` : `Zainstaluj ${u.latest}`;
    install.className = (u.relation === 'newer' || u.relation === 'unknown') ? 'primary' : 'ghost';
  }
  $('updCheckBtn').disabled = u.phase === 'checking' || installing;

  // Notes follow the release: refetch when a check finishes or the tag changes.
  if ((updPrevPhase === 'checking' && u.phase !== 'checking') || u.latest !== updPrevLatest) {
    loadUpdateDetails();
  }
  updPrevPhase = u.phase;
  updPrevLatest = u.latest;

  renderUpdateOverlay(u, installing);
}

function renderUpdateOverlay(u, installing) {
  if (installing) updWatching = true;
  const failed = u.phase === 'failed' && updWatching && updFailClosed !== u.error;
  $('updOverlay').hidden = !(installing || failed);
  if (!installing && !failed) return;

  const bar = $('updOvBar');
  $('updOvClose').hidden = !failed;
  if (failed) {
    $('updOvTitle').textContent = 'Aktualizacja nie powiodła się';
    $('updOvText').textContent = u.error;
    $('updOvHint').textContent = `Most zostaje przy ${u.current}, rotator jest znowu dostępny.`;
    bar.className = 'progress-bar bad';
    bar.style.width = '100%';
    return;
  }
  const [label, step] = PHASE_TEXT[u.phase];
  const pct = u.phase === 'rebooting' ? 100 : (u.total ? Math.floor(u.done * 100 / u.total) : 0);
  $('updOvTitle').textContent = `Aktualizacja do ${u.latest || 'nowej wersji'}`;
  $('updOvText').textContent = `${label} — ${u.phase === 'rebooting' ? 'za chwilę' : pct + '%'} (krok ${step} z 3)`;
  $('updOvHint').textContent = (u.by ? `Uruchomił: ${u.by}. ` : '') +
    'Nie odłączaj zasilania. Rotator jest zablokowany do restartu, potem zaloguj się ponownie.';
  bar.className = 'progress-bar';
  bar.style.width = pct + '%';
}

// Polls until the bridge answers again after its reboot, then reloads - which
// lands on the login screen, since a reboot ends every session.
let rebootWaitStarted = 0;
async function waitForReboot() {
  if (!rebootWaitStarted) rebootWaitStarted = Date.now();
  $('updOverlay').hidden = false;
  $('updOvTitle').textContent = 'Restart mostu';
  $('updOvText').textContent = 'Czekam, aż most uruchomi się z nową wersją…';
  $('updOvBar').style.width = '100%';
  try {
    const r = await fetch('/api/session', { cache: 'no-store' });
    if (r.ok) { location.reload(); return; }
  } catch (e) { /* still rebooting */ }
  if (Date.now() - rebootWaitStarted > 180000) {
    $('updOvText').textContent = 'Most nie odpowiada od 3 minut — sprawdź zasilanie i odśwież stronę.';
  }
  setTimeout(waitForReboot, 2000);
}

$('updBannerShow').onclick = () => {
  document.querySelector('.tab[data-tab=settings]').click();
  openTile($('updTile'));
  $('updTile').scrollIntoView({ block: 'nearest' });
};
$('updBannerHide').onclick = () => {
  const banner = $('updBanner');
  if (!banner.dataset.sticky && state && state.update) sessionSet('updHide', state.update.latest);
  delete banner.dataset.sticky;
  banner.hidden = true;
};

$('updCheckBtn').onclick = async () => {
  $('updErr').hidden = true;
  const res = await post('/api/update/check');
  if (!res.ok) {
    $('updErr').hidden = false;
    $('updErr').textContent = res.data.error || 'Błąd';
  }
};

$('updInstallBtn').onclick = async () => {
  const u = state && state.update;
  if (!u || !u.latest) return;
  const ok = confirm(`Zainstalować ${u.latest}? (teraz: ${u.current})\n\n` +
    'Rotator zostanie zatrzymany i zablokowany na czas aktualizacji. Most pobierze firmware i panel ' +
    'z GitHuba, zapisze je i zrestartuje się (ok. 1–2 min) — klienci rotctld/raw muszą połączyć się ponownie, ' +
    'a wszyscy zalogowani zalogować od nowa.');
  if (!ok) return;
  $('updErr').hidden = true;
  const res = await post('/api/update/install', { tag: u.latest });
  if (!res.ok) {
    $('updErr').hidden = false;
    $('updErr').textContent = res.data.error || 'Błąd';
    return;
  }
  updWatching = true;
  sessionSet('updTarget', u.latest);
};

$('updOvClose').onclick = () => {
  updFailClosed = state && state.update ? state.update.error : null;
  updWatching = false;
  sessionSet('updTarget', null);  // seen here already - no second report on a later reload
  $('updOverlay').hidden = true;
};

$('cfgUpdateAuto').onchange = async () => {
  if (await saveConfig($('updCfgErr'), $('updCfgOk'))) $('updCfgOk').textContent = 'Zapisano';
};

// --- jog with keepalive ----------------------------------------------------

function startJog(direction) {
  if (jogHeld === direction) return;
  jogHeld = direction;
  const send = () => {
    if (jogHeld && socket && socket.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify({ jog: jogHeld }));
    }
  };
  send();
  clearInterval(jogTimer);
  jogTimer = setInterval(send, 200);
}

function endJog() {
  if (!jogHeld) return;
  jogHeld = null;
  clearInterval(jogTimer);
  if (socket && socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify({ jog: 'stop' }));
}

// --- favourites ------------------------------------------------------------

function renderFavorites() {
  $('favBody').innerHTML = favorites.map((f, i) => `
    <tr>
      <td><span class="fav-swatch" style="background:${favColor(i)}"></span></td>
      <td><input type="text" value="${f.name}" data-name="${i}" maxlength="19"></td>
      <td class="right"><input type="number" value="${Math.round(f.az)}" min="0" max="359" data-az="${i}"></td>
      <td><button class="primary" data-go="${i}">▶</button></td>
      <td><button class="danger" data-del="${i}">×</button></td>
    </tr>`).join('');

  $('favBody').querySelectorAll('[data-go]').forEach((b) => {
    b.onclick = () => post('/api/goto', { az: favorites[b.dataset.go].az });
  });
  $('favBody').querySelectorAll('[data-del]').forEach((b) => {
    b.onclick = () => { favorites.splice(b.dataset.del, 1); favDirty = true; renderFavorites(); };
  });
  $('favBody').querySelectorAll('input').forEach((input) => {
    input.oninput = () => {
      const i = input.dataset.name !== undefined ? input.dataset.name : input.dataset.az;
      if (input.dataset.name !== undefined) favorites[i].name = input.value;
      else favorites[i].az = Number(input.value) || 0;
      favDirty = true;
      $('favSave').hidden = false;
      renderFavMarks();
    };
  });

  $('favSave').hidden = !favDirty;
  renderFavMarks();
}

async function loadFavorites() {
  favorites = (await getJson('/api/favorites')) || [];
  favDirty = false;
  renderFavorites();
}

// --- settings --------------------------------------------------------------

async function loadConfig() {
  const cfg = await getJson('/api/config');
  if (!cfg) return;
  $('cfgSite').value = cfg.siteName || '';
  setBrand(cfg.siteName);
  $('cfgHost').value = cfg.hostname || '';
  $('cfgRotctld').value = cfg.rotctldPort;
  $('cfgRaw').value = cfg.rawPort;
  $('cfgRotctldMax').value = cfg.rotctldMaxClients;
  $('cfgRotctldMax').max = cfg.rotctldCeiling;
  $('capRotctld').textContent = `(sprzętowy limit: ${cfg.rotctldCeiling})`;
  $('cfgRawMax').value = cfg.rawMaxClients;
  $('cfgRawMax').max = cfg.rawCeiling;
  $('capRaw').textContent = `(sprzętowy limit: ${cfg.rawCeiling})`;
  $('cfgBaud').value = String(cfg.serialBaud);
  $('cfgOvFrom').value = cfg.overlapFrom;
  $('cfgOvTo').value = cfg.overlapTo;
  $('cfgAntEnabled').checked = !!cfg.antEnabled;
  $('cfgAntHost').value = cfg.antHost || '';
  rotorAnt = Number(cfg.rotorAnt) || 0;
  $('cfgRotorAnt').value = String(rotorAnt);

  $('cfgDebugEnabled').checked = !!cfg.debugEnabled;
  $('cfgDebugRotctld').checked = !!cfg.debugRotctld;
  $('cfgDebugRaw').checked = !!cfg.debugRaw;
  $('cfgDebugAntenna').checked = !!cfg.debugAntenna;
  $('cfgDebugController').checked = !!cfg.debugController;
  $('monitorSection').hidden = !cfg.debugEnabled;
  $('cfgUpdateAuto').checked = cfg.updateAutoCheck !== false;
}

// --- start-up --------------------------------------------------------------

// The panel name is configurable; it labels the top bar, the login screen and
// the browser tab.
function setBrand(name) {
  const n = name || 'RotorBridge';
  $('brand').textContent = n;
  $('loginBrand').textContent = n;
  document.title = n;
}

async function enterApp() {
  $('login').hidden = true;
  $('app').hidden = false;
  buildDial();
  buildAntennaButtons();
  connectSocket();
  await loadFavorites();
  await loadConfig();
  await loadUsers();
  await loadWifiNetworks();
}

// The login screen picks an account from a dropdown (populated from
// /api/session's account list) rather than typing a name - each account has
// its own "needs first-run setup" state, so the form must react to whichever
// one is currently selected, not a single global flag.
function updateLoginMode() {
  const info = knownUsers.find((u) => u.name === $('loginUser').value);
  const needsSetup = !!(info && info.needsSetup);
  $('loginSub').textContent = needsSetup ? 'Pierwsze uruchomienie — ustaw hasło (min. 8 znaków)' : 'Zaloguj się';
  $('loginBtn').textContent = needsSetup ? 'Ustaw hasło' : 'Zaloguj';
  if (needsSetup) $('loginForm').dataset.mode = 'setup';
  else delete $('loginForm').dataset.mode;
}

async function init() {
  const session = await getJson('/api/session');
  if (session && session.siteName) setBrand(session.siteName);
  if (session && Array.isArray(session.users)) {
    knownUsers = session.users;
    $('loginUser').innerHTML = knownUsers.map((u) =>
      `<option value="${escapeHtml(u.name)}">${escapeHtml(u.name)}</option>`).join('');
    updateLoginMode();
  }
  if (session && session.authenticated) {
    myUser = session.user || null;
    await enterApp();
  }
}

$('loginUser').onchange = updateLoginMode;

$('loginForm').onsubmit = async (event) => {
  event.preventDefault();
  const user = $('loginUser').value, password = $('loginPass').value;
  const err = $('loginErr');
  err.hidden = true;
  $('forceBtn').hidden = true;

  if ($('loginForm').dataset.mode === 'setup') {
    const res = await post('/api/setup', { user, password });
    if (!res.ok) { err.hidden = false; err.textContent = res.data.error || 'Błąd'; return; }
    delete $('loginForm').dataset.mode;
    $('loginBtn').textContent = 'Zaloguj';
  }

  const res = await post('/api/login', { user, password });
  if (res.ok) { myUser = user; await enterApp(); return; }

  err.hidden = false;
  if (res.status === 409) {
    err.textContent = `Sesja zajęta z adresu ${res.data.sessionAddress}`;
    $('forceBtn').hidden = false;
  } else if (res.status === 429) {
    err.textContent = `Zbyt wiele prób — odczekaj ${Math.ceil(res.data.retryAfterMs / 1000)} s`;
  } else {
    err.textContent = res.data.error || 'Błędne dane logowania';
  }
};

// --- user accounts -----------------------------------------------------
// Flat, like the rest of Settings: any logged-in operator can reset or
// remove any account, including their own - there is no separate admin role.

function renderUsersTable(users) {
  knownUsers = users;
  $('usersBody').innerHTML = users.map((u) => `
    <tr>
      <td>${escapeHtml(u.name)}${u.needsSetup ? ' <span class="cap">(bez hasła)</span>' : ''}</td>
      <td><input type="password" placeholder="bez zmian" maxlength="63"></td>
      <td><button type="button" class="ghost small" data-reset="${escapeHtml(u.name)}">Ustaw</button></td>
      <td><button type="button" class="danger small" data-del="${escapeHtml(u.name)}"${users.length <= 1 ? ' disabled' : ''}>Usuń</button></td>
    </tr>`).join('');

  $('usersBody').querySelectorAll('[data-reset]').forEach((b) => {
    b.onclick = async () => {
      const password = b.closest('tr').querySelector('input[type=password]').value;
      $('usersErr').hidden = true;
      $('usersOk').hidden = true;
      const res = await post('/api/users', { name: b.dataset.reset, password });
      if (res.ok) {
        $('usersOk').hidden = false;
        $('usersOk').textContent = 'Zapisano';
        renderUsersTable(res.data);
      } else {
        $('usersErr').hidden = false;
        $('usersErr').textContent = res.data.error || 'Błąd';
      }
    };
  });
  $('usersBody').querySelectorAll('[data-del]').forEach((b) => {
    b.onclick = async () => {
      $('usersErr').hidden = true;
      $('usersOk').hidden = true;
      const res = await post('/api/users/delete', { name: b.dataset.del });
      if (res.ok) {
        renderUsersTable(res.data);
      } else {
        $('usersErr').hidden = false;
        $('usersErr').textContent = res.data.error || 'Błąd';
      }
    };
  });
}

async function loadUsers() {
  const users = await getJson('/api/users');
  if (users) renderUsersTable(users);
}

$('userAddBtn').onclick = async () => {
  const name = $('newUserName').value, password = $('newUserPass').value;
  $('usersErr').hidden = true;
  $('usersOk').hidden = true;
  const res = await post('/api/users', { name, password });
  if (res.ok) {
    $('newUserName').value = '';
    $('newUserPass').value = '';
    $('usersOk').hidden = false;
    $('usersOk').textContent = 'Dodano';
    renderUsersTable(res.data);
  } else {
    $('usersErr').hidden = false;
    $('usersErr').textContent = res.data.error || 'Błąd';
  }
};

// --- WiFi networks -----------------------------------------------------------
// Priority list (index 0 tried first) the bridge walks on every (re)connect
// before falling back to its own AP - see Net.cpp. Passwords are write-only,
// same as account passwords above: the list only ever shows SSIDs.

function renderWifiNetworksTable(networks) {
  $('wifiNetworksBody').innerHTML = networks.map((n) => `
    <tr>
      <td>${escapeHtml(n.ssid)}</td>
      <td><button type="button" class="danger small" data-wifidel="${escapeHtml(n.ssid)}">Usuń</button></td>
    </tr>`).join('');

  $('wifiNetworksBody').querySelectorAll('[data-wifidel]').forEach((b) => {
    b.onclick = async () => {
      $('wifiErr').hidden = true;
      $('wifiOk').hidden = true;
      const res = await post('/api/wifi/networks/delete', { ssid: b.dataset.wifidel });
      if (res.ok) {
        renderWifiNetworksTable(res.data);
      } else {
        $('wifiErr').hidden = false;
        $('wifiErr').textContent = res.data.error || 'Błąd';
      }
    };
  });
}

async function loadWifiNetworks() {
  const networks = await getJson('/api/wifi/networks');
  if (networks) renderWifiNetworksTable(networks);
}

// Bars fill by RSSI (roughly -90..-30 dBm -> 1..4 bars), same rough scale as
// the phone/laptop WiFi picker an operator already reads instinctively.
function wifiBars(rssi) {
  const n = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;
  return '▂▄▆█'.slice(0, n).padEnd(4, ' ');
}

let wifiScanPolling = false;

async function pollWifiScan() {
  const res = await getJson('/api/wifi/scan');
  if (!res) {
    wifiScanPolling = false;
    return;
  }
  if (res.status === 'scanning') {
    if (wifiScanPolling) setTimeout(pollWifiScan, 1000);
    return;
  }
  wifiScanPolling = false;
  const networks = (res.networks || []).slice().sort((a, b) => b.rssi - a.rssi);
  $('wifiScanResults').innerHTML = networks.map((n) => `
    <button type="button" class="ghost small" data-pick="${escapeHtml(n.ssid)}">
      ${n.secure ? '🔒' : ''} ${escapeHtml(n.ssid)} <span class="cap">${wifiBars(n.rssi)}</span>
    </button>`).join('') || '<span class="hint">Nic w zasięgu</span>';
  $('wifiScanResults').querySelectorAll('[data-pick]').forEach((b) => {
    b.onclick = () => {
      $('newWifiSsid').value = b.dataset.pick;
      $('newWifiPass').focus();
    };
  });
}

$('wifiScanBtn').onclick = () => {
  $('wifiScanResults').innerHTML = '<span class="hint">Skanowanie…</span>';
  wifiScanPolling = true;
  pollWifiScan();
};

$('wifiAddBtn').onclick = async () => {
  const ssid = $('newWifiSsid').value, password = $('newWifiPass').value;
  $('wifiErr').hidden = true;
  $('wifiOk').hidden = true;
  const res = await post('/api/wifi/networks', { ssid, password });
  if (res.ok) {
    $('newWifiSsid').value = '';
    $('newWifiPass').value = '';
    $('wifiOk').hidden = false;
    $('wifiOk').textContent = 'Dodano — zmiany po restarcie';
    renderWifiNetworksTable(res.data);
  } else {
    $('wifiErr').hidden = false;
    $('wifiErr').textContent = res.data.error || 'Błąd';
  }
};

$('forceBtn').onclick = async () => {
  const res = await post('/api/login',
    { user: $('loginUser').value, password: $('loginPass').value, force: '1' });
  if (res.ok) await enterApp();
};

$('dial').onclick = (event) => {
  const rect = $('dial').getBoundingClientRect();
  const x = (event.clientX - rect.left) / rect.width * 440 - C;
  const y = (event.clientY - rect.top) / rect.height * 440 - C;
  if (Math.hypot(x, y) < 55) return;   // the hub is a readout, not a target
  let deg = Math.atan2(x, -y) * 180 / Math.PI;
  if (deg < 0) deg += 360;
  post('/api/goto', { az: Math.round(deg) });
};

$('gotoForm').onsubmit = (e) => { e.preventDefault(); post('/api/goto', { az: $('gotoInput').value }); };
$('stopBtn').onclick = () => { endJog(); post('/api/stop'); };

for (const [id, dir] of [['cwBtn', 'cw'], ['ccwBtn', 'ccw']]) {
  const b = $(id);
  b.onpointerdown = (e) => { e.preventDefault(); startJog(dir); };
  b.onpointerup = endJog;
  b.onpointerleave = endJog;
  b.onpointercancel = endJog;
}

document.addEventListener('keydown', (e) => {
  if (e.repeat || e.target.tagName === 'INPUT') return;
  if (e.key === 'ArrowRight') startJog('cw');
  if (e.key === 'ArrowLeft') startJog('ccw');
  if (e.key === 'Escape') { endJog(); post('/api/stop'); }
});
document.addEventListener('keyup', (e) => {
  if (e.key === 'ArrowRight' || e.key === 'ArrowLeft') endJog();
});
// Losing focus with a key down would otherwise leave the jog latched.
window.addEventListener('blur', endJog);

document.querySelectorAll('.tab').forEach((tab) => {
  tab.onclick = () => {
    document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t === tab));
    $('tab-controller').hidden = tab.dataset.tab !== 'controller';
    $('tab-settings').hidden = tab.dataset.tab !== 'settings';
  };
});

// One tile open at a time - with everything expanded the settings page is a
// wall of inputs and the headings stop working as a menu.
function openTile(card) {
  document.querySelectorAll('.collapse').forEach((c) => c.classList.toggle('open', c === card));
}
document.querySelectorAll('.collapse-head').forEach((head) => {
  head.onclick = () => {
    const card = head.parentElement;
    openTile(card.classList.contains('open') ? null : card);
  };
});

// The dial's reset button is a shortcut straight to calibration, so it opens
// that tile rather than dropping the operator on a closed list. Found via
// syncAz's own ancestor, not "the first .collapse" - the tiles have been
// reordered before and silently opening the wrong one is an easy regression
// to miss, since this button still "works", just on the wrong tile.
$('syncOpen').onclick = () => {
  document.querySelector('.tab[data-tab=settings]').click();
  openTile($('syncAz').closest('.collapse'));
  if (state) $('syncAz').value = Math.round(state.position.azimuth);
};

$('favAdd').onclick = () => {
  if (favorites.length >= 9) return;
  favorites.push({ name: 'Nowy', az: state ? Math.round(state.position.azimuth) : 0 });
  favDirty = true;
  renderFavorites();
};
$('favSave').onclick = async () => {
  await fetch('/api/favorites', { method: 'POST', body: JSON.stringify(favorites) });
  await loadFavorites();
};

$('syncBtn').onclick = () => post('/api/sync', { az: $('syncAz').value });

// Shared by every Save button in Settings: the config is one struct saved as a
// whole, so any tile's button submits all fields - only the feedback
// (err/ok elements) shown next to the button that was actually clicked differs.
async function saveConfig(errEl, okEl) {
  const params = {
    hostname: $('cfgHost').value, siteName: $('cfgSite').value,
    rotctldPort: $('cfgRotctld').value, rawPort: $('cfgRaw').value,
    rotctldMaxClients: $('cfgRotctldMax').value, rawMaxClients: $('cfgRawMax').value,
    serialBaud: $('cfgBaud').value,
    overlapFrom: $('cfgOvFrom').value, overlapTo: $('cfgOvTo').value,
    antEnabled: $('cfgAntEnabled').checked ? '1' : '0', antHost: $('cfgAntHost').value,
    rotorAnt: $('cfgRotorAnt').value,
    debugEnabled: $('cfgDebugEnabled').checked ? '1' : '0',
    debugRotctld: $('cfgDebugRotctld').checked ? '1' : '0',
    debugRaw: $('cfgDebugRaw').checked ? '1' : '0',
    debugAntenna: $('cfgDebugAntenna').checked ? '1' : '0',
    debugController: $('cfgDebugController').checked ? '1' : '0',
    updateAutoCheck: $('cfgUpdateAuto').checked ? '1' : '0',
  };
  if ($('cfgApPass').value) params.apPassword = $('cfgApPass').value;

  const res = await post('/api/config', params);
  errEl.hidden = res.ok;
  okEl.hidden = !res.ok;
  if (res.ok) okEl.textContent = 'Zapisano — zmiany po restarcie';
  else errEl.textContent = res.data.error || 'Błąd zapisu';
  return res.ok;
}

$('cfgSave').onclick = () => saveConfig($('cfgErr'), $('cfgOk'));
$('netSave').onclick = () => saveConfig($('netErr'), $('netOk'));
$('antCfgSave').onclick = async () => {
  // rotorAnt drives the outline drawn on the Anteny card - update it right
  // away rather than waiting for a reload, same idea as the Monitor tab
  // checkbox below.
  if (await saveConfig($('antCfgErr'), $('antCfgOk'))) {
    rotorAnt = Number($('cfgRotorAnt').value) || 0;
  }
};
$('debugCfgSave').onclick = async () => {
  // The Monitor section's own visibility does not need the restart the other
  // fields on this shared save do - update it immediately rather than
  // leaving the operator to guess whether they need to reload.
  if (await saveConfig($('debugCfgErr'), $('debugCfgOk'))) {
    $('monitorSection').hidden = !$('cfgDebugEnabled').checked;
  }
};

$('fwBtn').onclick = async () => {
  const file = $('fwFile').files[0];
  const status = $('fwStatus');
  status.hidden = false;
  if (!file) { status.textContent = 'Wybierz plik'; return; }
  status.textContent = 'Wgrywanie…';

  const body = new FormData();
  body.append('target', $('fwTarget').value);
  body.append('file', file);
  try {
    const res = await fetch('/api/update', { method: 'POST', body });
    const data = await res.json().catch(() => ({}));
    status.textContent = res.ok ? 'Wgrano — restart' : ('Błąd: ' + (data.error || res.status));
  } catch (e) {
    // The bridge reboots on success, so a dropped connection here is expected.
    status.textContent = 'Restart w toku…';
  }
};

// Every tile that saves config settling in only after a reboot gets its own
// Restart button right next to Save, so the operator does not have to jump to
// the System tile to apply what they just saved.
function restartDevice() { return post('/api/restart'); }
$('restartBtn').onclick = restartDevice;
$('cfgRestartBtn').onclick = restartDevice;
$('netRestartBtn').onclick = restartDevice;
$('antRestartBtn').onclick = restartDevice;
$('debugRestartBtn').onclick = restartDevice;
$('logoutBtn').onclick = async () => { await post('/api/logout'); location.reload(); };

init();
