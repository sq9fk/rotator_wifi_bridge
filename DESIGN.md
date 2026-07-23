# Design

WiFi bridge between a [K3NG GS-232B rotator controller](https://github.com/sq9fk/k3ng_controler_nano_light)
(Arduino Nano, azimuth only, 450° rotator) and the network. Runs on a Wemos D1 mini (ESP8266).

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

- Controller TX → ESP RX (GPIO12/D6) **through a divider**. 5 V on an ESP pin destroys it.
- ESP TX (GPIO14/D5) → controller RX. 3.3 V clears the AVR's 3.0 V threshold, but with little margin; a 74AHCT125
  buffer solves both directions if it proves flaky.
- The controller link runs on `SoftwareSerial` (reliable at 9600) so hardware UART0 stays free for the console.
  UART0 would otherwise spray boot-time output at 74880 baud into the controller as garbage commands.

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

## ESP8266 budget

~40 KB of free heap with WiFi up. Hard limits on concurrent clients (2 × rotctld, 1 × raw, 1 × WebSocket), fixed
buffers instead of `String` concatenation in hot paths — heap fragmentation is the classic way this platform dies
after a few days of uptime. Limits are configurable once measurements justify raising them.

Baseline, phase 1: 29112 B RAM / 275379 B flash.

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
