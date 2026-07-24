# Panel simulator

A hardware-free test environment for the web panel. It serves the real panel
from `../data/www` and stands in for the ESP32 firmware — every REST endpoint,
the `/ws` WebSocket, and a rotator that actually drives toward its target so
the needle moves, the overlap arc lights up and the jog dead-man timer can be
exercised. No board, no controller, no network.

Only the Python standard library is used (tested on 3.11).

## Run

```bash
python sim/sim_server.py
```

Then open <http://localhost:8080>. On first load the panel asks you to set a
password (minimum 8 characters); it persists in memory until the server stops.

The simulator also brings up the **same rotctld and raw TCP servers** as the
firmware, so they can be tested with real clients without hardware:

```bash
rotctl -m 2 -r localhost:4533   # then: p, P 90 0, S, _, q
nc localhost 4532               # raw GS-232: C, I, M090, S
```

The rotctld side speaks the Hamlib 4.7.2 net rotator protocol
(`\dump_state` version 1, `p`/`P`/`S`/`M`/`_`/`q`; park/reset answer `RPRT -4`).
The raw side models the controller's serial replies (`AZ=`, `RAW=`, `?>`, and
silence on a successful move). Both honour the configured client limits, so you
can raise `rawMaxClients` to 2 and confirm two raw clients get their own replies
with no cross-talk.

The simulated rotator matches the real one: full-CCW stop at bearing 180°, raw
azimuth 180–585, overlap band 180–225°. `goto` picks the shorter-travel raw
target exactly as the firmware does, so a bearing in the overlap can send the
needle either way depending on where it starts.

## What you can test directly

- Login, the single-session refusal and the takeover button (open a second tab).
- Point-and-shoot on the dial, the numeric go field, and the favourites.
- Jog with the on-screen buttons or the arrow keys, and the dead-man stop —
  hold a jog and close the tab; the rotator halts.
- The overlap arc and `OL` badge appearing only inside raw 540–585.
- The settings tiles, editing favourites, changing the overlap band live.

## Faking the things a browser cannot

A few `/sim/*` endpoints drive state the panel only observes. They need no
login:

```bash
curl -d on=1        http://localhost:8080/sim/rotctld   # a logger appears on the banner
curl -d on=0        http://localhost:8080/sim/rotctld   # ...and disconnects
curl -d on=1        http://localhost:8080/sim/raw        # a raw client connects
curl -d state=ok    http://localhost:8080/sim/link       # green dot: talking to the controller
curl -d state=stale http://localhost:8080/sim/link       # yellow dot: no fresh poll, link not dead
curl -d state=dead  http://localhost:8080/sim/link       # red dot + banner: link severed
curl --data-urlencode "text=AZ Rotation Stall Detected" http://localhost:8080/sim/notice
```

## What this does NOT test

The simulator reimplements the panel-facing behaviour in Python; it is not the
firmware. It cannot catch a bug that lives in the C++ — the GS-232 encoding,
the real transaction queue, `chooseRawTarget()` on the device, rotctld
conformance, or the serial timing. Those still need the bench. This is for the
interface: layout, flows, wording, and the panel's own logic.
