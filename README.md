# rotator_wifi_bridge

WiFi bridge for a [K3NG GS-232B rotator controller](https://github.com/sq9fk/k3ng_controler_nano_light) — Arduino
Nano, azimuth only, 450° rotator — running on a LOLIN S3 Mini (ESP32-S3).

It gives the rotator three network faces at once, all sharing a single serialised link to the controller:

- **rotctld** — Hamlib net rotator protocol on TCP 4533, for logging and contest software.
- **raw** — a transparent GS-232 socket for programs that expect a serial rotator.
- **web panel** — password-protected, one session at a time, served from the ESP.

The current azimuth stays visible in the panel no matter which source is driving the rotator, and the panel shows
which source issued the last motion command.

**Status: phase 1 of 6.** The serial transaction layer and position cache work and are driven from the USB console;
WiFi, rotctld, the raw socket and the web panel are not written yet. See [DESIGN.md](DESIGN.md) for the architecture
and the reasoning behind it.

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
