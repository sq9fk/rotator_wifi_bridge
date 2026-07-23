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
| Customisable heading dial | Must render **450°**, not 360. The 360–450° overlap has to be visible, otherwise the position is ambiguous exactly where the operator most needs to know it. Proposal: a full circle for 0–360 plus an arc segment outside it for the overlap. |
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
- **Position sync** — telling the controller where the rotator actually is. Note this needs the controller's `\A`
  command, which is **compiled out** by `OPTION_SAVE_MEMORY_EXCLUDE_BACKSLASH_CMDS`. Either re-enable it there (it
  costs ~2 KB of the Nano's flash) or leave position sync out of the panel. Decision pending.

## Who is in control

A persistent banner, not an icon, whenever a rotctld or raw client is connected: connection type and client address.
Alongside the azimuth readout, the **source of the last motion command** — because the question when the antenna
starts moving is not "is someone connected" but "why is it turning".

The current azimuth is always shown regardless of which source holds control; the position poller runs
independently of the command sources.
