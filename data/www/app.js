'use strict';

const $ = (id) => document.getElementById(id);
let token = null;        // returned by the cookie flow, echoed to the WebSocket
let socket = null;
let state = null;
let jogHeld = null;      // 'cw' | 'ccw' while a key or button is down
let jogTimer = null;
let favorites = [];

// --- helpers ---------------------------------------------------------------

async function post(path, params) {
  const body = new URLSearchParams(params || {});
  const res = await fetch(path, { method: 'POST', body });
  return { ok: res.ok, status: res.status, data: await res.json().catch(() => ({})) };
}

async function getJson(path) {
  const res = await fetch(path);
  return res.ok ? res.json() : null;
}

// --- dial ------------------------------------------------------------------

const R = 200;

function polar(angleDeg, radius) {
  const a = (angleDeg - 90) * Math.PI / 180;
  return [R + radius * Math.cos(a), R + radius * Math.sin(a)];
}

function buildDial() {
  const parts = ['<circle class="face" cx="200" cy="200" r="170"/>',
                 '<circle class="ring" cx="200" cy="200" r="170"/>'];
  for (let deg = 0; deg < 360; deg += 10) {
    const major = deg % 30 === 0;
    const [x1, y1] = polar(deg, major ? 152 : 160);
    const [x2, y2] = polar(deg, 170);
    parts.push(`<line class="${major ? 'tick' : 'tick-minor'}" x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}"/>`);
  }
  [['N', 0], ['E', 90], ['S', 180], ['W', 270]].forEach(([label, deg]) => {
    const [x, y] = polar(deg, 128);
    parts.push(`<text class="label" x="${x}" y="${y}">${label}</text>`);
  });
  $('dialStatic').innerHTML = parts.join('');
}

function setNeedle(azimuth) {
  const [x, y] = polar(azimuth, 148);
  $('needle').setAttribute('x2', x);
  $('needle').setAttribute('y2', y);
}

// The overlap zone gets its own arc outside the ring, drawn from 0 up to how
// far past 360 the rotator currently is. Without it, two mechanically distinct
// positions look identical on the dial.
function setOverlap(raw, rawMax) {
  const arc = $('overlapArc');
  if (raw < 360) {
    arc.hidden = true;
    return;
  }
  const sweep = Math.min(raw - 360, rawMax - 360);
  const r = 186;
  const [sx, sy] = polar(0, r);
  const [ex, ey] = polar(sweep, r);
  const large = sweep > 180 ? 1 : 0;
  arc.outerHTML = `<path id="overlapArc" class="overlap-arc" d="M ${sx} ${sy} A ${r} ${r} 0 ${large} 1 ${ex} ${ey}"/>`;
}

function dialClickToAzimuth(event) {
  const svg = $('dial');
  const rect = svg.getBoundingClientRect();
  const x = (event.clientX - rect.left) / rect.width * 400 - R;
  const y = (event.clientY - rect.top) / rect.height * 400 - R;
  let deg = Math.atan2(x, -y) * 180 / Math.PI;
  if (deg < 0) deg += 360;
  return Math.round(deg);
}

// --- rendering -------------------------------------------------------------

function render(s) {
  state = s;

  $('azValue').textContent = s.position.fresh ? Math.round(s.position.azimuth) : '---';
  if (s.position.fresh) setNeedle(s.position.azimuth);
  setOverlap(s.position.raw, s.controller.rawMax);

  $('rawValue').textContent = 'raw ' + Math.round(s.position.raw);

  const fresh = $('freshValue');
  fresh.textContent = s.position.fresh ? 'na żywo' : 'brak danych';
  fresh.className = 'chip ' + (s.position.fresh ? 'live' : 'stale');

  // Not "is someone connected" but "why is it turning" - so the last motion
  // source is shown next to the heading, not buried in a status page.
  const motion = $('motionValue');
  if (s.lastMotion && s.lastMotion.ageMs < 60000) {
    motion.hidden = false;
    motion.textContent = 'ruch: ' + s.lastMotion.source;
  } else {
    motion.hidden = true;
  }

  const banner = $('banner');
  // A dead serial link outranks everything else: without it nothing else on
  // this page means anything.
  if (!s.controller.linkHealthy) {
    banner.hidden = false;
    banner.innerHTML = 'Brak łączności ze sterownikiem<span>sprawdź zasilanie i okablowanie</span>';
  } else if (s.sources.remoteConnected) {
    const bits = [];
    if (s.sources.rotctld.clients) bits.push(`rotctld ${s.sources.rotctld.addresses}`);
    if (s.sources.raw.clients) bits.push(`raw ${s.sources.raw.addresses}`);
    banner.hidden = false;
    banner.innerHTML = 'Zdalne sterowanie podłączone<span>' + bits.join(' • ') + '</span>';
  } else if (s.controller.bootLockout) {
    banner.hidden = false;
    banner.innerHTML = 'Sterownik po restarcie<span>komendy obrotu ignorowane przez 5 s</span>';
  } else if (s.controller.notice) {
    banner.hidden = false;
    banner.innerHTML = 'Sterownik: ' + s.controller.notice + '<span></span>';
  } else {
    banner.hidden = true;
  }

  $('ccwBtn').classList.toggle('active', s.jogging && jogHeld === 'ccw');
  $('cwBtn').classList.toggle('active', s.jogging && jogHeld === 'cw');

  $('footer').textContent =
    `${s.network.mode} · ${s.network.ssid} · ${s.network.address} · rotctld ${s.sources.rotctld.port} · raw ${s.sources.raw.port}`;
}

// --- websocket -------------------------------------------------------------

function connectSocket() {
  socket = new WebSocket(`ws://${location.host}/ws`);
  socket.onopen = () => socket.send(JSON.stringify({ token }));
  socket.onmessage = (event) => render(JSON.parse(event.data));
  socket.onclose = () => {
    // A dead socket means the dead-man timer is about to stop the rotator
    // anyway; reflect that rather than leaving the buttons looking live.
    jogHeld = null;
    setTimeout(connectSocket, 1500);
  };
}

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
  if (socket && socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify({ jog: 'stop' }));
  }
}

// --- favourites ------------------------------------------------------------

async function loadFavorites() {
  favorites = (await getJson('/api/favorites')) || [];
  renderFavorites();
}

function renderFavorites() {
  $('favRow').innerHTML = favorites
    .map((f, i) => `<button data-fav="${i}">${f.name}<span class="az">${Math.round(f.az)}°</span></button>`)
    .join('');
  $('favRow').querySelectorAll('[data-fav]').forEach((button) => {
    button.onclick = () => post('/api/goto', { az: favorites[button.dataset.fav].az });
  });

  $('favEdit').innerHTML = favorites
    .map((f, i) => `<div class="row"><input type="text" value="${f.name}" data-name="${i}">` +
                   `<input type="number" value="${Math.round(f.az)}" min="0" max="359" data-az="${i}">` +
                   `<button class="danger" data-del="${i}">×</button></div>`)
    .join('');
  $('favEdit').querySelectorAll('[data-del]').forEach((button) => {
    button.onclick = () => { favorites.splice(button.dataset.del, 1); renderFavorites(); };
  });
}

function collectFavorites() {
  return Array.from($('favEdit').querySelectorAll('.row')).map((row) => ({
    name: row.querySelector('input[type=text]').value.slice(0, 19),
    az: Number(row.querySelector('input[type=number]').value) || 0,
  }));
}

// --- settings --------------------------------------------------------------

async function loadConfig() {
  const cfg = await getJson('/api/config');
  if (!cfg) return;
  $('cfgSsid').value = cfg.wifiSsid || '';
  $('cfgHost').value = cfg.hostname || '';
  $('cfgRotctld').value = cfg.rotctldPort;
  $('cfgRaw').value = cfg.rawPort;
  $('cfgBaud').value = String(cfg.serialBaud);
}

// --- start-up --------------------------------------------------------------

async function enterApp() {
  $('login').hidden = true;
  $('app').hidden = false;
  buildDial();
  connectSocket();
  await loadFavorites();
  await loadConfig();
}

async function init() {
  buildDial();
  const session = await getJson('/api/session');

  if (session && session.setupRequired) {
    $('loginSub').textContent = 'Pierwsze uruchomienie — ustaw hasło (min. 8 znaków)';
    $('loginBtn').textContent = 'Ustaw hasło';
    $('loginForm').dataset.mode = 'setup';
  }
  if (session && session.authenticated) {
    token = 'cookie';
    await enterApp();
  }
}

$('loginForm').onsubmit = async (event) => {
  event.preventDefault();
  const user = $('loginUser').value;
  const password = $('loginPass').value;
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
  if (res.ok) {
    const session = await getJson('/api/session');
    token = session ? 'cookie' : null;
    await enterApp();
    return;
  }

  err.hidden = false;
  if (res.status === 409) {
    err.textContent = `Sesja zajęta z adresu ${res.data.sessionAddress}`;
    $('forceBtn').hidden = false;
  } else {
    err.textContent = res.data.error || 'Błędne dane logowania';
  }
};

$('forceBtn').onclick = async () => {
  const res = await post('/api/login',
    { user: $('loginUser').value, password: $('loginPass').value, force: '1' });
  if (res.ok) await enterApp();
};

$('dial').onclick = (event) => post('/api/goto', { az: dialClickToAzimuth(event) });

$('gotoForm').onsubmit = (event) => {
  event.preventDefault();
  post('/api/goto', { az: $('gotoInput').value });
};

$('stopBtn').onclick = () => { endJog(); post('/api/stop'); };

for (const [id, dir] of [['cwBtn', 'cw'], ['ccwBtn', 'ccw']]) {
  const button = $(id);
  button.onpointerdown = (event) => { event.preventDefault(); startJog(dir); };
  button.onpointerup = endJog;
  button.onpointerleave = endJog;
  button.onpointercancel = endJog;
}

document.addEventListener('keydown', (event) => {
  if (event.repeat || event.target.tagName === 'INPUT') return;
  if (event.key === 'ArrowRight') startJog('cw');
  if (event.key === 'ArrowLeft') startJog('ccw');
  if (event.key === 'Escape') { endJog(); post('/api/stop'); }
});
document.addEventListener('keyup', (event) => {
  if (event.key === 'ArrowRight' || event.key === 'ArrowLeft') endJog();
});
// Losing focus while a key is down would otherwise leave the jog latched.
window.addEventListener('blur', endJog);

$('settingsBtn').onclick = () => { $('settings').hidden = !$('settings').hidden; };

$('favAdd').onclick = () => { favorites = collectFavorites(); favorites.push({ name: 'Nowy', az: 0 }); renderFavorites(); };
$('favSave').onclick = async () => {
  favorites = collectFavorites();
  await fetch('/api/favorites', { method: 'POST', body: JSON.stringify(favorites) });
  await loadFavorites();
};

$('syncBtn').onclick = () => post('/api/sync', { raw: $('syncRaw').value });

$('cfgSave').onclick = async () => {
  const params = {
    hostname: $('cfgHost').value,
    wifiSsid: $('cfgSsid').value,
    rotctldPort: $('cfgRotctld').value,
    rawPort: $('cfgRaw').value,
    serialBaud: $('cfgBaud').value,
  };
  if ($('cfgPass').value) params.wifiPassword = $('cfgPass').value;
  if ($('cfgPassword').value) params.password = $('cfgPassword').value;

  const res = await post('/api/config', params);
  $('cfgErr').hidden = res.ok;
  $('cfgOk').hidden = !res.ok;
  if (res.ok) $('cfgOk').textContent = 'Zapisano — zmiany po restarcie';
  else $('cfgErr').textContent = res.data.error || 'Błąd zapisu';
};

$('fwBtn').onclick = async () => {
  const file = $('fwFile').files[0];
  const status = $('fwStatus');
  if (!file) { status.hidden = false; status.textContent = 'Wybierz plik'; return; }

  status.hidden = false;
  status.textContent = 'Wgrywanie…';

  const body = new FormData();
  body.append('target', $('fwTarget').value);
  body.append('file', file);

  try {
    const res = await fetch('/api/update', { method: 'POST', body });
    const data = await res.json().catch(() => ({}));
    status.textContent = res.ok ? 'Wgrano — restart' : ('Błąd: ' + (data.error || res.status));
  } catch (e) {
    // The bridge reboots on success, so a dropped connection here is expected
    // rather than a failure - saying "error" would be misleading.
    status.textContent = 'Restart w toku…';
  }
};

$('restartBtn').onclick = () => post('/api/restart');
$('logoutBtn').onclick = async () => { await post('/api/logout'); location.reload(); };

init();
