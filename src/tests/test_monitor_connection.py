"""POSIX production TUI connection colours on a test-owned UDP port and PTY.

No user terminal, GUI, production configuration, or real MAVLink stream is used.
Run: python3 src/tests/test_monitor_connection.py PATH_TO_INTERACTIVE
"""
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

from test_mavlink_router import Wire


def assert_source_width(text, rows=24, columns=80):
    """Reconstruct cursor-addressed rows; delta frames deliberately omit LF."""
    screen = [[] for _ in range(rows)]
    row = column = checked = 0
    control = re.compile(rb"\x1b\[([0-9;?]*)([A-Za-z])")

    def inspect(line):
        nonlocal checked
        value = bytes(line).rstrip(b" ")
        if value.startswith(b"source status="):
            assert len(value) < columns, value
            checked += 1

    index = 0
    while index < len(text):
        if text[index] == 27:
            match = control.match(text, index)
            assert match is not None, text[index:index + 32]
            parameters, command = match.groups()
            index = match.end()
            if parameters.startswith(b"?") or command == b"m":
                continue
            values = [int(value or b"0") for value in parameters.split(b";")]
            inspect(screen[row])
            if command in (b"H", b"f"):
                row = max(1, values[0]) - 1
                column = max(1, values[1] if len(values) > 1 else 1) - 1
                assert 0 <= row < rows and 0 <= column < columns
            elif command == b"K":
                if values[0] == 2:
                    screen[row] = []
                else:
                    assert values[0] == 0
                    del screen[row][column:]
            elif command == b"J":
                if values[0] == 2:
                    screen = [[] for _ in range(rows)]
                else:
                    assert values[0] == 0
                    del screen[row][column:]
                    for below in range(row + 1, rows):
                        screen[below] = []
            else:
                raise AssertionError((parameters, command))
        elif text[index] == 13:
            inspect(screen[row])
            column = 0
            index += 1
        elif text[index] == 10:
            inspect(screen[row])
            row += 1
            if row == rows:
                screen.pop(0)
                screen.append([])
                row -= 1
            index += 1
        else:
            while len(screen[row]) <= column:
                screen[row].append(32)
            screen[row][column] = text[index]
            column += 1
            index += 1
    for line in screen:
        inspect(line)
    assert checked, "No source row was reconstructed"


def main():
    if os.name != "posix":
        print("SKIP connection-colour PTY check: requires a POSIX PTY")
        return
    import errno
    import fcntl
    import select
    import struct
    import termios

    executable = Path(sys.argv[1]).resolve()
    master, slave = os.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    captured = bytearray()
    changed = threading.Condition()
    done = threading.Event()
    child = None

    def drain():
        while not done.is_set():
            try:
                if select.select([master], [], [], .05)[0]:
                    data = os.read(master, 65536)
                    if not data:
                        break
                    with changed:
                        captured.extend(data)
                        changed.notify_all()
            except OSError as error:
                if error.errno in (errno.EIO, errno.EBADF):
                    break
                raise

    def wait_for(marker, after=0, timeout=4):
        until = time.monotonic() + timeout
        with changed:
            while marker not in captured[after:]:
                assert child.poll() is None, (child.returncode, bytes(captured[-4000:]))
                remaining = until - time.monotonic()
                assert remaining > 0, (marker, bytes(captured[-4000:]))
                changed.wait(min(remaining, .05))
            return len(captured)

    reader = threading.Thread(target=drain, daemon=True)
    try:
        with tempfile.TemporaryDirectory(prefix="router-colour-test-") as temporary:
            # Reserve/release only a disposable local port; the production
            # receiver needs exclusive ownership, so send from a second socket.
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserve:
                reserve.bind(("127.0.0.1", 0))
                port = reserve.getsockname()[1]
            config = Path(temporary) / "router.ini"
            settings = {
                "url": "-", "stream": "colour-test", "session": "colour-test",
                "input_mode": "mavlink", "serial_port": "none", "auto_scan": 0,
                "udp_bind": "127.0.0.1", "udp_enabled": 1, "udp_port": port,
                "refresh_ms": 250, "probe_ms": 5000, "link_timeout_ms": 700,
                "reconnect_ms": 200, "duration_ms": 6000,
            }
            config.write_text("".join(f"{key}={value}\n" for key, value in settings.items()), encoding="utf-8")
            env = os.environ.copy()
            env["TERM"] = "xterm-256color"
            child = subprocess.Popen([str(executable), "--config", str(config), "--interactive"],
                                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                     stderr=slave, env=env)
            reader.start()
            wait_for(b"source status=\x1b[33mPROBING\x1b[0m")
            wire = Wire()
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
                source.sendto(b"not a MAVLink frame", ("127.0.0.1", port))
                time.sleep(.35)
                with changed:
                    assert b"\x1b[32m" not in captured, bytes(captured)
                for index in range(8):
                    source.sendto(wire.navigation(1000 + index * 50, index), ("127.0.0.1", port))
                    time.sleep(.05)
                wait_for(b"source status=\x1b[33mWAIT_HEARTBEAT\x1b[0m")
                with changed:
                    assert b"\x1b[32m" not in captured, bytes(captured)
                for _ in range(8):
                    source.sendto(wire.heartbeat(), ("127.0.0.1", port))
                    time.sleep(.05)
                mark = wait_for(b"source status=\x1b[32mLOCKED\x1b[0m")
                # Automatic recovery can reopen the input between dashboard
                # snapshots, so DISCONNECTED itself need not be visible. The
                # important contract is that the stale green state is cleared.
                mark = wait_for(b"source status=\x1b[33mPROBING\x1b[0m", mark)
                time.sleep(.4)
                with changed:
                    assert b"\x1b[32m" not in captured[mark:], bytes(captured[mark:])
            child.wait(timeout=8)
            assert child.returncode == 0, child.returncode
            time.sleep(.1)
            with changed:
                text = bytes(captured)
            assert b"\x1b[0m\x1b[?25h" in text, "exit must restore colour and cursor"
            assert_source_width(text)
            # The same interactive program must emit plain diagnostics when its
            # stderr is a pipe, even after it has rendered terminal colours.
            settings["duration_ms"] = 1200
            config.write_text("".join(f"{key}={value}\n" for key, value in settings.items()), encoding="utf-8")
            redirected = subprocess.run([str(executable), "--config", str(config), "--interactive"],
                                        stdin=subprocess.DEVNULL, capture_output=True, timeout=5)
            assert redirected.returncode == 0, redirected.stderr
            assert b"source status=" in redirected.stderr, redirected.stderr
            assert b"\x1b" not in redirected.stderr, redirected.stderr
        print("PASS connection colours: junk/unidentified yellow, live identified green, timeout yellow, clipped width, reset, plain logs")
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            child.wait(timeout=3)
        done.set()
        if reader.is_alive():
            reader.join(timeout=1)
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()
