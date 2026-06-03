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
_bot_start_time = 0.0     # monotonic ts of last spawn (for crash-loop backoff)
_bot_restart_count = 0    # consecutive rapid crashes
_bot_log_fd = None        # open file handle for bot stdout/stderr
_auto_restart = True      # cleared on intentional shutdown, re-set on manual start

# auto-restart crash-loop backoff
_RESTART_MIN_UPTIME = 60.0    # crash sooner than this (s) counts as a crash-loop
_RESTART_BACKOFF_BASE = 5.0   # first backoff delay (s)
_RESTART_BACKOFF_CAP = 300.0  # max backoff delay (s)

_bot_port = 5819
_bot_bin = None
_bot_config = None

# Persisted "desired state" of the bot, survives manager restarts (needrestart /
# reboot / manual). Only an explicit /api/shutdown writes "stopped"; everything
# else (crash, cgroup-kill on manager restart, reboot) leaves it "running" so the
# bot is brought back. Default "running" (fail toward availability).
_DESIRED_STATE_FILE = ROOT / "logs" / "bot.desired"


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

def _read_desired_state():
    """Last intended bot state ('running'|'stopped'), persisted across manager
    restarts. Default 'running' if absent/unreadable (fail toward availability)."""
    try:
        return "stopped" if _DESIRED_STATE_FILE.read_text().strip() == "stopped" else "running"
    except Exception:
        return "running"


def _write_desired_state(state):
    try:
        _DESIRED_STATE_FILE.parent.mkdir(exist_ok=True)
        _DESIRED_STATE_FILE.write_text(state)
    except Exception as e:
        print(f"[manager] failed to persist desired state: {e}", flush=True)


def _spawn_bot():
    """Spawn the bot subprocess. CALLER MUST HOLD _lock.
    Returns (pid, None) on success or (None, error_str) on failure."""
    global _bot_proc, _bot_status, _bot_exit_code, _bot_start_time, _bot_log_fd
    if not _bot_bin:
        return None, "bot binary not found; use --bot-bin"
    cmd = [_bot_bin]
    if _bot_config:
        cmd.append(_bot_config)
    try:
        log_path = ROOT / "logs" / "bot.log"
        log_path.parent.mkdir(exist_ok=True)
        # stdout/stderr → bot.log so nothing is lost. Close the previous handle
        # (the child holds its own dup) so restarts don't leak fds.
        if _bot_log_fd is not None:
            try:
                _bot_log_fd.close()
            except Exception:
                pass
        _bot_log_fd = open(log_path, "a")
        proc = subprocess.Popen(cmd, stdout=_bot_log_fd, stderr=_bot_log_fd)
    except Exception as e:
        return None, str(e)
    _bot_proc = proc
    _bot_status = "starting"
    _bot_exit_code = None
    _bot_start_time = time.monotonic()
    return proc.pid, None


def _monitor_loop():
    global _bot_status, _bot_exit_code, _bot_proc, _bot_restart_count
    while True:
        with _lock:
            proc = _bot_proc
            status = _bot_status
            auto = _auto_restart
            uptime = time.monotonic() - _bot_start_time

        if proc is not None:
            ret = proc.poll()
            if ret is not None:
                with _lock:
                    _bot_exit_code = ret
                    _bot_status = "crashed" if ret != 0 else "stopped"
                    _bot_proc = None

                if ret == 0 or not auto:
                    # clean exit or intentional shutdown → leave it stopped
                    print(f"[manager] bot exited (code={ret}); no auto-restart", flush=True)
                else:
                    # crash (non-zero / signal) → auto-restart with crash-loop backoff
                    if uptime < _RESTART_MIN_UPTIME:
                        _bot_restart_count += 1
                    else:
                        _bot_restart_count = 1  # healthy run then crashed: reset
                    delay = min(_RESTART_BACKOFF_BASE * (2 ** (_bot_restart_count - 1)),
                                _RESTART_BACKOFF_CAP)
                    print(f"[manager] bot crashed (code={ret}, uptime={uptime:.0f}s); "
                          f"restart #{_bot_restart_count} in {delay:.0f}s", flush=True)
                    time.sleep(delay)
                    with _lock:
                        # skip if a manual start/shutdown intervened during backoff
                        if _bot_proc is None and _auto_restart:
                            pid, err = _spawn_bot()
                            if err:
                                print(f"[manager] auto-restart failed: {err}", flush=True)
                            else:
                                print(f"[manager] bot auto-restarted pid={pid}", flush=True)
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
            html = load_dashboard_html()  # 每次读取，确保 dashboard.h 更新后立即生效
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
        global _auto_restart, _bot_restart_count
        with _lock:
            if _bot_status in ("running", "starting"):
                self._json(409, {"error": "bot already running", "bot_status": _bot_status})
                return
            _auto_restart = True       # a manual start re-enables auto-restart
            _bot_restart_count = 0
            _write_desired_state("running")  # persist intent across manager restarts
            pid, err = _spawn_bot()
            if err:
                self._json(500, {"error": err})
                return
            cmd = [_bot_bin] + ([_bot_config] if _bot_config else [])
        print(f"[manager] bot started  pid={pid}  cmd={cmd}", flush=True)
        self._json(200, {"message": "bot starting", "pid": pid})

    def _shutdown_bot(self):
        global _bot_status, _auto_restart
        with _lock:
            _auto_restart = False      # intentional stop: do not auto-restart
            _write_desired_state("stopped")  # persist intent: stay down across manager restarts
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

    # check if bot already running before we started (e.g. survived a manager-only restart)
    global _bot_status
    already_running = _check_bot_alive()
    if already_running:
        with _lock:
            _bot_status = "running"
        print("[manager] detected bot already running", flush=True)

    # Desired-state reconciliation on startup: if the bot isn't running but its last
    # intent was "running", spawn it. Makes the manager resilient to needrestart /
    # reboot / manager restarts (which cgroup-kill the bot) WITHOUT resurrecting a bot
    # stopped on purpose (an explicit /api/shutdown persisted "stopped").
    if not already_running and _bot_bin:
        desired = _read_desired_state()
        if desired == "running":
            with _lock:
                pid, err = _spawn_bot()
            if err:
                print(f"[manager] startup auto-spawn failed: {err}", flush=True)
            else:
                print(f"[manager] startup auto-spawn (desired=running) pid={pid}", flush=True)
        else:
            print("[manager] startup: bot desired=stopped → not spawning", flush=True)

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
