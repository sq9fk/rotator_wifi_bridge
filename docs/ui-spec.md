# Web panel specification

The look and feature set follow [stefan-wr/esp-rotor-control](https://github.com/stefan-wr/esp-rotor-control) as a
reference. **No code is taken from it** — the implementation here is original, so its Apache-2.0 terms do not attach;
the debt is acknowledged in the README.

Implementation: plain HTML/CSS/JS, no framework. The reference uses Vue 3, which its ESP32 has room for and ours
would too, but nothing in this panel needs a reactive framework: one live value, a dial, a favourites list and a
status banner. Hand-written code keeps the LittleFS bundle small and removes a build step from the firmware
pipeline.

## Layout

Single page, mobile-first, no navigation between screens except a settings drawer.

1. **Status banner** (top, only when relevant) — see "Who is in control" below.
2. **Heading dial** — the dominant element. Compass rose, current heading needle, target marker.
3. **Numeric readout** — current azimuth, target azimuth, and a freshness indicator.
4. **Controls** — CCW / stop / CW buttons, numeric entry, favourites row.
5. **Settings drawer** — calibration, favourites editor, network, session.

## Carried over from the reference

| Feature | Notes for this project |
|---|---|
| Customisable heading dial | Compass ring with degree labels outside it, cardinal marks, a tapered white needle for the position and a dim one for the target, and a hub reading the **real bearing** so the number agrees with where the needle points. A red arc on the ring marks the **ambiguous band**: see below. |
| Point-and-shoot on the dial | Tap a bearing → `requestAzimuth()`. Target chosen by shortest travel, as already implemented in `gs232::chooseRawTarget()`. A long press on the overlap arc forces the far-side target. |
| Keyboard arrow control | Left/right arrow → jog CCW/CW. **Needs a dead-man timer, see below.** |
| Favourites, up to 10, named | Stored in LittleFS as JSON, edited in the settings drawer. |
| Overlap-aware routing | Already in the protocol layer. |
| Colour themes | Light/dark, following the system preference by default. |
| Languages | Polish and English. |
| Guided calibration | See below — the analogue here is degrees-per-pulse, not ADC endpoints. |

## Dropped, with reasons

- **Speed control / smooth acceleration.** The controller answers `X1`–`X4` but does nothing: this board switches
  relays on and off, with no PWM output wired. Showing a speed slider would be a lie about the hardware.
- **Multi-user with temporary lock.** Replaced by a single session with explicit takeover — a deliberate choice, not
  a simplification.
- **SSD1306 status display.** Out of scope for now; the bridge has no local display.

## The overlap arc

The rotator's full-CCW stop is at bearing **180°** and it carries **45°** of overlap, so raw runs **180–585**. Raw
180–359 covers bearings 180–359 and raw 360–585 covers 0–225; their intersection, **180–225°**, is the band
reachable two different ways. That is what the red arc marks. `overlapFrom`/`overlapTo` stay configurable, because
the operator with the rotator in hand is the authority on what it physically does.

The rotor makes **one 405° sweep**, not two laps — from the CCW stop at raw 180 to raw 585. So most bearings have
exactly one raw value and one mechanical position: bearing 89 is only ever raw 449, reached partway through the
single sweep. Only bearings **180–225** are reachable two ways, because the sweep passes them once near the start
(raw 180–225) and again at the end (raw 540–585).

**The arc, the `OL` badge and the raw sub-line all appear only inside that band** — raw ≥ 540. There, and only
there, does raw carry information the bearing does not: at bearing 200 the rotor is at raw 200 or raw 560, and the
red `raw 560°` line under the hub tells you which. Everywhere else raw is fixed by the bearing and showing it would
be noise. Drawing the arc permanently would likewise make it scenery the eye stops seeing.

The hub's big number is the **real bearing**, so it agrees with the needle on the ring. Showing raw there made the
two disagree — 90 on the ring, "449" in the hub — which reads as a fault, and (since 449 is not in the overlap)
told the operator nothing anyway.

## The clock is UTC

Labelled as such. A rotator is used against schedules, beacons and other stations' logs, all of which are in UTC;
showing browser local time invites an off-by-an-hour that nothing else on the screen would reveal.

## Jog control needs a dead-man timer

This is the one place where copying the reference's interaction model would introduce a real hazard.

GS-232 `L` and `R` rotate **until something stops them**. If the operator holds an arrow key and the WebSocket dies —
browser crash, WiFi drop, laptop lid closed — the stop never arrives and the rotator keeps turning into its limit.

So jog is not fire-and-forget: while a jog is held, the panel sends a keepalive (~200 ms), and the bridge issues `A`
the moment one is missed by more than ~500 ms. The panel shows jog state as live, not latched, so a stalled
connection is visible rather than silent. `OPTION_AZ_MANUAL_ROTATE_LIMITS` on the controller is a second line of
defence, not the first.

## Calibration

The reference calibrates ADC endpoints against a potentiometer. There is no potentiometer here — position comes from
counted reed-switch pulses — so the equivalent adjustments are:

- **Degrees per pulse** — the controller's `D`/`Dxxxx` command, persisted in its EEPROM. Guided flow: rotate a known
  arc, enter the true bearing, the panel computes and sets the value.
- **Position sync** — telling the controller where the rotator actually is, via its `Ixxx` command. That command was
  added to the controller for this purpose rather than re-enabling the whole stripped backslash command set for the
  sake of one command. It also gives the bridge a raw position query, which removes the guesswork the poller would
  otherwise need.

## Who is in control

A persistent banner, not an icon, whenever a rotctld or raw client is connected: connection type and client address.
Alongside the azimuth readout, the **source of the last motion command** — because the question when the antenna
starts moving is not "is someone connected" but "why is it turning".

The current azimuth is always shown regardless of which source holds control; the position poller runs
independently of the command sources.
