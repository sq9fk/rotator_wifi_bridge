'use strict';

const $ = (id) => document.getElementById(id);
let socket = null;
let state = null;
let jogHeld = null;
let jogTimer = null;
let favorites = [];
let favDirty = false;

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

async function post(path, params) {
  const res = await fetch(path, { method: 'POST', body: new URLSearchParams(params || {}) });
  return { ok: res.ok, status: res.status, data: await res.json().catch(() => ({})) };
}
const getJson = async (path) => { const r = await fetch(path); return r.ok ? r.json() : null; };

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

  $('linkDot').className = 'dot' + (s.controller.linkHealthy && p.fresh ? '' : ' bad');

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
  $('sessRow').textContent = 'ta przeglądarka';

  $('motionRow').textContent = s.lastMotion
    ? `${s.lastMotion.source}, ${Math.round(s.lastMotion.ageMs / 1000)} s temu` : '—';

  $('ccwBtn').classList.toggle('active', s.jogging && jogHeld === 'ccw');
  $('cwBtn').classList.toggle('active', s.jogging && jogHeld === 'cw');

  $('sysNet').textContent = `${s.network.mode} · ${s.network.ssid} · ${s.network.rssi} dBm`;
  $('sysAddr').textContent = s.network.address;
  $('sysRange').textContent = `${s.controller.rawMin}..${s.controller.rawMax}°`;
  $('sysHeap').textContent = Math.round(s.heapFree / 1024) + ' kB';
  $('sysUptime').textContent = Math.floor(s.uptimeMs / 60000) + ' min';
}

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
  $('cfgSsid').value = cfg.wifiSsid || '';
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
  const session = await getJson('/api/session');
  if (session && session.setupRequired) {
    $('loginSub').textContent = 'Pierwsze uruchomienie — ustaw hasło (min. 8 znaków)';
    $('loginBtn').textContent = 'Ustaw hasło';
    $('loginForm').dataset.mode = 'setup';
  }
  if (session && session.authenticated) await enterApp();
}

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
  if (res.ok) { await enterApp(); return; }

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
// that tile rather than dropping the operator on a closed list.
$('syncOpen').onclick = () => {
  document.querySelector('.tab[data-tab=settings]').click();
  openTile(document.querySelector('.collapse'));
  if (state) $('syncRaw').value = Math.round(state.position.raw);
};

$('favAdd').onclick = () => {
  if (favorites.length >= 10) return;
  favorites.push({ name: 'Nowy', az: state ? Math.round(state.position.azimuth) : 0 });
  favDirty = true;
  renderFavorites();
};
$('favSave').onclick = async () => {
  await fetch('/api/favorites', { method: 'POST', body: JSON.stringify(favorites) });
  await loadFavorites();
};

$('syncBtn').onclick = () => post('/api/sync', { raw: $('syncRaw').value });

$('cfgSave').onclick = async () => {
  const params = {
    hostname: $('cfgHost').value, wifiSsid: $('cfgSsid').value,
    rotctldPort: $('cfgRotctld').value, rawPort: $('cfgRaw').value,
    rotctldMaxClients: $('cfgRotctldMax').value, rawMaxClients: $('cfgRawMax').value,
    serialBaud: $('cfgBaud').value,
    overlapFrom: $('cfgOvFrom').value, overlapTo: $('cfgOvTo').value,
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

$('restartBtn').onclick = () => post('/api/restart');
$('logoutBtn').onclick = async () => { await post('/api/logout'); location.reload(); };

init();
