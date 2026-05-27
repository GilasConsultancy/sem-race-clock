#!/usr/bin/env python3
"""
mock_raceclock.py — Simulates a second SEM Race Clock on the local network.

Registers _raceclock._tcp via mDNS with a hostname of raceclock2.local (or
raceclockN.local for --num N) and serves the Race Clock REST API, so you can
test peer discovery and session sync without a second ESP32 board.

Usage
─────
  sudo python3 tools/mock_raceclock.py          # full test — port 80, sync works
  python3 tools/mock_raceclock.py               # detection only — falls back to 8080
  python3 tools/mock_raceclock.py --num 3       # simulate device 3 instead of 2

What gets tested
────────────────
  Detection  The real board boots, scans, finds this mock via mDNS, and shows it
             in the WiFi card under "OTHER RACE CLOCKS ON NETWORK".  It also uses
             the mock's presence to negotiate its own device number (stays at 1
             because 2 is taken).

  Sync       Clicking "Sync sessions" in the web UI fetches /api/sessions from the
             mock and copies the three sample sessions to the real board.
             *** This requires port 80 — run with sudo. ***
             Without sudo the script falls back to port 8080; detection still works
             but the browser fetch (http://<ip>/api/sessions) hits port 80 and fails.

Requirements
────────────
  Python 3.  zeroconf is installed automatically into tools/.venv on first run.
"""

import os
import subprocess
import sys

# ── Bootstrap: ensure zeroconf is available ──────────────────────────────────
# Homebrew and other managed Python installations (PEP 668) block system-wide
# pip installs.  We sidestep this by creating a private .venv in the tools/
# directory on the first run, installing zeroconf there, then re-exec'ing
# this script inside that venv's Python.  Subsequent runs skip straight to
# the import — the whole thing is transparent.
def _bootstrap():
    try:
        import zeroconf  # noqa: F401 — just checking availability
        return           # already importable, nothing to do
    except ImportError:
        pass

    venv_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".venv")
    venv_py  = os.path.join(venv_dir, "bin", "python3")

    if not os.path.exists(venv_py):
        print("First run: creating tools/.venv and installing zeroconf…")
        subprocess.check_call([sys.executable, "-m", "venv", venv_dir])
        subprocess.check_call([venv_py, "-m", "pip", "install", "-q", "zeroconf"])
        print("Done — starting mock.\n")

    # Re-exec this script inside the venv Python (transparent to the user).
    # Use an absolute path for argv[0] so execv works regardless of cwd.
    abs_script = os.path.abspath(sys.argv[0])
    os.execv(venv_py, [venv_py, abs_script] + sys.argv[1:])

_bootstrap()
# ─────────────────────────────────────────────────────────────────────────────

import argparse
import json
import socket
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

from zeroconf import ServiceInfo, Zeroconf

# ── Sample sessions (deliberately different from what the real board might have)
MOCK_SESSIONS = [
    {"type": "Prototype",     "start": "09:00", "lastStart": "09:50", "end": "10:00"},
    {"type": "Urban Concept", "start": "10:30", "lastStart": "11:20", "end": "11:30"},
    {"type": "Prototype",     "start": "14:00", "lastStart": "14:50", "end": "15:00"},
]


# ── Helpers ──────────────────────────────────────────────────────────────────
def get_local_ip() -> str:
    """Return the LAN IP of this machine (not 127.0.0.1)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    finally:
        s.close()


def can_bind(port: int) -> bool:
    try:
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("", port))
        s.close()
        return True
    except OSError:
        return False


# ── HTTP request handler ─────────────────────────────────────────────────────
def make_handler(device_num: int, hostname: str):
    """Return a BaseHTTPRequestHandler subclass closed over device_num / hostname."""

    class Handler(BaseHTTPRequestHandler):

        # ── CORS preflight ──────────────────────────────────────────────────
        def do_OPTIONS(self):
            self.send_response(204)
            self._cors_headers()
            self.end_headers()

        # ── GET routes ──────────────────────────────────────────────────────
        def do_GET(self):
            if self.path == "/api/sessions":
                self._json(MOCK_SESSIONS)

            elif self.path == "/api/settings":
                self._json({
                    "tz":          "Europe/Warsaw",
                    "warnMinutes": 5,
                    "maxSessions": 20,
                    "ntpSynced":   True,
                    "apMode":      False,
                    "hostname":    hostname,
                    "deviceNum":   device_num,
                })

            elif self.path == "/api/time":
                self._json({"epoch": int(time.time()), "synced": True})

            elif self.path == "/api/display":
                self._json({
                    "state":   "NO_SESSION",
                    "line1":   "NO SESSION",
                    "line2":   "",
                    "blinkMs": 0,
                    "scroll":  False,
                })

            elif self.path == "/api/peers":
                self._json([])   # mock sees no further peers

            elif self.path == "/api/override":
                self._json({"text": ""})

            elif self.path == "/api/version":
                self._json({"version": "0.4.6-mock"})

            else:
                self.send_response(404)
                self._cors_headers()
                self.end_headers()

        # ── POST routes ─────────────────────────────────────────────────────
        def do_POST(self):
            length = int(self.headers.get("Content-Length", 0))
            body   = self.rfile.read(length)

            if self.path == "/api/sessions/clear":
                MOCK_SESSIONS.clear()
                self._json({"ok": True})

            elif self.path == "/api/sessions":
                try:
                    payload = json.loads(body)
                    session = payload.get("session", payload)
                    MOCK_SESSIONS.append(session)
                    self._json({"ok": True})
                except Exception:
                    self.send_response(400)
                    self._cors_headers()
                    self.end_headers()

            else:
                self._json({"ok": True})  # accept any other POST silently

        # ── Helpers ─────────────────────────────────────────────────────────
        def _json(self, data):
            body = json.dumps(data, indent=2).encode()
            self.send_response(200)
            self.send_header("Content-Type",   "application/json")
            self.send_header("Content-Length", str(len(body)))
            self._cors_headers()
            self.end_headers()
            self.wfile.write(body)

        def _cors_headers(self):
            self.send_header("Access-Control-Allow-Origin",  "*")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")

        def log_message(self, fmt, *args):
            # Terse one-liner per request
            print(f"  [{hostname}] {args[0]}  →  {args[1]}")

    return Handler


# ── Main ─────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="Mock SEM Race Clock peer",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--num", type=int, default=2, metavar="N",
        help="Device number to simulate (default: 2 → raceclock2.local)",
    )
    args = parser.parse_args()

    device_num = args.num
    hostname   = "raceclock" if device_num == 1 else f"raceclock{device_num}"
    local_ip   = get_local_ip()

    # ── Choose port ──────────────────────────────────────────────────────────
    # Try port 80 first, fall back to 8080.
    # With the firmware updated to include port in /api/peers, sync now works
    # on any port — the web UI picks up the correct port from the mDNS data.
    if can_bind(80):
        port    = 80
        sync_ok = True
    else:
        port    = 8080
        sync_ok = True   # works now — firmware passes port to web UI
        try:
            # Check whether it's a permission issue or port-in-use
            import ctypes
            _ = ctypes.CDLL(None)
            is_root = (os.geteuid() == 0)
        except Exception:
            is_root = (os.geteuid() == 0) if hasattr(os, "geteuid") else False

        if not is_root:
            reason = "needs root"
            hint   = f"sudo python3 {sys.argv[0]}"
        else:
            reason = "already in use by another process (Apache/httpd)"
            hint   = "sudo apachectl stop   # then re-run this script"
        print()
        print(f"  ℹ  Port 80 {reason} — using port 8080 instead.")
        print(f"     Sync still works: the board reads the port from mDNS.")
        print(f"     To use port 80:  {hint}")
        print()

    # ── mDNS registration ────────────────────────────────────────────────────
    # ServiceInfo parameters:
    #   type_   = the service type  (_raceclock._tcp.local.)
    #   name    = instance name     (raceclock2._raceclock._tcp.local.)
    #   server  = SRV target host   (raceclock2.local.)  ← what MDNS.hostname(i) returns
    #   addresses / port            = where the HTTP server actually listens
    # Bind to the specific LAN interface so the A record carries the real IP,
    # not 0.0.0.0.  Without this, zeroconf may advertise the all-interfaces
    # address which the ESP32 reports back as 0.0.0.0.
    zc   = Zeroconf(interfaces=[local_ip])
    info = ServiceInfo(
        "_raceclock._tcp.local.",
        f"{hostname}._raceclock._tcp.local.",
        addresses=[socket.inet_aton(local_ip)],
        port=port,
        server=f"{hostname}.local.",
    )
    zc.register_service(info)

    # ── Banner ───────────────────────────────────────────────────────────────
    print()
    print("┌─────────────────────────────────────────────┐")
    print("│         Mock SEM Race Clock peer             │")
    print("├─────────────────────────────────────────────┤")
    print(f"│  Device   : {hostname:<33}│")
    print(f"│  Hostname : {hostname + '.local':<33}│")
    print(f"│  IP       : {local_ip:<33}│")
    print(f"│  Port     : {port:<33}│")
    print(f"│  mDNS     : _raceclock._tcp ✓              │")
    print(f"│  Sessions : {len(MOCK_SESSIONS)} mock sessions                  │")
    sync_url = f"http://{local_ip}:{port}/api/sessions"
    print(f"│  Sync     : ✓  {sync_url:<29}│")
    print("└─────────────────────────────────────────────┘")
    print()
    print("Waiting for the real board to discover this mock…")
    print("Press Ctrl+C to stop.\n")

    # ── HTTP server (runs in this thread) ────────────────────────────────────
    httpd = HTTPServer(("0.0.0.0", port), make_handler(device_num, hostname))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down…")
    finally:
        zc.unregister_service(info)
        zc.close()
        print("Done.")


if __name__ == "__main__":
    main()
