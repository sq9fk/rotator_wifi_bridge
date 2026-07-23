# rotator_wifi_bridge

WiFi bridge for a [K3NG GS-232B rotator controller](https://github.com/sq9fk/k3ng_controler_nano_light) — Arduino
Nano, azimuth only, 450° rotator — running on a LOLIN S3 Mini (ESP32-S3).

It gives the rotator three network faces at once, all sharing a single serialised link to the controller:

- **rotctld** — Hamlib net rotator protocol on TCP 4533, for logging and contest software.
- **raw** — a transparent GS-232 socket for programs that expect a serial rotator.
- **web panel** — password-protected, one session at a time, served from the ESP.

The current azimuth stays visible in the panel no matter which source is driving the rotator, and the panel shows
which source issued the last motion command.

The web panel's look and feature set follow [stefan-wr/esp-rotor-control](https://github.com/stefan-wr/esp-rotor-control)
as a reference — with thanks. No code is taken from it; see [docs/ui-spec.md](docs/ui-spec.md) for what carries over
and what does not.

**Status: phase 3 of 6.** Working: the serial transaction layer, the position cache, WiFi with an AP fallback,
configuration in LittleFS, the REST API and the rotctld server. Not written yet: the raw passthrough socket, the web
panel and its authentication. See [DESIGN.md](DESIGN.md) for the architecture and the reasoning behind it.

## rotctld

Hamlib net rotator protocol, **port configurable** (`rotctldPort`, default 4533):

```bash
rotctl -m 2 -r rotator.local:4533
```

Supported: `p` / `\get_pos`, `P az el` / `\set_pos`, `S` / `\stop`, `M <dir> <speed>` (left and right only), `_` /
`\get_info`, `\dump_state`, `q`. Park and reset answer `RPRT -4`. Elevation is reported as a constant `0`.

Two behaviours worth knowing:

- **Positions are exchanged in real azimuth, 0–360**, which is what logging software thinks in. The overlap zone is
  not exposed as extra degrees; the bridge picks the raw target with the shorter travel from the current position.
- **`p` returns `RPRT -6` when the cached position is stale** rather than the last known heading. Reporting a
  heading the rotator has left behind is how an operator ends up trusting a stale number.

`\dump_state` answers protocol version 1 with an explicit `min_az`/`max_az`. Version 0 would leave the Hamlib client
using its built-in ±180° range.

## REST API

Unauthenticated for now — session handling arrives with the panel in phase 5. Do not expose this to an untrusted
network yet.

| Endpoint | Body | Notes |
|---|---|---|
| `GET /api/status` | — | position with freshness, overlap flag, boot lockout, last motion source, network, heap |
| `POST /api/goto` | `az=123` | 0–359; the raw target is chosen for shortest travel |
| `POST /api/jog` | `dir=cw` \| `dir=ccw` | rotates until stopped — see the dead-man note in the UI spec |
| `POST /api/stop` | — | jumps the command queue |
| `POST /api/sync` | `raw=370` | declares the rotator's true raw position |
| `GET /api/config` | — | never returns credentials, only whether they are set |
| `POST /api/config` | `wifiSsid=`, `wifiPassword=`, `hostname=`, `rotctldPort=`, `rawPort=` | takes effect after restart |
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
