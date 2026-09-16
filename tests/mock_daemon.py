#!/usr/bin/env python3
"""
tests/mock_daemon.py — Mock Headless Daemon for Offline E2E Testing
Simulates sony-headphones-daemon:
- Listens on UNIX domain socket ($XDG_RUNTIME_DIR/sony-headphones.sock)
- Manages headphone state
- Atomically writes state updates to $XDG_STATE_HOME/sony-headphones/status.json
- Handles wire protocol commands:
    status, noise, ambient-level, eq, dsee
- Signal handling:
    SIGTERM / SIGINT: clean shutdown (unlinks socket and status.json)
    SIGUSR1: simulate disconnect (connected: false)
    SIGUSR2: simulate reconnect (connected: true)
"""

import os
import sys
import json
import time
import socket
import select
import signal
import argparse

VALID_MODES = ["anc", "ambient", "off"]
VALID_PRESETS = ["off", "bright", "excited", "mellow", "relaxed", "vocal", "treble", "bass", "speech", "custom"]

class MockDaemon:
    def __init__(self, state_dir=None, runtime_dir=None):
        self.state_dir = state_dir or os.environ.get("XDG_STATE_HOME", os.path.expanduser("~/.local/state"))
        self.runtime_dir = runtime_dir or os.environ.get("XDG_RUNTIME_DIR", f"/tmp/run-{os.getuid()}")
        
        self.sony_state_dir = os.path.join(self.state_dir, "sony-headphones")
        self.status_file = os.path.join(self.sony_state_dir, "status.json")
        self.socket_path = os.path.join(self.runtime_dir, "sony-headphones.sock")
        
        self.running = True
        self.server_sock = None
        
        self.state = {
            "schema_version": 1,
            "connected": True,
            "device_name": "WH-1000XM3",
            "battery_level": 85,
            "battery_charging": False,
            "noise_mode": "anc",
            "ambient_sound_level": 0,
            "eq_preset": "off",
            "eq_custom_bands": [0, 0, 0, 0, 0],
            "clear_bass": 0,
            "dsee_extreme": True,
            "codec": "LDAC",
            "last_updated": int(time.time())
        }

    def setup_directories(self):
        os.makedirs(self.sony_state_dir, mode=0o700, exist_ok=True)
        os.makedirs(self.runtime_dir, mode=0o700, exist_ok=True)

    def write_status(self):
        self.state["last_updated"] = int(time.time())
        tmp_file = f"{self.status_file}.tmp.{os.getpid()}"
        payload = self.state if self.state.get("connected", True) else {
            "schema_version": 1,
            "connected": False
        }
        with open(tmp_file, "w") as f:
            json.dump(payload, f, indent=2)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp_file, 0o600)
        os.replace(tmp_file, self.status_file)

    def handle_command(self, cmd_line):
        line = cmd_line.strip()
        if not line:
            return "ERR empty command\n"
        parts = line.split()
        verb = parts[0].lower()

        if verb == "status":
            payload = self.state if self.state.get("connected", True) else {
                "schema_version": 1,
                "connected": False
            }
            return json.dumps(payload) + "\n"

        if verb == "noise":
            if len(parts) < 2:
                return "ERR missing mode (expected anc|ambient|off)\n"
            mode = parts[1].lower()
            if mode not in VALID_MODES:
                return f"ERR invalid mode \x27{mode}\x27\n"
            self.state["noise_mode"] = mode
            self.write_status()
            return "OK\n"

        if verb == "ambient-level":
            if len(parts) < 2:
                return "ERR missing level (0-20)\n"
            try:
                level = int(parts[1])
                if not (0 <= level <= 20):
                    return "ERR ambient level out of range [0-20]\n"
                self.state["ambient_sound_level"] = level
                self.write_status()
                return "OK\n"
            except ValueError:
                return f"ERR invalid level \x27{parts[1]}\x27\n"

        if verb == "eq":
            if len(parts) < 2:
                return "ERR missing eq preset\n"
            preset = parts[1].lower()
            if preset == "custom":
                if len(parts) < 8:
                    return "ERR custom eq requires 5 bands and clear bass (6 integers [-10, 10])\n"
                try:
                    bands = [int(p) for p in parts[2:7]]
                    cb = int(parts[7])
                    for b in bands:
                        if not (-10 <= b <= 10):
                            return "ERR custom eq band out of range [-10, 10]\n"
                    if not (-10 <= cb <= 10):
                        return "ERR clear bass out of range [-10, 10]\n"
                    self.state["eq_preset"] = "custom"
                    self.state["eq_custom_bands"] = bands
                    self.state["clear_bass"] = cb
                    self.write_status()
                    return "OK\n"
                except ValueError:
                    return "ERR invalid custom eq arguments\n"
            else:
                if preset not in VALID_PRESETS:
                    return f"ERR unknown eq preset \x27{preset}\x27\n"
                self.state["eq_preset"] = preset
                self.write_status()
                return "OK\n"

        if verb == "dsee":
            if len(parts) < 2 or parts[1].lower() not in ["on", "off"]:
                return "ERR expected on|off\n"
            self.state["dsee_extreme"] = parts[1].lower() == "on"
            self.write_status()
            return "OK\n"

        # Internal test helper verbs
        if verb == "_set_battery":
            if len(parts) >= 2:
                self.state["battery_level"] = int(parts[1])
            if len(parts) >= 3:
                self.state["battery_charging"] = parts[2].lower() in ["true", "1", "yes"]
            self.write_status()
            return "OK\n"

        if verb == "_disconnect":
            self.state["connected"] = False
            self.write_status()
            return "OK\n"

        if verb == "_reconnect":
            self.state["connected"] = True
            self.write_status()
            return "OK\n"

        return f"ERR unknown command \x27{verb}\x27\n"

    def run(self):
        self.setup_directories()

        if os.path.exists(self.socket_path):
            try:
                os.unlink(self.socket_path)
            except OSError:
                pass

        self.server_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server_sock.bind(self.socket_path)
        os.chmod(self.socket_path, 0o700)
        self.server_sock.listen(10)
        self.server_sock.setblocking(False)

        # Initial status write
        self.write_status()

        def sig_term_handler(_signum, _frame):
            self.running = False

        def sig_usr1_handler(_signum, _frame):
            # Disconnect simulation
            self.state["connected"] = False
            self.write_status()

        def sig_usr2_handler(_signum, _frame):
            # Reconnect simulation
            self.state["connected"] = True
            self.write_status()

        signal.signal(signal.SIGTERM, sig_term_handler)
        signal.signal(signal.SIGINT, sig_term_handler)
        signal.signal(signal.SIGUSR1, sig_usr1_handler)
        signal.signal(signal.SIGUSR2, sig_usr2_handler)

        sys.stdout.write(f"[MOCK_DAEMON] Ready PID={os.getpid()}\n")
        sys.stdout.flush()

        inputs = [self.server_sock]
        clients = {}

        while self.running:
            try:
                readable, _, _ = select.select(inputs, [], [], 0.5)
            except (select.error, InterruptedError):
                continue

            for s in readable:
                if s is self.server_sock:
                    try:
                        conn, _ = self.server_sock.accept()
                        conn.setblocking(False)
                        inputs.append(conn)
                        clients[conn] = b""
                    except OSError:
                        pass
                else:
                    try:
                        data = s.recv(1024)
                        if data:
                            clients[s] += data
                            if b"\n" in clients[s]:
                                line, _, rest = clients[s].partition(b"\n")
                                clients[s] = rest
                                cmd_str = line.decode("utf-8", errors="replace")
                                response = self.handle_command(cmd_str)
                                s.sendall(response.encode("utf-8"))
                        else:
                            inputs.remove(s)
                            if s in clients:
                                del clients[s]
                            s.close()
                    except OSError:
                        inputs.remove(s)
                        if s in clients:
                            del clients[s]
                        s.close()

        # Shutdown cleanup
        try:
            if self.server_sock:
                self.server_sock.close()
            if os.path.exists(self.socket_path):
                os.unlink(self.socket_path)
            if os.path.exists(self.status_file):
                os.unlink(self.status_file)
        except OSError:
            pass
        sys.stdout.write("[MOCK_DAEMON] Shutdown cleanly\n")
        sys.stdout.flush()

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--state-dir", default=None)
    parser.add_argument("--runtime-dir", default=None)
    args = parser.parse_args()
    daemon = MockDaemon(state_dir=args.state_dir, runtime_dir=args.runtime_dir)
    daemon.run()
