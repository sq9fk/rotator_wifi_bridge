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

CI (`.github/workflows/ci.yml`) runs the native tests and the firmware build on every push.

## Layout

```
lib/gs232/     GS-232 encoding, parsing, azimuth coordinate mapping - pure, no Arduino
lib/rotctl/    Hamlib net rotator command parsing - pure, no Arduino
src/           everything that touches hardware or the network
data/www/      the web panel, uploaded to LittleFS separately
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

## Controller behaviour you must not forget

| Fact | Consequence in this code |
|---|---|
| `C` gives a **real** azimuth (0–359), `M###` takes a **raw** one (180–585) | Read and write use different coordinate systems. Only `gs232::chooseRawTarget()` knows this. |
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

Two places where a bug turns into a rotator driven into its end stop. Treat changes to them accordingly.

1. **The jog dead-man timer** (`webapi::poll()`). `L`/`R` rotate until stopped. The panel repeats the command every
   200 ms while held; the bridge issues a stop after 500 ms of silence, and on WebSocket disconnect. Silence must
   never mean "carry on".
2. **`gs232::chooseRawTarget()`**. A wrong raw target sends the antenna the long way round, possibly through the end
   stop. It is unit tested; keep it that way.

Also: an over-the-network update stops the rotator first, because the reboot would otherwise leave a rotation
running with nothing watching it.

## State of verification

**Nothing in this repository has run against a real controller.** What is verified: the firmware builds, the two
pure libraries pass 22 host unit tests, and the panel has been rendered and inspected against mocked state. The
README's "Before first use" section is the bench checklist; the jog dead-man test is the one that matters most.

The likeliest first failures are `\dump_state` conformance against a real `rotctl` and the level-shifted serial
link.
