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
            plain = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", text)
            for line in plain.splitlines():
                if line.startswith(b"source status="):
                    assert len(line) < 80, line
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
