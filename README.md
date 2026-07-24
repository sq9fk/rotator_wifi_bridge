# rotator_wifi_bridge

WiFi bridge for a [K3NG GS-232B rotator controller](https://github.com/sq9fk/k3ng_controler_nano_light) — Arduino
Nano, azimuth only, 405° rotator — running on a LOLIN S3 Mini (ESP32-S3).

It gives the rotator three network faces at once, all sharing a single serialised link to the controller:

- **rotctld** — Hamlib net rotator protocol on TCP 4533, for logging and contest software.
- **raw** — a transparent GS-232 socket for programs that expect a serial rotator.
- **web panel** — password-protected, one session at a time, served from the ESP.

The current azimuth stays visible in the panel no matter which source is driving the rotator, and the panel shows
which source issued the last motion command.

The web panel's look and feature set follow [stefan-wr/esp-rotor-control](https://github.com/stefan-wr/esp-rotor-control)
as a reference — with thanks. No code is taken from it; see [docs/ui-spec.md](docs/ui-spec.md) for what carries over
and what does not.

**Status: feature complete, unverified on hardware.** All six planned phases are implemented and the firmware
builds; the two pure protocol libraries pass 22 host unit tests and the panel has been rendered and inspected, but
**nothing has yet run against a real controller**. See [DESIGN.md](DESIGN.md) for the architecture and the reasoning
behind it, [CLAUDE.md](CLAUDE.md) for the working notes, and "Before first use" below for the bench checklist.

Final size: 48212 B RAM (14.7 %), 901017 B flash (68.7 %).

## Try the panel without hardware

A Python simulator serves the real panel and fakes the firmware behind it —
the needle moves, the overlap arc lights up, jog and its dead-man stop work:

```bash
python sim/sim_server.py
```

Then open <http://localhost:8080>. See [sim/README.md](sim/README.md) for the
`/sim/*` controls that fake a connected logger or a dead serial link. It tests
the interface, not the C++ — that still needs the bench.

## Before first use

Nothing here has touched real hardware. In order:

1. **Wiring, with the controller unpowered.** Confirm the divider on the controller TX → GPIO18 line before applying
   power. 5 V on an ESP pin destroys it.
2. **Serial link.** Open the console at 115200 and send `?`. A healthy link reports an azimuth and `fresh=1`; the
   panel says "brak łączności ze sterownikiem" if the controller is not answering.
3. **Panel.** `pio run -t uploadfs`, then browse to the bridge, set a password, check the dial tracks the rotator.
4. **Jog dead-man.** Hold an arrow key, then pull the WiFi or close the laptop lid. The rotator must stop within
   about half a second. This is the one failure mode that can damage the rotator, so test it deliberately.
5. **rotctld.** `rotctl -m 2 -r <host>:4533`, then `p`, `P 90 0`, `S`.
6. **Raw socket.** `nc <host> 4532`, then `C`.
7. **All three at once.** rotctld polling, a raw client connected and the panel open — the point of the single queue
   is that none of them see another's replies.

## Updates over the network

The settings drawer takes a firmware or filesystem `.bin` and reboots into it, over the panel's own authenticated
connection — no ArduinoOTA, no second password, no toolchain needed at the mast. The rotator is stopped before the
update starts, since the reboot would otherwise leave a rotation running with nothing watching it.

## Web panel

Served from LittleFS — the filesystem image has to be uploaded once, separately from the firmware:

```bash
pio run -e lolin_s3_mini -t uploadfs
```

On first visit it asks for a password (minimum 8 characters); after that, username and password. **One session at a
time**: a second login is refused with the address of the holder and offers a deliberate takeover, since a browser
tab closed without logging out would otherwise hold the panel until the idle timeout (15 minutes) or a reboot.

Layout: a top bar with a link indicator and Sterowanie / Ustawienia tabs, the dial on the left with a
Position / Target / **UTC** bar under it, and manual rotation, session and favourites cards on the right. Settings
is a list of collapsible tiles rather than one long form.

**The hub reads the raw azimuth**, so a bearing past 360 says which turn the antenna is on — `C` alone cannot tell
you. A white needle shows the position, a dim one the target. A red arc appears on the ring **only while the rotator
is inside the band reachable two different ways** (raw 540–585, bearings 180–225), together with an `OL` badge. At
raw 200 and raw 560 the dial reads 200° either way; that arc is the only thing that distinguishes them.

The clock is **UTC and labelled as such** — a rotator is used against schedules, beacons and other stations' logs,
and browser local time would invite an off-by-an-hour that nothing else on screen would reveal.

A **persistent banner** — not an icon — appears whenever a rotctld or raw client is connected, with its address, and
outranked only by a dead serial link. The session card lists every connected source and which one issued the last
motion command, because the question when the antenna starts moving is not "is someone connected" but "why is it
turning".

Click the dial to rotate, or use the arrow keys. **Jog is held, not latched**: the panel repeats the command every
200 ms while the key or button is down and the bridge stops the rotator after 500 ms of silence. `L` and `R` rotate
until something stops them, so a dropped WebSocket during a held jog would otherwise drive the rotator into its
limit; a closed tab, a lost network, a locked laptop and a window losing focus all end the rotation.

Ten named favourites, stored in LittleFS and marked on the dial by number. Calibration covers position sync
(`Ixxx`); degrees-per-pulse is set with the controller's own `D` command.

**No speed control**, deliberately: the controller answers `X1`–`X4` but this board switches relays with no PWM
wired, so a slider would misrepresent the hardware.

### Password storage and TLS

Salted SHA-256, 10000 iterations — not PBKDF2, and named accurately rather than dressed up. It means a dump of
LittleFS does not hand over a reusable password. The session cookie is `HttpOnly; SameSite=Strict`.

Modern TLS is done by **terminating it at a reverse proxy** (a real Let's Encrypt cert, TLS 1.3), not on the ESP32 —
the firmware already cooperates with that: the cookie gains `Secure` behind `X-Forwarded-Proto: https` and the panel
switches to `wss` automatically. On-device TLS is deliberately not used, chiefly because a TLS handshake would block
the cooperative loop and delay the jog dead-man stop. See [docs/tls.md](docs/tls.md) for the reasoning and a ready
nginx / Synology config. **Over plain HTTP the password crosses the LAN in clear** — fine on a trusted network, not
if the bridge is exposed.

## Raw GS-232 socket

For software that expects a serial rotator. Port configurable (`rawPort`, default 4532), and the client count too
(`rawMaxClients`, default 1, hard-capped at 2):

```bash
nc rotator.local 4532
```

Type `C` and the controller's `AZ=123` comes back. Anything the controller accepts works, including the fork's `I`
and `D` commands.

Transparent at the **line** level, not the byte level, and one outstanding command per client. A byte pipe to the
UART would race the position poller: GS-232 carries no transaction ids, so two overlapping `C` commands yield two
indistinguishable `AZ=` replies and each reader has even odds of taking the other's. Reading whole commands and
pushing them through the shared queue keeps every reply attributable — and from the client's side it still behaves
like a cable.

It defaults to **one** client because raw emulates a single serial cable, but **two is safe**: each session has its
own buffer and pending-transaction id, so replies are routed per client with no packet collision — the only thing
two raw clients share is control of the rotator, the same as two rotctld clients. Raise `rawMaxClients` to 2 if you
want that.

A timeout is reported to the client as `?>` rather than as silence. Silence is what the controller sends after a
successful rotate, so a client cannot tell the two apart, and one of them means the link is unhealthy.

## Serial link

| Setting | Default | Note |
|---|---|---|
| `serialBaud` | 9600 | must match `CONTROL_PORT_BAUD_RATE` in the controller's `rotator_settings.h` |

**One UART means one baud rate.** It governs the rotctld path, the raw path and the position poller alike; there is
no per-service rate to set. Only standard rates from 1200 to 115200 are accepted — an arbitrary divisor would give a
link that looks configured but returns nothing but framing errors, and the way out would be a serial console the
operator may not have to hand.

## rotctld

Hamlib net rotator protocol — **verified against the hamlib 4.7.2 client** (its `netrotctl.c` sends exactly what
this server answers). Port configurable (`rotctldPort`, default 4533), and the number of simultaneous clients too
(`rotctldMaxClients`, default 2, hard-capped at 4 by the socket budget):

```bash
rotctl -m 2 -r rotator.local:4533
```

Supported: `p` / `\get_pos`, `P az el` / `\set_pos`, `S` / `\stop`, `M <dir> <speed>` (left and right only), `_` /
`\get_info`, `\dump_state`, `q`. Park and reset answer `RPRT -4` (`RIG_ENIMPL`). Elevation is reported as a
constant `0`. It is a from-scratch subset, not a port of hamlib's `rotctld`; if a client needs a command that is not
listed it gets `RPRT -4`, so tell me which program/version and I will add it.

Two behaviours worth knowing:

- **Positions are exchanged in real azimuth, 0–360**, which is what logging software thinks in. The bridge sends the
  real azimuth (`M###`) and the controller picks the raw turn; the overlap is not exposed as extra degrees.
- **`p` returns `RPRT -6` when the cached position is stale** rather than the last known heading. Reporting a
  heading the rotator has left behind is how an operator ends up trusting a stale number.

`\dump_state` answers protocol version 1 with an explicit `min_az`/`max_az`. Version 0 would leave the Hamlib client
using its built-in ±180° range.

## Connecting logging and rotator software

Three ways in, in order of preference:

1. **Native Hamlib / rotctld over the network.** If the program speaks the Hamlib net rotator model (Log4OM, CQRLOG,
   Gpredict, PstRotator, N1MM+ via hamlib, …), point it straight at `<host>:4533` — nothing else to install. This is
   the cleanest path and the one verified against the hamlib client.
2. **Native GS-232 over TCP.** Some programs (e.g. PstRotator's TCP modes) open a raw TCP GS-232 target directly;
   point them at `<host>:4532`.
3. **A program that only opens a physical COM port** needs a virtual serial port backed by a TCP client — below.

### Virtual COM port over TCP (Windows)

For COM-only software, bridge a virtual serial port to the raw socket with the open-source
**[com0com](https://sourceforge.net/projects/com0com/) + com2tcp** pair (GPL, same project):

1. com0com creates a linked pair `CNCA0 ↔ CNCB0`; rename the app-facing end to e.g. `COM5` in its setup tool.
2. The logging program opens `COM5`.
3. com2tcp joins the other end to the bridge as a TCP client:

```
com2tcp --baud 9600 \\.\CNCA0 <bridge-ip> 4532
```

It works because the raw socket behaves like a real GS-232 cable — line-framed, replies terminated `CR LF`, silence
on a successful rotate — and com2tcp is a byte-transparent pipe, so together they look to the program exactly like a
serial rotator. Points to watch:

- **Raw mode, not `--telnet`** — telnet option negotiation and `0xFF` (IAC) escaping would reach the controller as
  garbage commands.
- **Use the signed com0com build** (3.0.0.0) on 64-bit Windows, or driver-signature enforcement fights you.
- The **baud rate is irrelevant** — com0com is a virtual UART and clocks nothing; set whatever the program expects.
- **Two com2tcp bridges at once** work, but only after raising `rawMaxClients` to 2 (the hard cap) — the default 1
  refuses the second connection. Both then share control of the one rotator, and each still receives only its own
  replies (routed by transaction id); a third is refused.
- **com2tcp must stay running**, so put it in a startup task or wrap it as a service.

A free but closed-source alternative, with a GUI and a built-in auto-reconnecting Windows service, is **HW VSP3** by
HW group — worth it if com0com's driver signing is a nuisance.

## REST API

Everything except `/api/session`, `/api/setup` and `/api/login` requires the session cookie.

| Endpoint | Body | Notes |
|---|---|---|
| `GET /api/session` | — | whether setup is needed, whether you are authenticated, who holds the session |
| `POST /api/setup` | `user=`, `password=` | first run only; refused once a password exists |
| `POST /api/login` | `user=`, `password=`, `force=1` | `409` with the holder's address if a session is active |
| `POST /api/logout` | — | |
| `GET /api/favorites`, `POST /api/favorites` | JSON array | up to 10, replaced as a set |
| `GET /api/status` | — | position with freshness, overlap flag, boot lockout, last motion source, connected clients, network, heap |
| `POST /api/goto` | `az=123` | 0–359; the raw target is chosen for shortest travel |
| `POST /api/jog` | `dir=cw` \| `dir=ccw` | rotates until stopped — see the dead-man note in the UI spec |
| `POST /api/stop` | — | jumps the command queue |
| `POST /api/sync` | `raw=370` | declares the rotator's true raw position |
| `GET /api/config` | — | never returns credentials, only whether they are set |
| `POST /api/config` | `wifiSsid=`, `wifiPassword=`, `hostname=`, `rotctldPort=`, `rawPort=`, `serialBaud=` | takes effect after restart |
| `POST /api/restart` | — | |

Refusals are distinguished rather than lumped together: `503 controller in post-boot lockout`, `503 position
unknown`, `400 azimuth unreachable`.

## First boot

With no stored credentials — or if the configured network cannot be reached within 20 s — the bridge starts its own
access point named after its hostname (default `rotator`, password `rotator123`) and serves the same interface
there. It keeps retrying the configured network in the background every two minutes, so a router that rebooted does
not require the bridge to be rebooted too.

## Build

```bash
pio run -e lolin_s3_mini
```

```bash
pio run -e lolin_s3_mini -t upload
```

```bash
pio device monitor
```

Protocol-layer unit tests run on the host — no board required:

```bash
pio test -e native
```

## Wiring

The bridge connects to the **Nano's TX/RX pins**, not its USB port, so opening the link does not reset the
controller.

| ESP32-S3 (S3 Mini) | Controller | Note |
|---|---|---|
| GPIO18 (RX) | Nano TX | **via a divider**, e.g. 1 kΩ + 2 kΩ → 3.33 V. 5 V on an ESP pin destroys it |
| GPIO17 (TX) | Nano RX | 3.3 V clears the AVR's 3.0 V threshold, with little margin |
| GND | GND | common ground required |

No ESP32 board in this size class ships with a level shifter, and none is needed: only the controller→ESP direction
has to come down, which is two resistors. If the ESP→controller direction proves marginal, a 74AHCT125 buffers both.

The controller link is a real hardware UART (`Serial1`) on those pins via the GPIO matrix. UART0 stays on USB for
the console, so its boot-time output never reaches the controller as garbage commands.

## Console (phase 1)

With the serial monitor open at 115200:

| Input | Effect |
|---|---|
| `123` | rotate to 123° |
| `s` | stop |
| `?` | report cached azimuth, freshness, boot lockout, last motion source |
