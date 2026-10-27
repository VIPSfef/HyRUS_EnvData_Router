"""In-process TUI restart through a disposable POSIX PTY or Windows ConPTY.

Only generated UDP packets, temporary configuration files, and local HTTP are
used. No user terminal, real serial device, or production destination is opened.
"""
import errno
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from test_mavlink_router import Wire


def spare_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as handle:
        handle.bind(("127.0.0.1", 0))
        return handle.getsockname()[1]


def bound(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as handle:
        try:
            handle.bind(("127.0.0.1", port))
        except OSError as error:
            assert error.errno == errno.EADDRINUSE or (os.name == "nt" and error.errno == errno.EACCES), error
            return True
        return False


def wait_for(predicate, description, timeout=5):
    until = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < until, description
        time.sleep(.025)


def write_config(path, port, url="-"):
    settings = {
        "url": url, "stream": "restart-test", "session": "restart-test",
        "input_mode": "mavlink", "serial_port": "none", "auto_scan": 0,
        "udp_bind": "127.0.0.1", "udp_enabled": 1, "udp_port": port,
        "refresh_ms": 250, "probe_ms": 1000, "link_timeout_ms": 1500,
        "connect_timeout_ms": 500, "io_timeout_ms": 500,
        "reconnect_ms": 100, "send_interval_ms": 1000, "duration_ms": 0,
        "origin_auto": 0, "origin_lat": 37, "origin_lon": 127, "origin_alt": 100,
    }
    path.write_text("".join(f"{key}={value}\n" for key, value in settings.items()), encoding="utf-8")


class Terminal:
    def __init__(self, executable, config):
        import fcntl
        import select
        import termios
        self.master, self.slave = os.openpty()
        self.received = bytearray()
        self.changed = threading.Condition()
        self.done = threading.Event()
        self.original = termios.tcgetattr(self.slave)
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 48, 160, 0, 0))

        def control():
            os.setsid()
            fcntl.ioctl(self.slave, termios.TIOCSCTTY, 0)
            os.tcsetpgrp(self.slave, os.getpgrp())

        env = os.environ.copy()
        env["TERM"] = "xterm-256color"
        self.child = subprocess.Popen([str(executable), "--config", str(config), "--interactive"],
                                      stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                      stderr=self.slave, pass_fds=(self.slave,),
                                      preexec_fn=control, env=env)
        self.pid = self.child.pid

        def drain():
            while not self.done.is_set():
                try:
                    if select.select([self.master], [], [], .05)[0]:
                        data = os.read(self.master, 65536)
                        if not data:
                            break
                        with self.changed:
                            self.received.extend(data)
                            assert len(self.received) < 8 * 1024 * 1024, "unbounded test terminal output"
                            self.changed.notify_all()
                except OSError as error:
                    if error.errno in (errno.EIO, errno.EBADF):
                        break
                    raise
        self.reader = threading.Thread(target=drain, daemon=True)
        self.reader.start()
        try:
            self.await_text("e: settings")
        except BaseException:
            self.close()
            raise

    def mark(self):
        with self.changed:
            return len(self.received)

    def text(self, after=0):
        return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", self.received[after:].decode("utf-8", "replace"))

    def await_text(self, marker, after=0, timeout=5):
        until = time.monotonic() + timeout
        while marker not in self.text(after):
            assert self.child.poll() is None, (self.child.poll(), marker, self.text()[-6000:])
            assert time.monotonic() < until, (marker, self.text()[-6000:])
            time.sleep(.025)
        return self.mark()

    def keys(self, value):
        value = value.encode("ascii") if isinstance(value, str) else value
        while value:
            written = os.write(self.master, value)
            value = value[written:]

    def open_editor(self):
        marker = self.mark(); self.keys("e")
        self.await_text("[settings]", marker)

    def edit(self, index, name, value, valid=True):
        marker = self.mark(); self.keys(f"{index}\r")
        self.await_text(f"Edit {name}", marker)
        marker = self.mark(); self.keys(f"\x15{value}\r")
        self.await_text("Draft validated" if valid else "Invalid setting", marker)

    def same_process(self):
        assert self.child.pid == self.pid and self.child.poll() is None
        os.kill(self.pid, 0)

    def close(self):
        import termios
        if self.child.poll() is None:
            self.child.terminate()
            try:
                self.child.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self.child.kill(); self.child.wait(timeout=3)
        self.done.set(); self.reader.join(timeout=2)
        termios.tcsetattr(self.slave, termios.TCSANOW, self.original)
        os.close(self.master); os.close(self.slave)


class WindowsTerminal(Terminal):
    def __init__(self, executable, config):
        from physical_weather_conpty import PseudoConsole
        self.child = PseudoConsole([str(executable), "--config", str(config), "--interactive"], columns=160, rows=48)
        self.pid = self.child.process.dwProcessId
        try:
            self.await_text("e: settings")
        except BaseException:
            self.close()
            raise

    def mark(self):
        return len(self.child.text())

    def text(self, after=0):
        return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", self.child.text()[after:])

    def keys(self, value):
        self.child.write(value.encode("ascii") if isinstance(value, str) else value)

    def same_process(self):
        assert self.child.process.dwProcessId == self.pid and self.child.poll() is None

    def close(self):
        self.child.close()


def terminal_session(executable, config):
    return (WindowsTerminal if os.name == "nt" else Terminal)(executable, config)


class Source:
    def __init__(self, ports):
        self.ports = ports
        self.sample = threading.Event()
        self.sample_count = 1
        self.done = threading.Event()
        self.errors = []

        def run():
            wire = Wire()
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
                    while not self.done.is_set():
                        payload = wire.heartbeat()
                        if self.sample.is_set():
                            self.sample.clear()
                            # One sample per boot-time window. The last remains
                            # partial while preceding windows enter the ring.
                            for index in range(self.sample_count):
                                boot = 1000 + index * 1000
                                payload += (wire.navigation(boot, 0) + wire.named(boot + 50, "restart", 17 + index) +
                                            wire.navigation(boot + 100, 0) + wire.navigation(boot + 400, 0))
                        for port in self.ports:
                            source.sendto(payload, ("127.0.0.1", port))
                        self.done.wait(.08)
            except Exception as error:
                self.errors.append(error)
        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()

    def close(self):
        self.done.set(); self.thread.join(timeout=2)
        assert not self.errors, self.errors


class RejectingServer:
    def __init__(self):
        self.accept = False
        self.records = []
        self.changed = threading.Condition()
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                body = self.rfile.read(int(self.headers["Content-Length"]))
                with owner.changed:
                    status = 204 if owner.accept else 503
                    owner.records.append((self.path, status, json.loads(body)))
                    owner.changed.notify_all()
                self.send_response(status)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *unused):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.url = f"http://127.0.0.1:{self.server.server_port}"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown(); self.server.server_close(); self.thread.join(timeout=2)


def configuration_restart(executable, folder):
    first, second = spare_port(), spare_port()
    while second == first:
        second = spare_port()
    config = folder / "restart.ini"
    write_config(config, first)
    terminal = terminal_session(executable, config)
    source = Source([first, second])
    try:
        terminal.await_text("LOCKED")
        terminal.open_editor()
        terminal.edit(12, "refresh_ms", 500)
        terminal.edit(32, "udp_port", second)
        marker = terminal.mark(); terminal.keys("s")
        terminal.await_text("SAVED", marker)
        assert f"refresh_ms=500" in config.read_text() and f"udp_port={second}" in config.read_text()
        terminal.keys("q")
        assert bound(first) and not bound(second), "save-only changed the live UDP socket"
        terminal.same_process()
        marker = terminal.mark(); terminal.keys("r")
        terminal.await_text("config restarts=1 refresh=500ms", marker)
        wait_for(lambda: bound(second) and not bound(first), "restart did not transfer UDP socket")
        terminal.same_process()
        terminal.open_editor()
        terminal.edit(12, "refresh_ms", 250)
        terminal.edit(32, "udp_port", first)
        marker = terminal.mark(); terminal.keys("r")
        terminal.await_text("config restarts=2 refresh=250ms", marker)
        wait_for(lambda: bound(first) and not bound(second), "second restart did not release previous port")
        terminal.same_process()
        terminal.open_editor()
        terminal.edit(12, "refresh_ms", 0, valid=False)
        # r inside invalid field input is text, never a restart command.
        marker = terminal.mark(); terminal.keys("r\r")
        terminal.await_text("Invalid setting", marker)
        terminal.keys(b"\x1b"); time.sleep(.12)
        marker = terminal.mark(); terminal.keys("q")
        # Wait for the closed editor to be presented. Matching an older monitor
        # frame lets q + the next e arrive in the same drained input batch, so
        # the unchanged settings heading correctly needs no repaint.
        terminal.await_text("config restarts=2 refresh=250ms", marker)
        assert bound(first) and not bound(second)
        original = config.read_bytes()
        config.unlink(); config.mkdir()
        try:
            terminal.open_editor()
            terminal.edit(12, "refresh_ms", 750)
            marker = terminal.mark(); terminal.keys("r")
            terminal.await_text("Restart failed:", marker)
            terminal.same_process()
            assert bound(first) and not bound(second), "save failure interrupted active receiver"
        finally:
            config.rmdir(); config.write_bytes(original)
        marker = terminal.mark(); terminal.keys("q")
        terminal.await_text("config restarts=2 refresh=250ms", marker)
        print("PASS restart: save-only isolation, two same-PID restarts, port release/rebind, invalid/save-failed preservation")
    finally:
        source.close(); terminal.close()


def failed_drain_preserves_data(executable, folder):
    first, second = spare_port(), spare_port()
    while second == first:
        second = spare_port()
    server = RejectingServer()
    config = folder / "drain.ini"
    write_config(config, first, server.url + "/old")
    terminal = terminal_session(executable, config)
    source = Source([first, second])
    try:
        terminal.await_text("LOCKED")
        source.sample.set()
        wait_for(lambda: bool(server.records), "test sample never reached HTTP server")
        first_payload = server.records[0][2]
        assert first_payload["batches"] and first_payload["batches"][0]["raw_n"] > 0, first_payload
        terminal.open_editor()
        terminal.edit(1, "url", server.url + "/new")
        terminal.edit(32, "udp_port", second)
        attempts_before = len(server.records)
        marker = terminal.mark(); terminal.keys("r")
        terminal.await_text("Restart blocked: pending data retained; previous settings resumed.", marker, timeout=9)
        assert len(server.records) >= attempts_before + 3, "restart abandoned its drain retry budget early"
        terminal.await_text("config restarts=0 refresh=250ms", marker)
        wait_for(lambda: bound(first) and not bound(second), "failed drain did not restore previous receiver")
        terminal.same_process()
        assert all(path == "/old" for path, _, _ in server.records), server.records
        assert f"udp_port={second}" in config.read_text(), "saved draft was unexpectedly overwritten"
        with server.changed:
            server.accept = True
        wait_for(lambda: any(status == 204 for _, status, _ in server.records), "retained batch was not retried after recovery")
        accepted = [payload for path, status, payload in server.records if status == 204 and path == "/old"]
        original_batches = first_payload["batches"]
        assert any(all(batch in payload["batches"] for batch in original_batches) for payload in accepted), (first_payload, accepted)
        # A later retry may now apply the saved draft, after old data has been
        # acknowledged through the old destination.
        marker = terminal.mark(); terminal.keys("r")
        terminal.await_text("config restarts=1 refresh=250ms", marker)
        wait_for(lambda: bound(second) and not bound(first), "acknowledged retry did not apply saved draft")
        terminal.same_process()
        source.sample.set()
        wait_for(lambda: any(path == "/new" and status == 204 for path, status, _ in server.records),
                 "new session did not use the newly applied HTTP destination")
        print("PASS restart drain failure: original batches retained, old endpoint/config resume, ACK recovery then saved draft applies")
    finally:
        source.close(); terminal.close(); server.close()


def backlog_preserves_partial(executable, folder):
    first, second = spare_port(), spare_port()
    while second == first:
        second = spare_port()
    server = RejectingServer()
    config = folder / "backlog.ini"
    write_config(config, first, server.url + "/old")
    terminal = terminal_session(executable, config)
    source = Source([first, second])
    sample_count = 9  # Exceed the previous eight-record limit, including the partial.
    expected_values = set(range(17, 17 + sample_count))

    def acknowledged_values():
        return {row[payload["field_order"].index("value")] for path, status, payload in server.records if path == "/old" and status == 204
                for batch in payload.get("batches", []) for row in batch["raw"]}

    def assert_no_losses(after):
        # Compact terminal frames intentionally omit a zero-loss row; any
        # visible row must remain zero. HTTP envelopes below provide the
        # explicit counters even when the compact row is absent.
        visible = re.findall(r"loss gaps=(\d+) raw=(\d+) queue_windows=(\d+) queue_samples=(\d+)", terminal.text(after))
        assert all(not any(map(int, values)) for values in visible), visible

    try:
        terminal.await_text("LOCKED")
        # Prepare the draft before creating pressure, so editor typing cannot
        # consume the one-second partial-window seal interval.
        terminal.open_editor()
        terminal.edit(32, "udp_port", second)
        marker = terminal.mark(); terminal.keys("s")
        terminal.await_text("SAVED", marker)
        marker = terminal.mark(); terminal.keys("q")
        terminal.await_text("[monitor]", marker)
        source.sample_count = sample_count
        marker = terminal.mark(); source.sample.set()
        wait_for(lambda: any(int(count) >= 8 for count in re.findall(r"queue records=(\d+)", terminal.text(marker))),
                 "RAM backlog did not retain at least eight records")
        attempts_before = len(server.records)
        marker = terminal.mark(); terminal.keys("r")
        terminal.await_text("Restart blocked: pending data retained; previous settings resumed.", marker, timeout=9)
        assert len(server.records) >= attempts_before + 3, "backlog pre-drain stopped before its retry budget"
        terminal.same_process()
        assert bound(first) and not bound(second), "blocked restart changed the source socket"
        assert all(path == "/old" for path, _, _ in server.records), server.records
        with server.changed:
            server.accept = True
        wait_for(lambda: acknowledged_values() == expected_values,
                 "queued or partial sample was discarded during blocked restart", timeout=8)
        # Repaint unchanged zero counters after input resumes. During the
        # blocked restart the editor deliberately ignores input.
        terminal.open_editor()
        marker = terminal.mark(); terminal.keys("q")
        assert_no_losses(marker)
        terminal.await_text("config restarts=0 refresh=250ms", marker)
        assert all(path == "/old" for path, _, _ in server.records), server.records
        assert all(int(payload["loss"][key]) == 0 for _, _, payload in server.records
                   for key in ("queue_windows", "queue_samples", "raw_samples")), server.records
        print("PASS restart RAM backlog: all 9 queued/partial observations survive failed drain and are ACKed on old endpoint with zero losses")
    finally:
        source.close(); terminal.close(); server.close()


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="monitor-restart-") as temporary:
        folder = Path(temporary)
        configuration_restart(executable, folder)
        failed_drain_preserves_data(executable, folder)
        backlog_preserves_partial(executable, folder)


if __name__ == "__main__":
    main()
