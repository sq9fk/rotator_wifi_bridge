# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

A WiFi bridge between a [K3NG GS-232B rotator controller](https://github.com/sq9fk/k3ng_controler_nano_light) —
Arduino Nano, azimuth only, 405° rotator, reed-switch position sensor — and the network, running on a LOLIN S3 Mini
(ESP32-S3). It gives that rotator three network faces at once: Hamlib rotctld, a transparent GS-232 socket, and a
password-protected web panel.

**Read [DESIGN.md](DESIGN.md) before changing anything structural**, and [docs/ui-spec.md](docs/ui-spec.md) before
touching the panel. Both record *why* rather than *what*, and most of the non-obvious decisions here were forced by
the controller's behaviour rather than chosen.

## Build, flash, test

```
pio run -e lolin_s3_mini              # compile
pio run -e lolin_s3_mini -t upload    # flash firmware
pio run -e lolin_s3_mini -t uploadfs  # flash the panel (LittleFS) - SEPARATE STEP
pio device monitor                    # console, 115200
pio test -e native                    # protocol unit tests, no board needed
```

The panel lives in `data/www/` and does **not** ship with the firmware. Changing HTML/CSS/JS needs `uploadfs`, not
`upload` — and the static assets carry a `?v=N` query because they are served with a ten-minute cache header;
bump it when you change them or the stale panel is invisible until it is confusing.

`tools/gzip_www.py` (an `extra_scripts` pre-action on the filesystem build) regenerates `data/www/*.gz` on every
`buildfs`/`uploadfs` - never hand-edit or commit those `.gz` files (`.gitignore`'d on purpose): ESPAsyncWebServer's
static handler always prefers a `.gz` sibling over the plain file with no freshness check at all, so a stale
hand-maintained one would silently outlive whatever it was generated from. Also why `favicon.ico` exists at all -
without it, and without its own `.gz`, the browser's automatic favicon request logged a multi-line VFS error
cascade (missing file, missing `.gz`, then the static handler's directory-style fallback) on every page load.

`partitions.csv` at the repo root is pinned deliberately (`board_build.partitions` in `platformio.ini`) - don't
delete it to fall back to PlatformIO's own default. An unpinned scheme can shift on a platform update and make
LittleFS (`config.json`/`favorites.json`) unreadable at boot, which reformats it - see DESIGN.md's "Hardening".

`-D ARDUINO_USB_CDC_ON_BOOT=1` in `build_flags` is required on this board - the LOLIN S3 Mini has no separate
USB-UART bridge chip, so `Serial` is the native USB CDC peripheral, and without this flag its receive side (what
you type into `pio device monitor`) is unreliable on some core versions even though transmit still works fine -
confirmed live as a console that printed nothing back for any input at all. Don't remove it.

CI (`.github/workflows/ci.yml`) runs the native tests and the firmware build on every push.

**Test the panel without hardware** with the simulator: `python sim/sim_server.py`, then open
<http://localhost:8080>. It serves the real `data/www` and fakes the firmware behind it — every endpoint, the
WebSocket, and a rotator that drives toward its target. It exercises the interface, not the C++. See
[sim/README.md](sim/README.md), including the `/sim/*` controls that fake a connected client or a dead serial link.

## Layout

```
lib/gs232/     GS-232 encoding, parsing, azimuth coordinate mapping - pure, no Arduino
lib/rotctl/    Hamlib net rotator command parsing - pure, no Arduino
src/           everything that touches hardware or the network
data/www/      the web panel, uploaded to LittleFS separately
sim/           Python simulator of the firmware, for panel testing
test/          host unit tests for the two pure libraries
```

`lib/` is deliberately Arduino-free so the parts where a mistake sends the antenna the wrong way can be tested on
the host. Keep new protocol logic there rather than in `src/`.

## The rule that shapes everything: one queue

`RotatorLink` is the **sole owner** of the UART; `Rotator` wraps it and is what every control source uses. Never add
a second path to the serial port.

This is a correctness requirement, not tidiness. GS-232 carries no transaction ids, so two overlapping `C` commands
produce two indistinguishable `AZ=` replies and each reader has even odds of taking the other's. That is why the
"transparent" raw socket is transparent at the *line* level — it parses whole commands, pushes them through the same
queue and routes replies back by transaction id.

**The last goto/jog wins, from any source** — a newer one replaces a not-yet-dispatched one in place rather than
queuing behind it, and a stop purges every queued motion command outright (`RotatorLink::submit()`,
`purgeQueuedMotionCommands()`). See DESIGN.md's "One queue for everything" for why, including how a superseded raw
client still gets resolved instead of hanging.

## Controller behaviour you must not forget

| Fact | Consequence in this code |
|---|---|
| `C` gives a **real** azimuth (0–359); `M000`–`M360` takes a real one (the controller picks the raw turn through the overlap), `M361`–`M585` is explicit raw | `Rotator::gotoAzimuth()` sends a **real** azimuth (`M%03d`) and lets the controller choose, exactly like a direct GS-232B logger. `gs232::chooseRawTarget()` is no longer on the goto path — kept in the lib with its tests for a future force-the-far-side feature that would send explicit raw. |
| The fork's `I` reports raw, `Ixxx` sets it | The poller uses `I`, never `C`. Deriving raw from a real azimuth is guesswork that is wrong half the time in the overlap. |
| `M`, `L`, `R`, `A`, `S` answer **nothing** on success | `gs232::classify()` drives the timeout; a transaction cannot just wait for a reply. |
| Rotation commands are **silently dropped for 5 s after the controller boots** | `RotatorLink` refuses motion during the lockout and re-arms it when the link recovers, because a controller that answers after a silence was probably power-cycled. |
| The controller stalls its own loop ~13 ms writing EEPROM | Reply timeout is 300 ms. Tighter produces phantom retries, and a retried motion command is a second rotation. |
| `X1`–`X4` answer but do nothing (relays, no PWM) | Never expose speed control. |
| `AZ Rotation Stall Detected` arrives unsolicited | The reader handles lines with no transaction outstanding. |

## Geometry: raw, real and the overlap band

The rotator's full-CCW stop is at bearing **180°** with **45°** of overlap, so raw runs **180–585**
(`rawMin`/`rawMax` in `Config`, which must match `AZIMUTH_STARTING_POINT` and `AZIMUTH_ROTATION_CAPABILITY` in the
controller — they decide which way it turns).

Raw 180–359 covers bearings 180–359; raw 360–585 covers 0–225. Their intersection, **180–225°**, is the only band
reachable two different ways. Note that "past 360" is **not** the same thing: raw 360–539 is a second lap over
bearings that still have exactly one mechanical position. `Rotator::inOverlap()` uses the narrow definition, and the
panel's red arc appears only inside it.

## Safety-critical paths

The one place where a bug turns into a rotator driven into its end stop:

1. **The jog dead-man timer** (`webapi::poll()`). `L`/`R` rotate until stopped. The panel repeats the command every
   200 ms while held; the bridge issues a stop after 500 ms of silence, and on WebSocket disconnect. Silence must
   never mean "carry on".

Overlap target selection is no longer the bridge's job — `gotoAzimuth()` sends a real azimuth and the controller
picks the raw turn. `gs232::chooseRawTarget()` stays unit tested but is off the live path.

Also: an over-the-network update stops the rotator first, because the reboot would otherwise leave a rotation
running with nothing watching it.

## Task watchdog

`esp_task_wdt_init(8, true)` + `enableLoopWDT()`, called at the very end of `setup()` (mirrors the AVR watchdog in
ant-sw-2x6: enabled last, once every begin() above it has already run). If `loop()` ever hangs, the ESP32 reboots on
its own instead of waiting for someone to notice and power-cycle it. The Arduino core's loop task feeds it every
iteration automatically — don't add a manual `esp_task_wdt_reset()` call anywhere. Distinct from the serial-link
watchdog below, which detects a dead UART link rather than a hung task; don't conflate the two when reading DESIGN.md.

## Heartbeat LED

The onboard WS2812 (`RGB_BUILTIN`, GPIO47) is a no-tools-needed liveness check, added because the board has no other
visible sign of life and USB CDC serial output is easy to miss (it only prints once, at boot, in `setup()`).
`Heartbeat.h`/`.cpp`: a dim blue pulse once a second means the flash succeeded and `loop()` is running, even before
WiFi or the controller link exist; a brief green flash on every line sent to or received from the controller
(`RotatorLink::startNext()`/`readIncoming()`) means that UART link specifically is talking. It runs unconditionally,
not gated by `config.debugEnabled` - it is a physical indicator, not a Monitor-tab capture. `heartbeat::begin()` is
the very first call in `setup()`, before even `Serial.begin()`, so it lights up regardless of what a later `begin()`
does.

## Antenna switch (ant-sw-2x6)

`AntennaSwitch.h`/`.cpp` talks to a separate device, [ant-sw-2x6](https://github.com/sq9fk/ant-sw-2x6) (6 antennas x
2 TRX), over **plain HTTP** (`GET /?S{bank}{ant:02d}`, `GET /?J` for status) — **not OTRSP**, which stays reserved
for the logging program. Uses `AsyncClient` (AsyncTCP), not `WiFiClient`: a blocking connect to a dead/misconfigured
host would stall `loop()` and delay the jog dead-man timer. Off by default (`config.antEnabled`/`antHost`); status
rides the existing `/api/status`/WebSocket stream (`doc["antenna"]`), no separate poll loop in the panel. See
DESIGN.md for the full reasoning, including why status is read from a small endpoint added to that project
(`/?J`) rather than by parsing its HTML page.

`/?K` (antenna names) carries the switch's own site name as a 7th field (`AntennaSwitch::deviceName()`), shown next
to the "Anteny" heading. `AntennaSwitch::fresh()` (connected **and** the last success was within `kFreshMs` = 5 s)
drives a three-state status dot there, same `.dot`/`.dot.warn`/`.dot.bad` idea as the controller link's own `linkDot`.
Every retry path in `poll()` must stay floored by a real interval (`kPollIntervalMs`/`kNamesIntervalMs`) - the
"fetch names right away on enable" fast path used to skip that floor entirely (`!namesFetched` with no time check),
and a synchronously-failing `client.connect()` (bad/unreachable `antHost`) never sets `namesFetched`, so it retried
on every single `loop()` iteration - a real incident, not a hypothetical: it flooded the console with
`connect(): pcb == NULL` and starved WiFi's own reconnect/AP handling badly enough to look like the network stack
itself had broken. See DESIGN.md's "Antenna switch" for the fix.

`config.rotorAnt` (0 = unset) is a **panel-only label** — which antenna number this rotator physically turns — drawn
as an outline on that number in both TRX rows plus a bold legend entry; never fed back into `AntennaSwitch` at all.
Antenna **collision** (same real antenna picked for both TRX) is detected purely client-side from the two numbers
`/?J` already reports — no ant-sw-2x6 firmware change needed for that, unlike the deviceName addition. See
docs/ui-spec.md's antenna sections for the full reasoning behind all four.

## Monitor tab / debug traffic

`DebugLog.h`/`.cpp` is a small ring buffer (24 entries) that `RotctldServer`, `RawServer`, `RotatorLink`, and
`AntennaSwitch` each log into, gated by `config.debugEnabled` **and** a per-stream checkbox
(`debugRotctld`/`debugRaw`/`debugAntenna`/`debugController`) so a chatty stream (the controller poller runs every
~300 ms) doesn't fill the buffer just because the tab happens to be open. The panel's **Monitor** tab (shown only
when `debugEnabled` is set) renders it TX/RX-colored per session. **Only `webapi::poll()`'s periodic WebSocket
broadcast drains the buffer — `buildStatus()` never does**, because it is also called directly by action handlers
(`handleGoto` etc.) to answer their own HTTP response; draining there would consume an entry into a response the
panel's JS never reads instead of the broadcast that actually renders it. See DESIGN.md's "Monitor tab (protocol
traffic)" for the full reasoning.

## Wall clock and "last motion" attribution

`Net.cpp` calls `configTime()` once in station mode (never in the AP-only fallback, which has no internet route)
and again every 6 h - `net::timeSynced()` gates anything that trusts `time(nullptr)`, since before the first sync
it is just a small number near 1970. `WebApi.cpp` separately tracks *which account* last moved the rotator over the
web (`lastWebActor` + a WS-client-id → account map, since a jog message carries no cookie of its own) - `Rotator`'s
own `Source` enum (poller/web/rotctld/raw) is unchanged and stays the safety-relevant one. See DESIGN.md's "Showing
who is in control" for the full reasoning, including the accepted gap around the temporary USB console.

## Auth and TLS

**Multiple accounts (`Config::User[kMaxUsers]`), one session per account** — not a single global session, and not a
fixed pool of session slots either. Logging the same account in twice is refused with the address of the holder and
can take over deliberately, exactly like the original single-account design; the difference is that this is now
per-account rather than global, so two different accounts (e.g. `sq9fk` and `sq9um`) can be logged in at once, each
from their own device. Account management (`auth::upsertUser`/`deleteUser`, `/api/users`) is flat — any authenticated
session can create, reset, or delete any account, including its own; the panel has no separate admin role. The login
throttle (5 failures → 60 s) stays **global across every account**, deliberately not per-account — splitting it would
let wrong guesses against two accounts run in parallel without ever tripping a limit. The password is salted SHA-256
(10k iterations per account), the cookie is `HttpOnly; SameSite=Strict`.
The WebSocket authenticates from the **handshake cookie** at `WS_EVT_CONNECT` (the connect event's `arg` is the
request), not from a token in a message — do not reintroduce the token-in-message scheme, it was broken.

**TLS is not on the device.** ESPAsyncWebServer has no working TLS on ESP32, and more importantly a handshake would
block the cooperative loop and delay the jog dead-man. It is terminated at a reverse proxy instead; the firmware
marks the cookie `Secure` behind `X-Forwarded-Proto: https` and the panel picks `wss` when served over `https`. See
[docs/tls.md](docs/tls.md). Don't add on-device TLS without moving the web layer off the control loop.

## WiFi network selection

**A priority list (`Config::wifiNetworks[5]`), not one SSID/password.** `Net.cpp` tries index 0 first on every
(re)connect, walks down the list on a per-attempt timeout, and only falls back to the bridge's own AP once every
configured slot has failed — list order is the operator's own priority, never picked by signal strength. A dropped
station connection restarts the search from index 0 rather than resuming where it left off. `GET /api/wifi/scan`
is async (`WiFi.scanNetworks(true)`/`scanComplete()`, polled from the panel) — never add a blocking scan call here,
same reasoning as `AntennaSwitch`'s `AsyncClient`: nothing an operator can trigger from the panel may block `loop()`
and delay the jog dead-man. `/api/wifi/networks`(`+/delete`) manage the list like accounts — passwords are
write-only, never echoed back by any GET. The fallback AP itself now has a configurable password too
(`config.apPassword`, also write-only) instead of the old hardcoded constant, and a fixed address
(`10.10.10.1/24` via `softAPConfig()`) instead of the ESP32 default, so it never collides with a station-mode
subnet. See DESIGN.md's "WiFi network selection" for the full reasoning.

## State of verification

**Nothing in this repository has run against a real controller.** What is verified: the firmware builds, the two
pure libraries pass 22 host unit tests, and the panel has been rendered and inspected against mocked state. The
README's "Before first use" section is the bench checklist; the jog dead-man test is the one that matters most.

The likeliest first failures are `\dump_state` conformance against a real `rotctl` and the level-shifted serial
link.
