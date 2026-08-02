# Design

WiFi bridge between a [K3NG GS-232B rotator controller](https://github.com/sq9fk/k3ng_controler_nano_light)
(Arduino Nano, azimuth only, 405° rotator) and the network. Runs on a LOLIN S3 Mini (ESP32-S3).

Three network faces, one rotator:

- **rotctld** — Hamlib net rotator protocol on TCP 4533, for logging and contest software.
- **raw** — a transparent GS-232 socket, so an existing program that expects a serial rotator can talk to it as if
  over a cable.
- **web panel** — password-protected, single session, served from the ESP.

## Execution model

Single-threaded cooperative loop, no RTOS. The WiFi stack has to be given the CPU regularly, and the longest
operation in the system — a command to the controller and its reply — takes ~15 ms. Everything that waits on the
serial line is a state machine; nothing blocks.

## One queue for everything

`RotatorLink` is the sole owner of the UART. Every command from every source passes through its queue.

This is not tidiness, it is a correctness requirement. GS-232 has no transaction ids. If a raw client sends `C` at
the same moment as the position poller, two identical `AZ=xxx` replies come back with nothing to distinguish them,
and each consumer has a 50 % chance of reading the other's answer. A byte-transparent passthrough cannot avoid this;
line-level framing can. So the raw socket is transparent *at the line level*: it reads whole GS-232 commands, feeds
them through the same queue, and routes replies back to the client that asked. From the client's side it still looks
like a cable.

Consequences that follow from the single queue:

- **The azimuth is always visible.** The poller keeps running regardless of who is in control, so the web panel
  shows the live position even while rotctld or a raw client is driving the rotator.
- Stop commands (`S`, `A`) jump the queue. A stop queued behind a position poll is a stop that arrives late.
- The cache carries a timestamp. Past `kPositionStaleMs` the position is reported as stale rather than presented as
  live — a frozen number that looks current is worse than an explicit "unknown".

**The last motion command wins, regardless of source.** A not-yet-dispatched goto/jog (`M`/`L`/`R`) sitting in the
queue is replaced in place — not appended behind — by a newer one from *any* source (`RotatorLink::submit()`).
Without this, two clients issuing conflicting gotos close together (well within the ~80 ms `kErrorGraceMs` window
that a motion command occupies before the queue can advance) would both reach the controller: the rotator would
visibly start turning toward the first target for a moment before the second command corrected it. A queued stop
goes further and purges *every* not-yet-sent motion command outright (`purgeQueuedMotionCommands()`) — otherwise a
stop would only be momentary, with the goto that was queued behind it starting the rotator moving again right after.
An already-dispatched command (on the wire, in `active_`) is never touched — it can't be recalled, and the
controller will accept whatever comes next as its new target regardless.

Superseding a queued raw command still has to resolve its caller, not just discard it: `RawServer` allows only one
outstanding command per client at a time (`RawServer.cpp`) and would otherwise wait forever for a reply that will
now never arrive.
`resolveSuperseded()` invokes the reply handler with `Result::NoReply` for the entry being replaced — the same
outcome that command would have produced by actually succeeding, since `M`/`L`/`R` answer nothing either way. So a
superseded raw client sees exactly the silence it would have seen had the controller genuinely accepted its command.

## Talking to this particular controller

Behaviour of the firmware on the other end that the bridge has to accommodate:

| Fact | Consequence |
|---|---|
| `C` reports a **real** azimuth (0–359); `M000`–`M360` takes a real one and the controller itself picks the raw target through the overlap; `M361`–`M585` is explicit raw | The bridge commands a **real** azimuth (`M%03d`, 0–359) like a standard GS-232B logger and lets the controller choose the raw turn — it has the live position, which the polled cache here does not. `gs232::chooseRawTarget()` is kept in the library (and unit tested) only for a future "force the far side" feature that would send explicit raw. |
| The fork's `I` command reports the **raw** azimuth, and `Ixxx` sets it | The poller uses `I`, not `C`. A real azimuth cannot say which turn the rotator is on, so the panel needs raw for the overlap arc, `OL` badge and the raw sub-line. `Ixxx` is the panel's position-sync calibration. Commanding a goto does **not** use raw — that goes out as a real azimuth. |
| `M`, `L`, `R`, `A`, `S` answer **nothing** on success, `?>` on rejection | The transaction layer cannot simply wait for a reply. `gs232::classify()` splits commands into always-answers / answers-only-on-error / never-answers, which drives the timeout. |
| Rotation commands are **silently dropped for 5 s after the controller boots** | The bridge refuses motion commands during the lockout instead of sending them into the void, and tells the caller. |
| The controller can stall its own loop ~13 ms writing EEPROM | Reply timeout is 300 ms. A tight timeout produces phantom retries, and a retried motion command is a second rotation. |
| The controller (2026-08-01+) has its own AVR hardware watchdog (`OPTION_WATCHDOG`, 2 s) and resets itself if its `loop()` ever hangs | No bridge-side change needed: a watchdog reset is a real MCU reset, so it produces exactly the "goes silent, then replies again" pattern the bridge already treats as a probable power-cycle and uses to re-arm its own boot-lockout tracking. The two designs compose without either side knowing about the other. |
| The controller (2026-08-01+) seeds a fresh/incompatible EEPROM's azimuth to `azimuth_starting_point` (180) instead of leaving it at a raw `0` | Older controller firmware could report a bogus `AZ=000`/`RAW=0` for the first few seconds after a first flash or an EEPROM-layout version bump, before the first real pulse arrived. Not a bridge bug, but worth knowing if an old log shows a brief 0° reading right after a controller update. |
| `X1`–`X4` answer `Speed Xn` but do nothing (no PWM wired) | Do not expose speed control in the UI. |
| The controller emits `AZ Rotation Stall Detected` unsolicited | The reader must handle a line arriving with no transaction outstanding. |

## Wiring

Bridge connects to the **Nano's TX/RX pins**, not its USB port — so opening the link does not reset the controller,
and the 5 s lockout only ever applies after a real power-up.

- Controller TX → ESP RX (GPIO18) **through a divider** (1 kΩ + 2 kΩ → 3.33 V). 5 V on an ESP pin destroys it.
- ESP TX (GPIO17) → controller RX. 3.3 V clears the AVR's 3.0 V threshold, but with little margin; a 74AHCT125
  buffer solves both directions if it proves flaky.
- The controller link is a hardware UART (`Serial1`) placed on those pins by the GPIO matrix. UART0 stays on USB for
  the console, so its boot-time output never reaches the controller as garbage commands.

No dev board in this size class integrates a 3.3/5 V level shifter — it is not a standard feature outside
industrial hardware — and none is needed here, since only one of the two directions requires translation.

## Why ESP32-S3 rather than ESP8266

The project started on a D1 mini and moved once the web panel scope became clear. Three reasons:

- **RAM.** Phase 1 alone used 35.5 % of the ESP8266's heap before WiFi, WebSocket, two TCP servers and a compiled
  frontend bundle. The same code uses 5.9 % on the S3. The ESP8266 plan required fixed buffers and strict client
  limits purely to survive heap fragmentation; that pressure is gone.
- **Hardware UART on arbitrary pins.** The GPIO matrix removes `SoftwareSerial` from the critical path.
- Same footprint as the D1 mini, so the wiring plan carried over unchanged.

## Access control

- **Multiple accounts, each with its own password and its own session.** Not a single global account, and not a
  fixed pool of session slots shared by whoever logs in first: every configured account (`Config::User`, seeded with
  two — one per operator of this station) gets exactly one session slot of its own, the same way the original
  single-account design worked. Two different accounts can be logged in at once, each from their own device, simply
  because each has its own slot to hold - no separate "N concurrent sessions" limit was needed.
- Password stored as **salted SHA-256 over 10000 iterations, per account** — not PBKDF2, and named accurately rather
  than dressed up. It means a LittleFS dump does not yield a reusable password.
- **One session per account.** A second login to the *same* account is refused with the address of the holder, plus
  an explicit takeover that invalidates it — otherwise a closed browser tab locks that account until reboot. Idle
  sessions expire after 15 minutes, independently per account; five wrong guesses buy a minute of refusal, and that
  throttle is **global across every account, not per account** — a per-account throttle would let an attacker
  guess two accounts' passwords in parallel without ever tripping either one's limit.
- **Account management is flat.** Creating, resetting, or deleting an account (`/api/users`) is available to any
  authenticated session - the panel has no separate admin role, so this follows the same model as the rest of
  Settings. Deleting the last remaining account is refused outright: the panel must always have somewhere to log
  in from.
- The WebSocket authenticates from the handshake cookie at connect time (see CLAUDE.md) - the same cookie that
  identifies which account's session this is, so no separate token exchange is needed over the socket itself.
- **TLS is terminated at a reverse proxy, not on the device** — see [docs/tls.md](docs/tls.md). The firmware marks
  the cookie `Secure` behind `X-Forwarded-Proto: https` and the WebSocket authenticates from the handshake cookie,
  so a TLS proxy needs no firmware change. On-device TLS is declined mainly because a handshake would block the
  cooperative loop and delay the jog dead-man stop — trading a real safety margin for encryption the LAN usually
  does not need. Over plain HTTP the password crosses the LAN in clear.

## WiFi network selection

- **A priority list, not a single network** (`Config::wifiNetworks[kMaxWifiNetworks]`, 5 slots). `Net.cpp` tries
  index 0 first on every (re)connect, walking down the list on a timeout and only falling back to the bridge's own
  AP once every configured slot has had its turn. Priority is **list order, not signal strength** — the operator's
  own judgement about which network should win (e.g. the shack's own AP over a phone hotspot that happens to be
  louder) isn't something a scan result can infer. A dropped station connection restarts the search from index 0
  rather than resuming wherever it left off, so a higher-priority network coming back into range reclaims it.
- **`GET /api/wifi/scan` is async** (`WiFi.scanNetworks(true)`, polled by `scanComplete()`) - a blocking scan takes
  seconds, and this bridge never blocks `loop()` for anything an operator can trigger from the panel, the same
  reasoning as `AntennaSwitch`'s `AsyncClient`. The panel polls this endpoint roughly once a second while a scan is
  in flight and stops once it reports `"done"`.
- Saved networks are managed like accounts (`/api/wifi/networks`, `+/delete`) — add by name-or-update, delete by
  name, both immediate rather than bundled into the one shared config save. Passwords are **write-only**: neither
  `GET /api/wifi/networks` nor `GET /api/config` ever echoes one back, same reasoning as account passwords - the
  client only needs to know which SSIDs are already saved, and the settings field always starts blank so leaving it
  untouched cannot be misread as "set the password to empty" (see below).
- **The fallback AP's own password (`config.apPassword`) is now configurable** — it used to be a hardcoded constant
  in `Net.cpp` with no way to change it short of reflashing. It is write-only for the same reason: `handleSetConfig`
  only touches it when the field is actually present in the request, so an empty submission is a deliberate choice
  (an open fallback AP) rather than "leave unchanged" being confused with "clear it". The AP's address is fixed at
  `10.10.10.1/24` with an explicit `softAPConfig()` (not the ESP32 default `192.168.4.1`), chosen so it never
  collides with whatever subnet the operator's own station-mode router happens to use.

## Showing who is in control

`/api/status` reports every connected rotctld and raw client with its address, plus a single `remoteConnected` flag
so the panel does not have to work out for itself what counts as "someone else". `Rotator` records the source of the
last motion command, including raw clients — otherwise the panel would attribute a movement to whoever last used the
API, which is the wrong answer to "why is it turning".

The panel shows a **persistent banner**, not an icon, whenever anything other than the local session is connected —
outranked only by a dead serial link, since without the link nothing else on the page means anything.

**"Last motion" names an operator, not just "web".** `Rotator`/`RotatorLink` still only know the four-way
`Source` (poller/web/rotctld/raw) - that stays as-is, it is the safety-relevant queue-attribution mechanism and
does not need an account name to do its job. Which *account* issued a web-sourced motion is tracked separately, in
`WebApi.cpp` only (`lastWebActor`, plus a small WebSocket-client-id → account map for jog commands, which arrive
with no cookie of their own): set right after a `POST /api/goto` or a jog succeeds, read back into
`lastMotion.user` only when the source is `web`. This stays stale if the temporary USB console
(`main.cpp`'s `serviceConsole()`, which also submits as `Source::Web`) causes a motion instead - an accepted gap,
since that console is a dev aid rather than an operator-facing control path.

**An absolute date, not just an age.** The ESP32 has no real-time clock of its own - `Net.cpp` calls `configTime()`
once the bridge reaches the real internet (station mode only; the AP-only fallback has no route out) and again
every 6 h after that, both explicitly, not relying on SNTP's own undocumented default resync interval. `time(nullptr)`
below a 2024-01-01 sentinel means "not synced yet" (`net::timeSynced()`); `lastMotion.epochS` is only sent once that
is true, so the panel falls back to the relative "N s temu" it always had rather than showing a bogus 1970 date
during the window right after boot before the first sync completes.

## Antenna switch (ant-sw-2x6)

A separate device on the LAN — [ant-sw-2x6](https://github.com/sq9fk/ant-sw-2x6), a 6-antenna x 2-TRX switch — gets
a control surface in this panel too (`AntennaSwitch.h`/`.cpp`), so the operator does not have to switch between two
web pages while turning the antenna. It is deliberately **plain HTTP, not OTRSP**: OTRSP stays reserved for the
logging program (N1MM+ etc.), and the bridge instead speaks the same `GET /?S{bank}{ant:02d}` requests that device's
own web panel uses. Reading back which antenna is selected uses a small machine-readable endpoint added to that
device, `GET /?J` → `A=<trx1>,<trx2>` — parsing its full HTML page instead would couple this firmware to the other
project's page layout, breaking silently whenever that HTML changes.

**Non-blocking, like everything else here.** A plain `WiFiClient::connect()` can block for seconds against a
misconfigured or dead host, which would stall `loop()` and delay the jog dead-man timer — a real safety margin, not
a nicety. So this client uses `AsyncClient` from `AsyncTCP` (already a transitive dependency of
`ESPAsyncWebServer`, listed explicitly in `platformio.ini`) instead of the synchronous `WiFiClient`, fully
event-driven the same way the HTTP/WebSocket server already is.

Off by default (`config.antEnabled`, `config.antHost`) — nothing probes the network for it until the operator turns
it on in Settings and points it at a host. Its status is folded into the same `/api/status` / WebSocket stream as
the rotator (`doc["antenna"]`) rather than a second poll loop in the panel.

**Status freshness, not just success/failure.** `linkConnected` (last exchange's own result) flips on every request,
but a single slow round trip would otherwise flicker the panel's dot between green and red. `lastOkAt` plus
`fresh()` (connected **and** that success was within `kFreshMs` = 5 s, ~2.5x the poll interval) give a third,
amber "stale" state instead — the same idea as the controller link's own `linkHealthy`/position-freshness pair,
reused here rather than inventing a second convention for the second link this bridge maintains.

**The switch's own name travels for free.** `/?K` (antenna names) already exists for this bridge; its site name
rides along as a 7th comma-separated field rather than a new endpoint, at +16 B against ant-sw-2x6's ~90 B flash
margin (that project is separately documented, see its own CLAUDE.md/DESIGN.md) instead of the +68 B a second
endpoint reusing `HTTP_HEAD` would have cost. `AntennaSwitch::parseNames()` tolerates older firmware missing the
field by simply leaving the device name at its default rather than requiring it.

**The antenna this rotor turns, and collisions, are panel-only facts — nothing new asked of ant-sw-2x6 for either.**
`config.rotorAnt` just labels one antenna number for this operator; the switch has no notion of "the antenna on this
rotor" and is never told. Collision (the same real antenna picked for both TRX, which the switch itself refuses and
flags on its own panel) is likewise derived purely from the two numbers `/?J` already reports — recomputing the
same trigger condition client-side, rather than teaching this bridge the switch's own multi-TRX collision algorithm
or asking that flash-constrained project for a new field. See docs/ui-spec.md for the panel-side reasoning on both.

## Monitor tab (protocol traffic)

A live view of what is actually being said on each of the bridge's four protocol surfaces — rotctld, raw GS-232,
the antenna switch, and the serial link to the controller itself — shown TX/RX-colored and session-tagged in a
new **Monitor** tab. Built for diagnosing a misbehaving client (a logger sending malformed commands, a controller
reply that doesn't parse) without a separate packet capture.

**Opt-in per stream, not just a display filter (`DebugLog.h`/`.cpp`).** `config.debugEnabled` shows the tab at all;
five checkboxes below it (`debugRotctld`/`debugRaw`/`debugAntenna`/`debugController`) each gate *capture*, not just
rendering — the position poller alone talks to the controller roughly every 300 ms, which would flood a shared ring
buffer whether or not the Monitor tab is even open if capture were not itself opt-in. A small fixed ring buffer (24
entries) holds whatever is captured; overflowing it sets a flag the panel shows as "some lines dropped" rather than
growing unboundedly or blocking a caller that logs faster than the panel drains.

**Drained only by the periodic WebSocket broadcast, never by an ad-hoc status build.** `buildStatus()` is called
from two different kinds of places: the 250 ms broadcast loop in `webapi::poll()`, and individual action handlers
(`handleGoto`/`handleStop`/`handleSync`/`handleAntenna`) that return fresh status directly in their own HTTP
response. Since draining the ring buffer clears it, if `buildStatus()` itself drained the log, an entry logged by
a goto action (e.g. the `M210` sent to the controller) would be consumed into *that specific* HTTP response — which
the panel's JS never reads, since goto/stop/jog only check `res.ok` — instead of surviving to the next broadcast,
which is the one thing `render()` actually turns into Monitor-tab rows. So the drain happens exactly once, in
`poll()`, right after `buildStatus()` returns and only when there is at least one WebSocket client to send it to;
`buildStatus()` itself never touches the debug log. The simulator (`sim/sim_server.py`) mirrors this exactly:
`build_status()` never drains, only `ws_broadcast_loop()` does.

## Memory budget

320 KB RAM, 4 MB flash. Client limits are configurable (`rotctldMaxClients`, `rawMaxClients`) but clamped to
compile-time ceilings — `RotctldServer::kClientCeiling` and `RawServer::kClientCeiling` — which size the session
arrays for the ESP32's ~10-slot BSD socket pool (shared by the two `WiFiServer`s; AsyncTCP for HTTP/WS uses a
separate pool). The config value clamps rather than rejects. Any raw client count up to the ceiling is safe:
replies are routed per transaction id, so there is no packet collision, only shared control. Fixed buffers in the
serial path remain because they also make the transaction layer easier to reason about.

**Both ceilings scale with `Config::kMaxUsers` (3)** rather than being fixed numbers, on the reasoning that each
panel account is a different operator who might run their own logging software: `RotctldServer::kClientCeiling`
= 2 × kMaxUsers = **6**, `RawServer::kClientCeiling` = 1 × kMaxUsers = **3**. Total **9 of the ~10-slot pool** —
tight, and worth remembering before raising `kMaxUsers` again: each extra account costs 3 more sockets, not one.

Baseline, phase 1: 19180 B RAM (5.9 %) / 270993 B flash (20.7 %).
Phase 2, with the WiFi stack and HTTP server: 45824 B RAM (14.0 %) / 823501 B flash (62.8 %).
Phase 6 ("Complete", see below): 48212 B RAM (14.7 %) / 901017 B flash (68.7 %).
Current, with multi-account auth, the Monitor tab and the antenna switch's device name/collision/rotor-marker
additions on top of phase 6: 52504 B RAM (16.0 %) / 955641 B flash (72.9 %).

The flash figure is worth watching: it is a fraction of one OTA app partition, and the default 4 MB layout keeps two
of them. There is room for the panel, but not unlimited room, which is one more argument for a hand-written
frontend over a framework bundle.

## Phases

1. **Serial layer** — `Gs232` (pure, unit tested) + `RotatorLink` + position cache, driven from the USB console. ✔
2. **WiFi, config in LittleFS, AP fallback, REST.** ✔ ← phase 2 baseline: 45824 B RAM (14.0 %), 823501 B flash (62.8 %)
3. **`RotctldServer`, port configurable.** ✔ Parser unit tested; the protocol details were taken from Hamlib's own
   client (`rigs/dummy/netrotctl.c`), not guessed. **Not yet verified against a real `rotctl`** — that needs the
   hardware. Phase 3 baseline: 47092 B RAM (14.4 %), 847313 B flash (64.6 %).
4. **Raw passthrough socket + connected-source reporting.** ✔ `/api/status` reports every connected rotctld and raw
   client with its address, plus a single `remoteConnected` flag for the panel's banner. Phase 4 baseline: 47108 B
   RAM (14.4 %), 849945 B flash (64.8 %).
5. **Web panel over WebSocket** — see [docs/ui-spec.md](docs/ui-spec.md). ✔ Phase 5 baseline: 47556 B RAM (14.5 %),
   884365 B flash (67.5 %). Rendered and checked at desktop and mobile widths against mocked state; everything
   behind it still needs the hardware.
6. **OTA, hardening, serial-link watchdog.** ✔ Final baseline: 48188 B RAM (14.7 %), 899877 B flash (68.7 %).

## Task watchdog

Separate from the serial-link watchdog below, and easy to confuse with it: this one watches the `loop()` task
itself, not the UART. If `loop()` ever hangs - a bug in any of the servers or clients it drives, not just the
controller link - the bridge should recover on its own rather than needing a power cycle at the mast, the same
reasoning as the always-on AVR watchdog in the sibling [ant-sw-2x6](https://github.com/sq9fk/ant-sw-2x6) firmware.
`main.cpp` calls `esp_task_wdt_init(8, true)` (panic on timeout, which reboots) and `enableLoopWDT()` at the very
end of `setup()`, once every other `begin()` has run. Nothing elsewhere has to call `esp_task_wdt_reset()`: the
Arduino core's own loop task already feeds it once per iteration. Eight seconds is generous next to every actual
operation in `loop()` - everything in it is non-blocking except a LittleFS config/favourites write, which takes
milliseconds - so it only fires on a genuine hang.

Manual restart already existed before this (the panel's Settings → System → Restart button, `POST /api/restart`);
the task watchdog is what catches the case nobody is there to click it.

## Serial-link watchdog

Five consecutive timeouts — roughly three seconds of silence — mark the link unhealthy, which the panel shows as a
banner outranking everything else on the page. A single dropped reply is noise and does not count.

Recovery is where this earns its keep: when the controller answers again after being silent, it was almost certainly
power-cycled, and a freshly booted controller **silently discards rotation commands for five seconds**. So the
bridge re-arms the boot lockout on recovery rather than firing commands into a gap where they would vanish without
an error.

## Hardening

- **Login throttling.** Five wrong guesses buy a minute of refusal. Without it, guessing is limited only by how fast
  the ESP can hash — which is the wrong thing to be the limit.
- **Updates through the panel**, not ArduinoOTA: one authenticated surface instead of two, and no second password to
  manage. The rotator is stopped first, because the reboot would otherwise leave it turning unattended.
- Static assets carry a cache header; the config and favourites files are written through a temporary file so a
  power cut cannot leave an unparseable one.

**Full-codebase bug review (2026-08-02).** Nine fixes, from an unauthenticated info leak down to defensive-only
edge cases:

- **`auth::login()` now returns a `LoginResult` enum (`Ok`/`InvalidCredentials`/`SessionHeld`), not an ambiguous
  empty `String`.** The old signature could not tell its own caller *why* it failed, so `handleLogin()` guessed by
  checking `auth::session(user)->active` **independently of whether the submitted password was even correct** -
  meaning an unauthenticated request naming any configured account, with any password at all, got back "session
  held" plus the holder's IP address whenever that account happened to be logged in. `SessionHeld` is now only
  reachable in `login()` itself, strictly after `checkPassword()` has already succeeded.
- **`Rotator::gotoAzimuth()` and `gs232::chooseRawTarget()` reject non-finite azimuths.** `az < 0 || az >= 360`
  (the check every caller used) is `false` for NaN on both sides - every comparison against NaN is - so it never
  actually rejected one, and `lroundf(NaN)` afterwards is implementation-defined. Confirmed live against the
  simulator's Python mirror (`az=nan` raised an uncaught `ValueError` deep inside `do_goto()`, proving the
  equivalent check there had the same gap) before fixing both. Reachable via `/api/goto`, rotctld's `P` (which had
  *no* pre-check at all), and the USB console - the guard belongs in `gotoAzimuth()`/`chooseRawTarget()` themselves,
  not scattered across every caller.
- **The session cookie is a session cookie now** (`WebApi.cpp`'s `sessionCookie()`), with no `Max-Age` - previously
  a flat `Max-Age=3600` never got renewed, while the server's own idle timeout (`Auth.cpp`'s `kIdleTimeoutMs`, 15
  min) slides on every request. A continuously-active session - a multi-hour contest run, exactly what "sit
  through a slow QSO" describes - would hit that fixed mark and force a re-login despite the server still
  considering it valid.
- **`handleSetConfig()` validates `rotctldPort`/`rawPort` into locals before touching `config`.** `readPort()`
  writes its target as soon as a value parses; checking the "must differ" rule against the live fields afterwards
  meant a rejected request could still leave a bad value sitting in `config`, later persisted by some unrelated
  save (the settings form always submits the whole struct together).
- **`AntennaSwitch` queues one command per TRX bank, not one shared slot.** Setting both banks in quick succession
  (reconfiguring TRX1 and TRX2 at once) let the second `setAntenna()` call silently overwrite the first in
  `queuedPath` before `poll()` ever sent it - both calls returned `true`, but only the last command ever reached
  the switch.
- **`cookieToken()` walks "name=value" pairs instead of `indexOf("sid=")` over the whole header.** The old scan
  would match "sid=" inside any other cookie whose name happened to end in it (e.g. a stray "xsid=..."), silently
  returning the wrong value as the session token.
- **`Rotator::stop()` only clears `hasTarget_` once the stop actually reaches the queue.** Clearing it
  unconditionally hid a real, still-current target on the one failure path (queue full) where the stop itself
  never went anywhere.
- **`RotatorLink::kMaxCommandLen` raised from 16 to 32, matching `RawServer::kLineLen`.** A raw client's line is
  forwarded verbatim into this buffer; the mismatch meant a longer-than-15-character line got silently truncated
  before it ever reached `classify()` or the wire, with no error back to that client.
- **`Favorites::at()` bounds-checks its index** rather than trusting every future caller to stay within `count()`.

The Python simulator (`sim/sim_server.py`) already got the login ordering right on its own, but needed the same
NaN guard (`choose_raw_target()`, the `/api/goto` check) and the same port-conflict validation (it had none at
all before) to keep mirroring the fixed firmware.

## Testing

`lib/gs232` has no Arduino dependency so it runs under `pio test -e native` on the host. That is where the coordinate
mapping and command classification are tested — the parts where a mistake means the antenna goes the wrong way.
Everything above it needs the real controller.
