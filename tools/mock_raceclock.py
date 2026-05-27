#!/usr/bin/env python3
"""
mock_raceclock.py — Simulates a second SEM Race Clock on the local network.

Registers _raceclock._tcp via mDNS with a hostname of raceclock2.local (or
raceclockN.local for --num N) and serves the Race Clock REST API, so you can
test peer discovery and session sync without a second ESP32 board.

Usage
─────
  sudo python3 tools/mock_raceclock.py           # full test — port 80
  python3 tools/mock_raceclock.py                # falls back to port 8080
  python3 tools/mock_raceclock.py --num 3        # simulate device 3 instead of 2
  python3 tools/mock_raceclock.py --push         # push mock sessions → real board
  python3 tools/mock_raceclock.py --push --push-delay 5   # wait 5 s before pushing

What gets tested
────────────────
  Detection   The real board boots, scans, finds this mock via mDNS, and shows it
              in the WiFi card under "PEERS".  It also uses the mock's presence to
              negotiate its own device number (stays at 1 because 2 is taken).

  Push IN     Click "↑ Push sessions" in the web UI — the board posts its sessions
              here.  The mock validates each session and prints a PASS/FAIL summary.

  Push OUT    Run with --push.  After startup the mock discovers the real board via
              mDNS and posts its three sample sessions to it, printing a PASS/FAIL
              summary.  The board's session list updates immediately.

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
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

from zeroconf import ServiceBrowser, ServiceInfo, Zeroconf

# ── Sample sessions (deliberately different from what the real board might have)
MOCK_SESSIONS = [
    {"type": "Prototype",     "start": "09:00", "lastStart": "09:50", "end": "10:00"},
    {"type": "Urban Concept", "start": "10:30", "lastStart": "11:20", "end": "11:30"},
    {"type": "Prototype",     "start": "14:00", "lastStart": "14:50", "end": "15:00"},
]

# ── Push-test state ───────────────────────────────────────────────────────────
# When the real board pushes sessions here (clear → N×POST /api/sessions) we
# validate each received session and print a PASS/FAIL summary to the terminal.
_push_lock     = threading.Lock()
_push_timer    = None   # threading.Timer — fires when the push burst ends
_push_source   = None   # IP of the sender
_push_received = []     # sessions collected in the current push


def _t2m(t: str) -> int:
    """'HH:MM' → minutes since midnight, or -1 on error."""
    try:
        h, m = t.split(":")
        return int(h) * 60 + int(m)
    except Exception:
        return -1


def _validate(s: dict) -> list:
    """Return list of error strings; empty list means the session is valid."""
    errs = []
    for f in ("type", "start", "lastStart", "end"):
        if not s.get(f):
            errs.append(f"missing '{f}'")
    if not errs:
        st, ls, en = _t2m(s["start"]), _t2m(s["lastStart"]), _t2m(s["end"])
        if st < 0 or ls < 0 or en < 0:
            errs.append("time not in HH:MM format")
        elif not (st < ls < en):
            errs.append(f"order wrong  ({s['start']} < {s['lastStart']} < {s['end']}  must hold)")
    return errs


def _push_summary():
    """Print test results after a push sequence completes (called from Timer thread)."""
    with _push_lock:
        sessions = list(_push_received)
        source   = _push_source or "?"

    W = 52  # box width
    print()
    print(f"  ╔{'═' * W}╗")
    print(f"  ║{'  Push test  —  from ' + source :<{W}}║")
    print(f"  ╠{'═' * W}╣")

    if not sessions:
        print(f"  ║  {'✗  No sessions received — empty schedule was pushed.':<{W-2}}║")
        verdict, icon = "FAIL", "❌"
    else:
        all_ok = True
        for i, s in enumerate(sessions, 1):
            errs = _validate(s)
            if errs:
                all_ok = False
                for e in errs:
                    line = f"✗  Session {i}: {e}"
                    print(f"  ║  {line:<{W-2}}║")
            else:
                line = f"✓  [{i}] {s['type']}  {s['start']} → {s['lastStart']} → {s['end']}"
                print(f"  ║  {line:<{W-2}}║")
        verdict, icon = ("PASS", "✅") if all_ok else ("FAIL", "❌")

    print(f"  ╠{'═' * W}╣")
    n   = len(sessions)
    msg = f"{icon}  {verdict}  —  {n} session{'s' if n != 1 else ''} received"
    print(f"  ║  {msg:<{W-2}}║")
    print(f"  ╚{'═' * W}╝")
    print()


# ── Outbound push ────────────────────────────────────────────────────────────

def _http_post(ip: str, port: int, path: str, payload: dict) -> tuple:
    """POST JSON to ip:port/path.  Returns (http_status, response_dict)."""
    url  = f"http://{ip}:{port}{path}"
    data = json.dumps(payload).encode()
    req  = urllib.request.Request(
        url, data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, json.loads(resp.read())
    except urllib.error.HTTPError as e:
        try:
            body = json.loads(e.read())
        except Exception:
            body = {}
        return e.code, body
    except Exception as e:
        raise RuntimeError(str(e)) from e


def _push_to_peer(name: str, ip: str, port: int, sessions: list) -> None:
    """Push mock sessions to one peer clock and print a boxed PASS/FAIL result."""
    W = 52
    print()
    print(f"  ╔{'═' * W}╗")
    print(f"  ║{'  Push test  —  to ' + name:<{W}}║")
    print(f"  ╠{'═' * W}╣")

    try:
        # Step 1 — clear
        status, body = _http_post(ip, port, "/api/sessions/clear", {})
        if status not in range(200, 300):
            msg = f"✗  Clear failed (HTTP {status})"
            print(f"  ║  {msg:<{W-2}}║")
            print(f"  ╠{'═' * W}╣")
            print(f"  ║  {'❌  FAIL':<{W-2}}║")
            print(f"  ╚{'═' * W}╝")
            print()
            return

        # Step 2 — push each session
        ok = fail = 0
        for i, s in enumerate(sessions, 1):
            status, body = _http_post(ip, port, "/api/sessions",
                                      {"index": -1, "session": s})
            if status in range(200, 300) and body.get("ok"):
                ok += 1
                line = (f"✓  [{i}] {s.get('type','?')}  "
                        f"{s.get('start','?')} → {s.get('lastStart','?')} → {s.get('end','?')}")
            else:
                fail += 1
                err  = body.get("error", f"HTTP {status}")
                line = f"✗  [{i}] {s.get('type','?')}: {err}"
            print(f"  ║  {line:<{W-2}}║")

        verdict = "PASS" if fail == 0 else "FAIL"
        icon    = "✅" if fail == 0 else "❌"

    except RuntimeError as e:
        print(f"  ║  {'✗  Connection failed: ' + str(e):<{W-2}}║")
        verdict, icon = "FAIL", "❌"

    n   = len(sessions)
    msg = f"{icon}  {verdict}  —  {n} session{'s' if n != 1 else ''} pushed"
    print(f"  ╠{'═' * W}╣")
    print(f"  ║  {msg:<{W-2}}║")
    print(f"  ╚{'═' * W}╝")
    print()


def push_to_peers(zc: Zeroconf, local_ip: str, local_port: int,
                  sessions: list, delay: float = 3.0) -> None:
    """Discover other race clocks via mDNS and push sessions to each one."""
    print(f"\n  Waiting {delay:.0f} s for mDNS discovery before pushing…")
    time.sleep(delay)

    found: dict[str, tuple] = {}   # name → (ip, port)
    found_lock = threading.Lock()

    class _Listener:
        def add_service(self, zc_, type_, name):
            info = zc_.get_service_info(type_, name)
            if not info or not info.addresses:
                return
            ip   = socket.inet_ntoa(info.addresses[0])
            port = info.port
            if ip == local_ip and port == local_port:
                return  # skip self
            with found_lock:
                found[name] = (ip, port)

        def remove_service(self, *_): pass
        def update_service(self, *_): pass

    # Use a *separate* Zeroconf instance for browsing so the ServiceBrowser
    # never touches zc — the instance that is advertising our own service.
    # Closing browse_zc also cancels the browser cleanly.
    browse_zc = Zeroconf(interfaces=[local_ip])
    ServiceBrowser(browse_zc, "_raceclock._tcp.local.", _Listener())
    time.sleep(2)          # let the browser collect responses
    browse_zc.close()

    with found_lock:
        peers = dict(found)

    if not peers:
        print("  No other race clocks found — nothing to push to.\n")
        return

    for name, (ip, port) in peers.items():
        _push_to_peer(name, ip, port, sessions)


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
            global _push_timer, _push_source, _push_received

            length = int(self.headers.get("Content-Length", 0))
            body   = self.rfile.read(length)

            if self.path == "/api/sessions/clear":
                before = len(MOCK_SESSIONS)
                MOCK_SESSIONS.clear()
                with _push_lock:
                    if _push_timer:
                        _push_timer.cancel()
                        _push_timer = None
                    _push_source   = self.client_address[0]
                    _push_received = []
                print(f"\n  📥  Push started from {_push_source}"
                      f" — clearing {before} existing session(s)…")
                self._json({"ok": True})

            elif self.path == "/api/sessions":
                try:
                    payload = json.loads(body)
                    session = payload.get("session", payload)
                    MOCK_SESSIONS.append(session)
                    with _push_lock:
                        _push_received.append(session)
                        if _push_timer:
                            _push_timer.cancel()
                        # Fire summary 0.8 s after the last session in the burst
                        _push_timer = threading.Timer(0.8, _push_summary)
                        _push_timer.start()
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
    parser.add_argument(
        "--push", action="store_true",
        help="After startup, push mock sessions to all discovered real clocks",
    )
    parser.add_argument(
        "--push-delay", type=float, default=3.0, metavar="SECS",
        help="Seconds to wait for mDNS before pushing (default: 3)",
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
    push_mode = f"--push  (after {args.push_delay:.0f} s)" if args.push else "off"
    print(f"│  Push out : {push_mode:<33}│")
    print("└─────────────────────────────────────────────┘")
    print()
    if args.push:
        print("Will push mock sessions to real clocks after mDNS settles.")
    print("Waiting for the real board to discover this mock…")
    print("Press Ctrl+C to stop.\n")

    # ── Outbound push (background thread, fires after --push-delay) ──────────
    if args.push:
        def _deferred_push():
            push_to_peers(zc, local_ip, port, MOCK_SESSIONS, args.push_delay)
        threading.Thread(target=_deferred_push, daemon=True).start()

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
