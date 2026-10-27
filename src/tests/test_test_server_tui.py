"""Test-server redraws in a disposable Windows ConPTY or POSIX terminal.

Only synthetic records and a private loopback TCP port are used. The test
never opens a user terminal, real serial device, or production destination.
"""
import errno
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import threading
import time

from test_monitor_battery import Screen
from test_test_stream import SCHEMA, request, response_status


class Terminal:
    """Own a terminal, its bounded output reader, child, and resize lifecycle."""
    def __init__(self, arguments):
        self.errors = []
        if os.name == "nt":
            from physical_weather_conpty import PseudoConsole
            self.child = PseudoConsole(arguments, columns=160, rows=48)
            return
        import select
        self.master, self.slave = os.openpty()
        self.received = bytearray()
        self.changed = threading.Condition()
        self.done = threading.Event()
        self.resize(160, 48)
        environment = dict(os.environ, TERM="xterm-256color")
        self.child = subprocess.Popen(arguments, stdin=subprocess.DEVNULL,
                                      stdout=subprocess.DEVNULL, stderr=self.slave,
                                      env=environment)

        def drain():
            try:
                while not self.done.is_set():
                    if not select.select([self.master], [], [], .025)[0]:
                        continue
                    data = os.read(self.master, 65536)
                    if not data:
                        return
                    with self.changed:
                        self.received.extend(data)
                        assert len(self.received) < 8 * 1024 * 1024, "unbounded terminal output"
                        self.changed.notify_all()
            except OSError as error:
                if error.errno not in (errno.EIO, errno.EBADF):
                    self.errors.append(error)
            except BaseException as error:
                self.errors.append(error)
        self.reader = threading.Thread(target=drain, daemon=True)
        self.reader.start()

    def resize(self, columns, rows):
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes
            from physical_weather_conpty import COORD
            resize = self.child.k.ResizePseudoConsole
            resize.argtypes = [wintypes.HANDLE, COORD]
            resize.restype = ctypes.c_long
            result = resize(self.child.hpc, COORD(columns, rows))
            assert result >= 0, ("ResizePseudoConsole", result)
        else:
            import fcntl
            import termios
            fcntl.ioctl(self.slave, termios.TIOCSWINSZ,
                        struct.pack("HHHH", rows, columns, 0, 0))

    def close(self):
        if os.name == "nt":
            assert not self.child.overflow, "unbounded ConPTY output"
            self.child.close()
            return
        if self.child.poll() is None:
            self.child.terminate()
            try:
                self.child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.child.kill()
                self.child.wait(timeout=3)
        self.done.set()
        self.reader.join(timeout=2)
        os.close(self.master)
        os.close(self.slave)
        assert not self.reader.is_alive() and not self.errors, self.errors


class Dashboard(Screen):
    """Inspect displayed cells: ConPTY can split a changed number into cells."""
    def wait(self, predicate, description, timeout=3):
        deadline = time.monotonic() + timeout
        while True:
            self.refresh()
            if predicate():
                return
            assert self.terminal.child.poll() is None, (description, self.raw()[-4000:])
            assert time.monotonic() < deadline, (description, self.raw()[-4000:])
            time.sleep(.025)

    def elapsed(self):
        match = re.search(r"run_elapsed=([0-9.]+)s", self.line(1))
        return float(match[1]) if match else -1

    def counters(self, count):
        return self.line(2).startswith(f"records={count} statuses=0 keys=0")

    def has_record(self, sequence, rows=48, columns=160):
        prefix = f"#{sequence} HEARTBEAT seq={sequence} ID=1.1"
        return any(prefix in self.line(row)[:columns] for row in range(6, rows))

    def checkpoint(self):
        self.refresh()
        return len(self.raw()), self.full_clears

    def stable_updates(self, checkpoint, rows=48, columns=160):
        offset, clears = checkpoint
        self.refresh()
        output = self.raw()[offset:]
        assert self.full_clears == clears, ("screen cleared during refresh", output)
        assert "\x1b[2K" not in output, ("row blanked before replacement", output)
        assert "[test server]" not in output, ("static header was redrawn", output)
        assert self.line(0).startswith("[test server] LISTENING"), self.line(0)
        # A native ConPTY can emit its own positioning while reflowing. Only
        # inspect stable-size updates; resize output is deliberately excluded.
        for match in re.finditer(r"\x1b\[(\d+);(\d+)H", output):
            assert 1 <= int(match[1]) <= rows, ("drawing below viewport", match[0])
            assert 1 <= int(match[2]) <= columns, ("drawing beyond viewport", match[0])
        if os.name != "nt":
            # Application updates use absolute positioning, never scrolling LF.
            assert "\n" not in output, ("dashboard scrolled", output)


def heartbeat_body(sequence):
    record = {
        "seq": sequence, "source_generation": 1,
        "session": "test-session.0123456789abcdef.1", "source": "synthetic-terminal-test",
        "heartbeat": {"sysid": 1, "compid": 1, "type": 2, "autopilot": 12,
                      "base_mode": 128, "custom_mode": 0, "system_status": 4,
                      "received_monotonic_ms": sequence * 1000},
        "position": None, "coordinate_age_ms": None,
    }
    return json.dumps({"schema": SCHEMA, "stream": "test-stream", "records": [record]},
                      separators=(",", ":")).encode("ascii")


def run(executable):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    terminal = Terminal([str(executable), "--bind", "127.0.0.1", "--port", str(port),
                         "--duration-ms", "12000"])
    screen = Dashboard(terminal)
    try:
        screen.wait(lambda: "LISTENING" in screen.line(0) and screen.counters(0), "initial dashboard")
        screen.wait(lambda: screen.elapsed() >= 0, "run_elapsed label")
        first_elapsed = screen.elapsed()
        initial = screen.checkpoint()
        screen.wait(lambda: screen.elapsed() >= first_elapsed + .5, "idle elapsed refresh")
        assert screen.counters(0), screen.line(2)
        screen.stable_updates(initial)

        def send(sequence, rows=48, columns=160):
            assert response_status(port, request(heartbeat_body(sequence))) == 204
            screen.wait(lambda: screen.counters(sequence) and screen.has_record(sequence, rows, columns),
                        f"request {sequence} displayed")

        for sequence in range(1, 9):
            send(sequence)
        screen.stable_updates(initial)
        assert "Waiting for data" not in "\n".join(screen.line(row) for row in range(5, 16)), (
            "obsolete placeholder was not replaced", screen.raw()[-3000:])

        # Enough records to fill a short viewport expose accidental newline
        # scrolling, row-count mistakes, and stale history after expansion.
        terminal.resize(64, 8)
        time.sleep(.45)
        narrow = screen.checkpoint()
        for sequence in range(9, 25):
            send(sequence, 8, 64)
        screen.stable_updates(narrow, 8, 64)

        terminal.resize(160, 48)
        screen.wait(lambda: screen.has_record(24) and screen.counters(24), "expanded viewport")
        time.sleep(.4)
        expanded = screen.checkpoint()
        send(25)
        screen.stable_updates(expanded)

        examples = Path(__file__).resolve().parents[2] / "docs" / "api" / "examples"
        weather = json.loads((examples / "synthetic-weather-unknown.json").read_text(encoding="utf-8"))
        def post(body):
            assert response_status(port, request(json.dumps(body).encode("utf-8"))) == 204

        post(weather)
        screen.wait(lambda: "records=26 statuses=0 keys=2 IDs=1" in screen.line(2), "unique temperature/humidity keys")
        assert "T 20.00C" in screen.line(4) and "RH 50.00%" in screen.line(4), screen.line(4)
        assert any("T  1.1/12920/0/0" in screen.line(row) for row in range(6, 20))
        post(weather)  # An HTTP retry is another received record, not another key.
        screen.wait(lambda: "records=27 statuses=0 keys=2 IDs=1" in screen.line(2), "retry keeps unique key count")
        weather["records"][0]["source"]["sysid"] = 2
        post(weather)
        screen.wait(lambda: "records=28 statuses=0 keys=4 IDs=2" in screen.line(2), "different device has separate keys")

        status = json.loads((examples / "synthetic-status-disconnected.json").read_text(encoding="utf-8"))
        status["records"][0]["position"] = {"source": "ESTGPS", "state": "LIVE", "fix": 3,
            "lat_deg": 37.123456, "lon_deg": 127.654321, "alt_msl_m": 123.4,
            "rx_age_ms": "100", "stale_after_ms": 600}
        post(status)
        screen.wait(lambda: "records=28 statuses=1 keys=4 IDs=3" in screen.line(2), "status keeps sample keys unchanged")
        assert "link=DOWN" in screen.line(3) and "T 25.00C" in screen.line(4), screen.line(3)
        assert "37.123456 127.654321 123.4m" in screen.line(5), screen.line(5)
        screen.wait(lambda: re.search(r"age=([1-9][0-9]*\.[0-9])s", screen.line(5)), "last coordinate ages between reports")
        assert "STALE" not in screen.line(5), "waiting for next status invented upstream GPS outage"
        assert any("#29 STATUS seq=7 ID=1.1" in screen.line(row) for row in range(6, 30))
        status["records"][0]["position"]["state"] = "INVALID"
        post(status)
        screen.wait(lambda: "INVALID" in screen.line(5), "explicit upstream invalid coordinate retained")
        assert not any("variance=" in screen.line(row) or "boot_us=" in screen.line(row)
                       for row in range(48)), "verbose JSON diagnostics leaked into compact view"
        screen.stable_updates(expanded)
        last_elapsed = screen.elapsed()
        screen.wait(lambda: screen.elapsed() >= last_elapsed + .5, "idle refresh after requests")
        screen.stable_updates(expanded)

        assert terminal.child.wait(timeout=10) == 0, screen.raw()[-4000:]
        time.sleep(.1)
        raw = screen.raw()
        assert "\x1b[?25h" in raw, "cursor was not restored on clean shutdown"
        assert "STOPPED" in raw, raw[-2000:]
        print("Test server TUI: compact values/GPS, unique keys/IDs, arrival order, ages, row diffs, resize, clean exit: PASS")
    finally:
        terminal.close()


if __name__ == "__main__":
    assert len(sys.argv) == 2, "usage: test_test_server_tui.py TEST_SERVER_EXE"
    executable = Path(sys.argv[1]).resolve()
    assert executable.is_file(), executable
    run(executable)
