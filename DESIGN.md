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

## Talking to this particular controller

Behaviour of the firmware on the other end that the bridge has to accommodate:

| Fact | Consequence |
|---|---|
| `C` reports a **real** azimuth (0–359); `M###` takes a **raw** one (180–585) | Read and write are in different coordinate systems. `gs232::chooseRawTarget()` maps between them and is the only place that knows this. |
| The fork's `I` command reports the **raw** azimuth, and `Ixxx` sets it | The poller uses `I`, not `C`. A real azimuth cannot say which turn the rotator is on, so deriving raw from it is guesswork that is wrong half the time in the overlap zone. `Ixxx` is what the panel's position-sync calibration will use. |
| A real azimuth in the overlap zone has two raw representations (10° = raw 10 or raw 370) | The target must be chosen relative to the current position, otherwise the rotator occasionally travels 350° the wrong way. Policy: shortest travel. |
| `M`, `L`, `R`, `A`, `S` answer **nothing** on success, `?>` on rejection | The transaction layer cannot simply wait for a reply. `gs232::classify()` splits commands into always-answers / answers-only-on-error / never-answers, which drives the timeout. |
| Rotation commands are **silently dropped for 5 s after the controller boots** | The bridge refuses motion commands during the lockout instead of sending them into the void, and tells the caller. |
| The controller can stall its own loop ~13 ms writing EEPROM | Reply timeout is 300 ms. A tight timeout produces phantom retries, and a retried motion command is a second rotation. |
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

- Password stored as **salted SHA-256 over 10000 iterations** — not PBKDF2, and named accurately rather than
  dressed up. It means a LittleFS dump does not yield a reusable password.
- **One session at a time.** A second login is refused with the address of the holder, plus an explicit takeover
  that invalidates it — otherwise a closed browser tab locks the panel until reboot. Idle sessions expire after
  15 minutes; five wrong guesses buy a minute of refusal, since otherwise the guessing rate is limited only by how
  fast the ESP can hash.
- The WebSocket carries its own authentication: the handshake headers are not available in the event callback, so
  the panel presents its token as the first message and the connection is closed if it does not check out.
- **TLS is terminated at a reverse proxy, not on the device** — see [docs/tls.md](docs/tls.md). The firmware marks
  the cookie `Secure` behind `X-Forwarded-Proto: https` and the WebSocket authenticates from the handshake cookie,
  so a TLS proxy needs no firmware change. On-device TLS is declined mainly because a handshake would block the
  cooperative loop and delay the jog dead-man stop — trading a real safety margin for encryption the LAN usually
  does not need. Over plain HTTP the password crosses the LAN in clear.

## Showing who is in control

`/api/status` reports every connected rotctld and raw client with its address, plus a single `remoteConnected` flag
so the panel does not have to work out for itself what counts as "someone else". `Rotator` records the source of the
last motion command, including raw clients — otherwise the panel would attribute a movement to whoever last used the
API, which is the wrong answer to "why is it turning".

The panel shows a **persistent banner**, not an icon, whenever anything other than the local session is connected —
outranked only by a dead serial link, since without the link nothing else on the page means anything.

## Memory budget

320 KB RAM, 4 MB flash. Client limits stay configurable (2 × rotctld, 1 × raw, 1 × WebSocket) but are now a policy
choice rather than a survival measure. Fixed buffers in the serial path remain, because they also make the
transaction layer easier to reason about.

Baseline, phase 1: 19180 B RAM (5.9 %) / 270993 B flash (20.7 %).
Phase 2, with the WiFi stack and HTTP server: 45824 B RAM (14.0 %) / 823501 B flash (62.8 %).
Complete: 48212 B RAM (14.7 %) / 901017 B flash (68.7 %).

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

## Testing

`lib/gs232` has no Arduino dependency so it runs under `pio test -e native` on the host. That is where the coordinate
mapping and command classification are tested — the parts where a mistake means the antenna goes the wrong way.
Everything above it needs the real controller.
