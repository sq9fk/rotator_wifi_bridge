# Design

WiFi bridge between a [K3NG GS-232B rotator controller](https://github.com/sq9fk/k3ng_controler_nano_light)
(Arduino Nano, azimuth only, 450° rotator) and the network. Runs on a LOLIN S3 Mini (ESP32-S3).

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
| `C` reports a **real** azimuth (0–359); `M###` takes a **raw** one (180–630) | Read and write are in different coordinate systems. `gs232::chooseRawTarget()` maps between them and is the only place that knows this. |
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

- Password stored as PBKDF2-SHA256 with a per-device salt, never in clear.
- **One session at a time.** A second login is refused with the address of the holder, plus an explicit takeover
  that invalidates it — otherwise a closed browser tab locks the panel until reboot. Idle sessions expire (~15 min);
  repeated failures are rate-limited.
- **Without TLS the password crosses the LAN in clear.** Acceptable on a private network, not if the ESP is
  exposed. BearSSL on ESP8266 works but costs ~40 KB of heap and a slow handshake; deferred until the memory
  picture is clear.

## Showing who is in control

`SessionRegistry` tracks every control path — rotctld, raw, web — with address, connect time and last command time,
pushed to the panel over WebSocket.

The panel shows a **persistent banner**, not an icon, whenever anything other than the local session is connected.
It also shows **which source issued the last motion command**, because the question that actually matters when the
antenna starts moving is not "is someone connected" but "why is it turning".

Optional: exclusive takeover from the panel, blocking rotctld and raw during manual work.

## Memory budget

320 KB RAM, 4 MB flash. Client limits stay configurable (2 × rotctld, 1 × raw, 1 × WebSocket) but are now a policy
choice rather than a survival measure. Fixed buffers in the serial path remain, because they also make the
transaction layer easier to reason about.

Baseline, phase 1: 19180 B RAM (5.9 %) / 270993 B flash (20.7 %).

## Phases

1. **Serial layer** — `Gs232` (pure, unit tested) + `RotatorLink` + position cache, driven from the USB console. ← current
2. WiFi, config in LittleFS, AP fallback, REST status/goto/stop.
3. `RotctldServer` on 4533, verified with `rotctl -m 2 -r <host>:4533`.
4. Raw passthrough socket + `SessionRegistry`.
5. Web panel (design to follow) over WebSocket.
6. OTA, hardening, serial-link watchdog.

## Testing

`lib/gs232` has no Arduino dependency so it runs under `pio test -e native` on the host. That is where the coordinate
mapping and command classification are tested — the parts where a mistake means the antenna goes the wrong way.
Everything above it needs the real controller.
