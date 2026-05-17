#!/usr/bin/env python3
"""
Polymarket bot manager (port 9090, always-on).

Usage:
  python3 tools/manager.py [--port 9090] [--bot-port 5819] \
                           [--bot-bin ./build/polymarket-arb] \
                           [--config ./config/config.json]

Endpoints:
  GET  /                     → dashboard HTML (from src/dashboard.h)
  GET  /api/manager/status   → {"bot_status":"running|stopped|crashed|starting","bot_pid":N}
  POST /api/start            → spawn bot process
  POST /api/shutdown         → forward to bot + track state
  GET  /api/*                → proxy to bot
  POST /api/*                → proxy to bot
"""

import argparse
import http.server
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).parent.parent

# --- mutable global state (all access under _lock) ---
_lock = threading.Lock()
_bot_proc = None          # subprocess.Popen | None
_bot_status = "stopped"   # "stopped" | "starting" | "running" | "crashed"
_bot_exit_code = None     # int | None

_bot_port = 5819
_bot_bin = None
_bot_config = None


# ---------------------------------------------------------------------------
# Dashboard HTML loader
# ---------------------------------------------------------------------------

_dashboard_cache = None

def load_dashboard_html():
    global _dashboard_cache
    src = ROOT / "src" / "dashboard.h"
    try:
        text = src.read_text(encoding="utf-8")
        m = re.search(r'R"html\((.*?)\)html"', text, re.DOTALL)
        if m:
            _dashboard_cache = m.group(1).encode("utf-8")
        else:
            _dashboard_cache = b"<h1>dashboard.h: R\"html( ... )html\" marker not found</h1>"
    except Exception as e:
        _dashboard_cache = f"<h1>Cannot read src/dashboard.h: {e}</h1>".encode()
    return _dashboard_cache


# ---------------------------------------------------------------------------
# Bot proxy helpers
# ---------------------------------------------------------------------------

def _bot_url(path):
    return f"http://127.0.0.1:{_bot_port}{path}"


def _proxy_to_bot(path, method="GET", body=None, timeout=4):
    """Returns (status_code, content_type, body_bytes) or (None, None, None) on error."""
    try:
        headers = {}
        if body:
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(_bot_url(path), data=body, method=method, headers=headers)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.headers.get("Content-Type", "application/json"), resp.read()
    except Exception:
        return None, None, None


def _check_bot_alive():
    sc, _, _ = _proxy_to_bot("/api/status", timeout=2)
    return sc == 200


# ---------------------------------------------------------------------------
# Bot lifecycle monitor
# ---------------------------------------------------------------------------

def _monitor_loop():
    global _bot_status, _bot_exit_code, _bot_proc
    while True:
        with _lock:
            proc = _bot_proc
            status = _bot_status

        if proc is not None:
            ret = proc.poll()
            if ret is not None:
                with _lock:
                    _bot_exit_code = ret
                    _bot_status = "crashed" if ret != 0 else "stopped"
                    _bot_proc = None
            elif status == "starting":
                if _check_bot_alive():
                    with _lock:
                        _bot_status = "running"
        time.sleep(1)


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

class _Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # suppress per-request noise

    def _send(self, code, content_type, body: bytes):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code, obj):
        self._send(code, "application/json", json.dumps(obj).encode())

    def _read_body(self):
        length = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(length) if length else None

    # --- routing ---

    def do_GET(self):
        p = self.path.split("?")[0]
        if p in ("/", "/index.html"):
            html = _dashboard_cache or load_dashboard_html()
            self._send(200, "text/html; charset=utf-8", html)
        elif p == "/api/manager/status":
            self._manager_status()
        elif p.startswith("/api/"):
            self._proxy("GET")
        else:
            self._send(404, "text/plain", b"Not found")

    def do_POST(self):
        p = self.path.split("?")[0]
        if p == "/api/start":
            self._start_bot()
        elif p == "/api/shutdown":
            self._shutdown_bot()
        elif p.startswith("/api/"):
            self._proxy("POST", self._read_body())
        else:
            self._send(404, "text/plain", b"Not found")

    # --- handlers ---

    def _manager_status(self):
        with _lock:
            status = _bot_status
            pid = _bot_proc.pid if _bot_proc else None
            exit_code = _bot_exit_code
        self._json(200, {
            "bot_status": status,
            "bot_pid": pid,
            "bot_exit_code": exit_code,
        })

    def _start_bot(self):
        global _bot_proc, _bot_status, _bot_exit_code
        with _lock:
            if _bot_status in ("running", "starting"):
                self._json(409, {"error": "bot already running", "bot_status": _bot_status})
                return
            if not _bot_bin:
                self._json(500, {"error": "bot binary not found; use --bot-bin"})
                return
            cmd = [_bot_bin]
            if _bot_config:
                cmd.append(_bot_config)
            try:
                log_path = ROOT / "logs" / "bot.log"
                log_path.parent.mkdir(exist_ok=True)
                # stdout/stderr → bot.log so nothing is lost
                log_fd = open(log_path, "a")
                proc = subprocess.Popen(cmd, stdout=log_fd, stderr=log_fd)
                _bot_proc = proc
                _bot_status = "starting"
                _bot_exit_code = None
                pid = proc.pid
            except Exception as e:
                self._json(500, {"error": str(e)})
                return
        print(f"[manager] bot started  pid={pid}  cmd={cmd}", flush=True)
        self._json(200, {"message": "bot starting", "pid": pid})

    def _shutdown_bot(self):
        global _bot_status
        sc, ct, body = _proxy_to_bot("/api/shutdown", "POST")
        if sc is not None:
            with _lock:
                _bot_status = "stopped"
            self._send(sc, ct or "application/json", body)
        else:
            self._json(503, {"error": "bot not reachable"})

    def _proxy(self, method, body=None):
        sc, ct, resp_body = _proxy_to_bot(self.path, method, body)
        if sc is not None:
            self._send(sc, ct or "application/json", resp_body)
        else:
            self._json(503, {"bot_running": False, "error": "bot offline"})


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    global _bot_port, _bot_bin, _bot_config

    ap = argparse.ArgumentParser(description="Polymarket bot manager")
    ap.add_argument("--port",     type=int, default=9090,  help="Manager listen port (default: 9090)")
    ap.add_argument("--host",     default="127.0.0.1",    help="Manager listen host (default: 127.0.0.1; use 0.0.0.0 for external access)")
    ap.add_argument("--bot-port", type=int, default=None,  help="Bot API port (default: read from config.json or 5819)")
    ap.add_argument("--bot-bin",  default=None, help="Path to bot binary")
    ap.add_argument("--config",   default=None, help="Path to config.json passed to bot")
    args = ap.parse_args()

    # resolve config path
    _bot_config = args.config
    if _bot_config is None:
        candidate = ROOT / "config" / "config.json"
        if candidate.exists():
            _bot_config = str(candidate)

    # read bot api_port from config
    if args.bot_port is not None:
        _bot_port = args.bot_port
    elif _bot_config:
        try:
            with open(_bot_config) as f:
                cfg = json.load(f)
            _bot_port = cfg.get("network", {}).get("api_port", 5819)
        except Exception:
            _bot_port = 5819

    # resolve binary
    _bot_bin = args.bot_bin
    if _bot_bin is None:
        for c in [ROOT / "build" / "polymarket-arb"]:
            if c.exists():
                _bot_bin = str(c)
                break

    # pre-load dashboard HTML
    load_dashboard_html()

    # start monitor thread
    threading.Thread(target=_monitor_loop, daemon=True, name="bot-monitor").start()

    # check if bot already running before we started
    global _bot_status
    if _check_bot_alive():
        with _lock:
            _bot_status = "running"
        print("[manager] detected bot already running", flush=True)

    server = http.server.ThreadingHTTPServer((args.host, args.port), _Handler)
    display_host = "127.0.0.1" if args.host == "0.0.0.0" else args.host
    print(f"[manager] dashboard → http://{display_host}:{args.port}", flush=True)
    print(f"[manager] bot API   → http://127.0.0.1:{_bot_port}", flush=True)
    if _bot_bin:
        print(f"[manager] bot bin   → {_bot_bin}", flush=True)
    if _bot_config:
        print(f"[manager] bot cfg   → {_bot_config}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[manager] stopped.", flush=True)


if __name__ == "__main__":
    main()
