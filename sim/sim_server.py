#!/usr/bin/env python3
"""Hardware-free simulator for the rotator_wifi_bridge web panel.

Serves the real panel from ../data/www and stands in for the ESP32 firmware:
every REST endpoint, the /ws WebSocket, and a rotator that actually drives
toward its target so the needle moves, the overlap arc lights up and the
dead-man timer can be exercised. No board, no network, no controller.

    python sim/sim_server.py            # http://localhost:8080

Only the Python standard library is used. The simulated rotator matches the
configured 405 deg rotator: full-CCW stop at bearing 180, raw 180..585.
"""

import base64
import hashlib
import json
import os
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

WWW_DIR = os.path.join(os.path.dirname(__file__), "..", "data", "www")
PORT = 8080

# --- simulated device state -------------------------------------------------

state_lock = threading.Lock()

state = {
    "rawAz": 200.0,          # raw azimuth, 180..585
    "targetRaw": None,       # None when not moving to a target
    "jog": None,             # 'cw' | 'ccw' | None
    "lastJogMs": 0,
    "linkHealthy": True,     # toggle via /sim to test the dead-link banner
    "bootLockout": False,
    "notice": "",
    "lastMotionSource": None,
    "lastMotionMs": 0,
    # fake remote clients, so the banner and session card can be tested
    "rotctldClients": [],
    "rawClients": [],
}

config = {
    "hostname": "rotator",
    "wifiSsid": "TestNet",
    "wifiConfigured": True,
    "rotctldPort": 4533,
    "rawPort": 4532,
    "serialBaud": 9600,
    "rawMin": 180,
    "rawMax": 585,
    "overlapFrom": 180,
    "overlapTo": 225,
}

favorites = [
    {"name": "Polnoc", "az": 0},
    {"name": "Wschod", "az": 90},
    {"name": "Poludnie", "az": 180},
    {"name": "Zachod", "az": 270},
]

# Start with no password so the first-run setup flow can be tested; it then
# persists in memory until the server restarts.
auth = {"password": None, "user": "admin", "token": None, "sessionAddr": None, "sessionStartMs": 0}

ROTATION_SPEED = 12.0        # deg/s, roughly a real azimuth rotator
TOLERANCE = 2.0
JOG_DEADMAN_MS = 500

def now_ms():
    return int(time.time() * 1000)

def raw_to_real(raw):
    return raw % 360.0

def choose_raw_target(desired_real, current_raw):
    real = desired_real % 360.0
    best, best_travel = None, None
    for turn in (-1, 0, 1, 2):
        cand = real + 360.0 * turn
        if cand < config["rawMin"] or cand > config["rawMax"]:
            continue
        travel = abs(cand - current_raw)
        if best is None or travel < best_travel:
            best, best_travel = cand, travel
    return best

def in_overlap(raw):
    return raw >= config["rawMin"] + 360.0

def note_motion(source):
    state["lastMotionSource"] = source
    state["lastMotionMs"] = now_ms()

# --- physics tick -----------------------------------------------------------

def tick_loop():
    last = time.time()
    while True:
        time.sleep(0.05)
        t = time.time()
        dt = t - last
        last = t
        with state_lock:
            # dead-man: silence stops the jog
            if state["jog"] and now_ms() - state["lastJogMs"] > JOG_DEADMAN_MS:
                state["jog"] = None

            step = ROTATION_SPEED * dt
            if state["jog"] == "cw":
                state["rawAz"] = min(config["rawMax"], state["rawAz"] + step)
            elif state["jog"] == "ccw":
                state["rawAz"] = max(config["rawMin"], state["rawAz"] - step)
            elif state["targetRaw"] is not None:
                diff = state["targetRaw"] - state["rawAz"]
                if abs(diff) <= max(step, TOLERANCE):
                    state["rawAz"] = state["targetRaw"]
                    state["targetRaw"] = None
                else:
                    state["rawAz"] += step if diff > 0 else -step

threading.Thread(target=tick_loop, daemon=True).start()

# --- status document (shared by REST and WebSocket) -------------------------

def build_status():
    with state_lock:
        raw = state["rawAz"]
        has_target = state["targetRaw"] is not None
        doc = {
            "position": {
                "fresh": state["linkHealthy"],
                "azimuth": raw_to_real(raw),
                "raw": raw,
                "overlap": in_overlap(raw),
                "ageMs": 0 if state["linkHealthy"] else 5000,
                "hasTarget": has_target,
            },
            "controller": {
                "bootLockout": state["bootLockout"],
                "linkHealthy": state["linkHealthy"],
                "rawMin": config["rawMin"],
                "rawMax": config["rawMax"],
                "overlapFrom": config["overlapFrom"],
                "overlapTo": config["overlapTo"],
            },
            "sources": {
                "rotctld": {"port": config["rotctldPort"], "clients": len(state["rotctldClients"])},
                "raw": {"port": config["rawPort"], "clients": len(state["rawClients"])},
                "remoteConnected": bool(state["rotctldClients"] or state["rawClients"]),
            },
            "network": {"mode": "station", "ssid": config["wifiSsid"],
                        "address": "127.0.0.1", "rssi": -48},
            "jogging": state["jog"] is not None,
            "heapFree": 214000,
            "uptimeMs": now_ms(),
        }
        if has_target:
            doc["position"]["target"] = raw_to_real(state["targetRaw"])
            doc["position"]["targetRaw"] = state["targetRaw"]
        if state["notice"]:
            doc["controller"]["notice"] = state["notice"]
        if state["rotctldClients"]:
            doc["sources"]["rotctld"]["addresses"] = ", ".join(state["rotctldClients"])
        if state["rawClients"]:
            doc["sources"]["raw"]["addresses"] = ", ".join(state["rawClients"])
        if state["lastMotionSource"]:
            doc["lastMotion"] = {"source": state["lastMotionSource"],
                                 "ageMs": now_ms() - state["lastMotionMs"]}
    return doc

# --- WebSocket --------------------------------------------------------------

ws_clients = []
ws_lock = threading.Lock()
WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

def ws_send_text(conn, text):
    data = text.encode("utf-8")
    header = bytearray([0x81])
    n = len(data)
    if n < 126:
        header.append(n)
    elif n < 65536:
        header.append(126)
        header += struct.pack(">H", n)
    else:
        header.append(127)
        header += struct.pack(">Q", n)
    try:
        conn.sendall(bytes(header) + data)
    except OSError:
        pass

def ws_read_frame(rfile):
    b = rfile.read(2)
    if len(b) < 2:
        return None
    opcode = b[0] & 0x0F
    masked = b[1] & 0x80
    length = b[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", rfile.read(2))[0]
    elif length == 127:
        length = struct.unpack(">Q", rfile.read(8))[0]
    mask = rfile.read(4) if masked else b"\x00\x00\x00\x00"
    payload = bytearray(rfile.read(length))
    for i in range(length):
        payload[i] ^= mask[i % 4]
    return opcode, bytes(payload)

def ws_broadcast_loop():
    while True:
        time.sleep(0.25)
        with ws_lock:
            targets = list(ws_clients)
        if targets:
            body = json.dumps(build_status())
            for conn in targets:
                ws_send_text(conn, body)

threading.Thread(target=ws_broadcast_loop, daemon=True).start()

def handle_ws_message(text):
    try:
        msg = json.loads(text)
    except ValueError:
        return
    if "jog" in msg:
        with state_lock:
            j = msg["jog"]
            if j == "stop":
                state["jog"] = None
                state["targetRaw"] = None
            elif j in ("cw", "ccw"):
                state["jog"] = j
                state["lastJogMs"] = now_ms()
                state["targetRaw"] = None
                note_motion("web")

# --- HTTP -------------------------------------------------------------------

CONTENT_TYPES = {".html": "text/html", ".css": "text/css", ".js": "application/javascript"}

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass  # quiet

    # -- helpers --
    def send_json(self, code, obj, extra_headers=None):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if extra_headers:
            for k, v in extra_headers:
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def cookie_token(self):
        cookie = self.headers.get("Cookie", "")
        for part in cookie.split(";"):
            part = part.strip()
            if part.startswith("sid="):
                return part[4:]
        return None

    def authed(self):
        return auth["token"] is not None and self.cookie_token() == auth["token"]

    def body_params(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length).decode("utf-8") if length else ""
        if self.headers.get("Content-Type", "").startswith("application/json"):
            try:
                return json.loads(raw)
            except ValueError:
                return {}
        return {k: v[0] for k, v in parse_qs(raw).items()}

    # -- GET --
    def do_GET(self):
        path = urlparse(self.path).path

        if path == "/ws":
            self.upgrade_ws()
            return

        if path == "/api/session":
            self.send_json(200, {
                "setupRequired": auth["password"] is None,
                "authenticated": self.authed(),
                "user": auth["user"],
                "sessionActive": auth["token"] is not None,
                "sessionAddress": auth["sessionAddr"],
                "sessionAgeMs": now_ms() - auth["sessionStartMs"] if auth["token"] else 0,
            })
            return

        if path == "/api/status":
            if not self.authed():
                self.send_json(401, {"error": "not authenticated"}); return
            self.send_json(200, build_status()); return

        if path == "/api/config":
            if not self.authed():
                self.send_json(401, {"error": "not authenticated"}); return
            self.send_json(200, config); return

        if path == "/api/favorites":
            if not self.authed():
                self.send_json(401, {"error": "not authenticated"}); return
            self.send_json(200, favorites); return

        self.serve_static(path)

    # -- POST --
    def do_POST(self):
        path = urlparse(self.path).path
        p = self.body_params()

        if path == "/api/setup":
            if auth["password"] is not None:
                self.send_json(409, {"error": "already configured"}); return
            if len(p.get("password", "")) < 8:
                self.send_json(400, {"error": "password must be at least 8 characters"}); return
            auth["password"] = p["password"]
            auth["user"] = p.get("user", "admin")
            self.send_json(200, {"ok": True}); return

        if path == "/api/login":
            if p.get("user") != auth["user"] or p.get("password") != auth["password"]:
                self.send_json(401, {"error": "invalid credentials"}); return
            if auth["token"] and p.get("force") != "1":
                self.send_json(409, {"error": "session held", "sessionAddress": auth["sessionAddr"],
                                     "canForce": True}); return
            auth["token"] = base64.b16encode(os.urandom(16)).decode()
            auth["sessionAddr"] = self.client_address[0]
            auth["sessionStartMs"] = now_ms()
            self.send_json(200, {"ok": True},
                           [("Set-Cookie", f"sid={auth['token']}; Path=/; Max-Age=3600")]); return

        if path == "/api/logout":
            auth["token"] = None
            self.send_json(200, {"ok": True}, [("Set-Cookie", "sid=; Path=/; Max-Age=0")]); return

        # Simulator-only controls, before the auth gate so they work from a bare
        # curl as documented - they exist to exercise the panel, not to model
        # anything the firmware does.
        if path == "/sim/rotctld":
            with state_lock:
                state["rotctldClients"] = ["192.168.1.44"] if p.get("on") == "1" else []
                if state["rotctldClients"]:
                    note_motion("rotctld")
            self.send_json(200, {"ok": True}); return
        if path == "/sim/raw":
            with state_lock:
                state["rawClients"] = ["192.168.1.77"] if p.get("on") == "1" else []
            self.send_json(200, {"ok": True}); return
        if path == "/sim/link":
            with state_lock:
                state["linkHealthy"] = p.get("healthy", "1") == "1"
            self.send_json(200, {"ok": True}); return
        if path == "/sim/notice":
            with state_lock:
                state["notice"] = p.get("text", "")
            self.send_json(200, {"ok": True}); return

        if not self.authed():
            self.send_json(401, {"error": "not authenticated"}); return

        if path == "/api/goto":
            az = float(p.get("az", -1))
            if az < 0 or az >= 360:
                self.send_json(400, {"error": "az out of range"}); return
            with state_lock:
                target = choose_raw_target(az, state["rawAz"])
                if target is None:
                    self.send_json(400, {"error": "azimuth unreachable"}); return
                state["targetRaw"] = target
                state["jog"] = None
                note_motion("web")
            self.send_json(200, build_status()); return

        if path == "/api/stop":
            with state_lock:
                state["jog"] = None
                state["targetRaw"] = None
            self.send_json(200, build_status()); return

        if path == "/api/sync":
            with state_lock:
                r = int(p.get("raw", state["rawAz"]))
                if config["rawMin"] <= r <= config["rawMax"]:
                    state["rawAz"] = float(r)
                    state["targetRaw"] = None  # a fresh position invalidates any target in flight
            self.send_json(200, build_status()); return

        if path == "/api/favorites":
            favorites.clear()
            for item in (p if isinstance(p, list) else []):
                favorites.append({"name": str(item.get("name", ""))[:19], "az": float(item.get("az", 0))})
            self.send_json(200, favorites); return

        if path == "/api/config":
            for key in ("hostname", "wifiSsid"):
                if key in p:
                    config[key] = p[key]
            for key in ("rotctldPort", "rawPort", "serialBaud", "overlapFrom", "overlapTo"):
                if key in p:
                    config[key] = int(p[key])
            self.send_json(200, {"saved": True, "restartRequired": True}); return

        if path == "/api/update":
            self.send_json(200, {"ok": True}); return

        if path == "/api/restart":
            self.send_json(200, {"restarting": True}); return

        self.send_json(404, {"error": "not found"})

    # -- static files --
    def serve_static(self, path):
        if path == "/":
            path = "/index.html"
        # strip the ?v=N query, already gone via urlparse
        full = os.path.normpath(os.path.join(WWW_DIR, path.lstrip("/")))
        if not full.startswith(os.path.normpath(WWW_DIR)) or not os.path.isfile(full):
            self.send_json(404, {"error": "not found"}); return
        ext = os.path.splitext(full)[1]
        with open(full, "rb") as f:
            body = f.read()
        self.send_response(200)
        self.send_header("Content-Type", CONTENT_TYPES.get(ext, "application/octet-stream"))
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # -- WebSocket upgrade --
    def upgrade_ws(self):
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            self.send_json(400, {"error": "not a websocket"}); return
        accept = base64.b64encode(hashlib.sha1((key + WS_MAGIC).encode()).digest()).decode()
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()

        conn = self.connection
        with ws_lock:
            ws_clients.append(conn)
        try:
            while True:
                frame = ws_read_frame(self.rfile)
                if frame is None:
                    break
                opcode, payload = frame
                if opcode == 0x8:      # close
                    break
                if opcode == 0x1:      # text
                    handle_ws_message(payload.decode("utf-8", "ignore"))
        except OSError:
            pass
        finally:
            with ws_lock:
                if conn in ws_clients:
                    ws_clients.remove(conn)
            # a panel that vanished mid-jog must not leave the rotator turning
            with state_lock:
                state["jog"] = None


def main():
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(f"rotator_wifi_bridge simulator on http://localhost:{PORT}")
    print("first run: the panel asks you to set a password (min 8 chars)")
    print("sim controls (curl):")
    print(f"  curl -d on=1  http://localhost:{PORT}/sim/rotctld   # fake a connected logger")
    print(f"  curl -d healthy=0 http://localhost:{PORT}/sim/link  # kill the serial link")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
