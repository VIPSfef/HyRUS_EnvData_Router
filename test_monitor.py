"""Monitor/config integration tests with generated input and loopback HTTP only.

No external service, GUI focus, or additional packages. POSIX uses a test-owned PTY.
Run: python test_monitor.py [telemetry_send.exe]
         [--monitor-executable telemetry_monitor.exe]
Interactive console drawing/editor keys require a separate console check.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parent
UTC = 1_800_000_000_000_000
CLOCK = f"@clock,0,{UTC},1000,200\n"
TOKEN = "PRIVATE_BEARER_MONITOR_TEST_849213"
QUERY_SECRET = "PRIVATE_QUERY_MONITOR_TEST_572624"
TOKEN_ENV = "TB_MONITOR_TEST_TOKEN"


def row(index: int, step_us: int = 4000) -> str:
    """250 Hz aggregate; four independent keys each advance at 62.5 Hz."""
    return (f"{1_000_000 + index * step_us},{index // 4},7,sensor.k{index % 4},"
            f"unit,{index * .125},{index * .25},0,3,25,-50,0,5,1000,0.5\n")


def environment() -> dict[str, str]:
    result = os.environ.copy()
    result.pop("TB_TOKEN", None)
    result[TOKEN_ENV] = TOKEN
    return result


def command(executable: Path, url: str, monitor: bool = True) -> list[str]:
    return [str(executable), url, "monitor-test", "monitor-session", "37", "127", "100"] + (
        ["--monitor"] if monitor else [])


def write_config(path: Path, url: str, **overrides: object) -> None:
    settings = {
        "url": url, "stream": "monitor-test", "session": "monitor-session",
        "token_env": TOKEN_ENV, "origin_lat": 37, "origin_lon": 127, "origin_alt": 100,
        "send_interval_ms": 1000, "connect_timeout_ms": 500, "io_timeout_ms": 5000,
        "max_windows": 4, "refresh_ms": 250, "idle_warn_ms": 3000,
        "ack_warn_ms": 500, "summary": 0,
    }
    settings.update(overrides)
    path.write_text("".join(f"{key}={value}\n" for key, value in settings.items()),
                    encoding="utf-8")


def parse_frames(text: str) -> list[dict[str, str]]:
    frames: list[dict[str, str]] = []
    for line in text.splitlines():
        if line.startswith("[monitor] "):
            frames.append({"header": line})
        elif frames and line:
            prefix = line.split(maxsplit=1)[0]
            if prefix in {"input", "loss", "queue", "network", "HTTP", "health", "auth",
                          "clock", "alignment", "keys", "config", "endpoint"}:
                frames[-1][prefix] = line
    return frames


def number(frame: dict[str, str], section: str, name: str) -> int:
    match = re.search(r"(?:^|[ \t])" + re.escape(name) + r"=(\d+)", frame[section])
    assert match, (section, name, frame)
    return int(match[1])


def assert_private(text: str) -> None:
    assert TOKEN not in text, "The bearer credential appeared in diagnostic output"
    assert QUERY_SECRET not in text, "The URL query secret appeared in diagnostic output"
    assert "\x1b" not in text, "Redirected stderr contained terminal control escapes"


def assert_final(text: str, accepted: int, dry: bool) -> dict[str, str]:
    assert_private(text)
    frames = parse_frames(text)
    assert frames, text
    last = frames[-1]
    assert number(last, "input", "accepted") == accepted, text
    assert number(last, "input", "rejected") == 0, text
    assert number(last, "loss", "gaps") == 0, text
    assert number(last, "loss", "raw") == 0, text
    assert number(last, "loss", "queue_windows") == 0, text
    assert number(last, "queue", "windows") == 0, text
    assert number(last, "queue", "inflight") == 0, text
    if dry:
        assert number(last, "network", "acks") == 0, text
        assert number(last, "network", "dry_releases") > 0, text
    else:
        assert number(last, "network", "acks") > 0, text
    assert "pending=0" in text, text
    return last


def dry_stdout_and_counters(executable: Path) -> None:
    count = 800
    text = CLOCK + "".join(row(i) for i in range(count))
    process = subprocess.run(command(executable, "-"), input=text, capture_output=True,
                             text=True, env=environment(), timeout=12)
    assert process.returncode == 0, process.stderr
    final = assert_final(process.stderr, count, dry=True)
    assert number(final, "input", "lines") == count + 1
    assert number(final, "input", "bytes") == len(text.encode("utf-8"))
    payloads = [json.loads(line) for line in process.stdout.splitlines()]
    raw = [datum for payload in payloads for batch in payload["batches"]
           for datum in batch["raw"]]
    assert len(raw) == count
    raw.sort(key=lambda datum: int(datum[0]))
    for i, datum in enumerate(raw):
        assert int(datum[0]) == 1_000_000 + i * 4000
        assert datum[1] == i // 4 and datum[3] == f"sensor.k{i % 4}"
    assert "[monitor]" not in process.stdout and "\x1b" not in process.stdout
    print("PASS redirected monitor: 800 observations, exact counters, JSON stdout untouched")


class Capture:
    def __init__(self, pipe):
        self.lines: list[tuple[float, str]] = []
        self.lock = threading.Lock()

        def read():
            for line in pipe:
                with self.lock:
                    self.lines.append((time.monotonic(), line))

        self.thread = threading.Thread(target=read, daemon=True)
        self.thread.start()

    def snapshot(self):
        with self.lock:
            return list(self.lines)

    def finish(self) -> str:
        self.thread.join(timeout=3)
        assert not self.thread.is_alive(), "Monitor stderr reader did not finish"
        return "".join(line for _, line in self.snapshot())


class LocalServer:
    def __init__(self, statuses=(202, 204), first_delay=2.4):
        self.records: list[dict] = []
        self.errors: list[str] = []
        self.condition = threading.Condition()
        self.connections = 0
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                try:
                    body = self.rfile.read(int(self.headers["Content-Length"]))
                    payload = json.loads(body)
                    with owner.condition:
                        index = len(owner.records)
                        record = {"at": time.monotonic(), "replied": None, "payload": payload,
                                  "authorization": self.headers.get("Authorization"),
                                  "status": statuses[index] if index < len(statuses) else 204}
                        owner.records.append(record)
                        owner.condition.notify_all()
                    if index == 0 and first_delay:
                        time.sleep(first_delay)
                    self.send_response(record["status"])
                    self.send_header("Content-Length", "0")
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.flush()
                    with owner.condition:
                        record["replied"] = time.monotonic()
                        owner.condition.notify_all()
                except Exception as error:
                    with owner.condition:
                        owner.errors.append(repr(error))
                        owner.condition.notify_all()

            def log_message(self, *unused):
                pass

        class CountedServer(ThreadingHTTPServer):
            def get_request(self):
                connection = super().get_request()
                with owner.condition:
                    owner.connections += 1
                return connection

        self.http = CountedServer(("127.0.0.1", 0), Handler)
        self.http.daemon_threads = True
        self.thread = threading.Thread(target=self.http.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.http.server_port}/monitor?secret={QUERY_SECRET}"

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.http.shutdown()
        self.http.server_close()
        self.thread.join(timeout=3)

    def snapshot(self):
        with self.condition:
            assert not self.errors, self.errors
            return list(self.records)


def batch_id(payload, batch):
    return payload["stream"], payload["session"], batch["window_us"]


def slow_http_progress(executable: Path, folder: Path) -> None:
    with LocalServer() as server:
        config = folder / "설정-monitor.ini"
        write_config(config, server.url)
        child = subprocess.Popen(
            [str(executable), "--config", str(config), "--monitor"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env=environment())
        capture = Capture(child.stderr)
        count = 1375  # 5.5 seconds of paced 250 Hz input; fits every raw window.
        try:
            child.stdin.write(CLOCK)
            child.stdin.flush()
            start = time.monotonic()
            for i in range(count):
                child.stdin.write(row(i))
                child.stdin.flush()
                remaining = start + (i + 1) / 250 - time.monotonic()
                if remaining > 0:
                    time.sleep(remaining)
            child.stdin.close()
            child.stdin = None
            child.wait(timeout=18)
            text = capture.finish()
            out = child.stdout.read()
            assert child.returncode == 0, text
            assert not out, "HTTP mode wrote telemetry or monitoring to stdout"
            final = assert_final(text, count, dry=False)
            records = server.snapshot()
            assert len(records) >= 2 and records[0]["status"] == 202
            assert records[-1]["status"] == 204
            assert all(record["authorization"] == f"Bearer {TOKEN}" for record in records)
            assert number(final, "network", "attempts") == len(records), text
            assert number(final, "network", "failures") == 1, text
            assert number(final, "network", "acks") == len(records) - 1, text
            for a, b in zip(records, records[1:]):
                assert b["at"] - a["at"] >= .95
            # A 202 retains the original immutable windows for the next request.
            first = {batch_id(records[0]["payload"], b): b
                     for b in records[0]["payload"]["batches"]}
            second = {batch_id(records[1]["payload"], b): b
                      for b in records[1]["payload"]["batches"]}
            assert first and all(key in second and second[key] == batch
                                 for key, batch in first.items())
            unique = {}
            for record in records:
                for batch in record["payload"]["batches"]:
                    identity = batch_id(record["payload"], batch)
                    if identity in unique:
                        assert unique[identity] == batch
                    unique[identity] = batch
            assert sum(batch["raw_n"] for batch in unique.values()) == count
            # Both telemetry receipt and monitor refresh progress during slow HTTP.
            first_record = records[0]
            progress = []
            for at, line in capture.snapshot():
                if (first_record["at"] + .15 < at < first_record["replied"] - .05
                        and line.startswith("input ")):
                    match = re.search(r"\baccepted=(\d+)", line)
                    if match:
                        progress.append(int(match[1]))
            assert len(progress) >= 2 and max(progress) - min(progress) >= 100, (
                "Monitor/receiver stopped during slow HTTP", progress, text)
            assert any("inflight=" in line and re.search(r"\binflight=[1-9]", line)
                       for _, line in capture.snapshot()), text
        finally:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=3)
            if capture.thread.is_alive():
                capture.thread.join(timeout=3)
    print("PASS paced250Hz + slow HTTP: monitor/receipt progress; 202 retained; 204 clears queue")
    print("PASS UTF-8 config path/custom token environment; bearer/query secrets remain hidden")


def invalid_config_before_network(executable: Path, folder: Path) -> None:
    with LocalServer(statuses=(204,), first_delay=0) as server:
        config = folder / "invalid.ini"
        write_config(config, server.url)
        with config.open("a", encoding="utf-8") as out:
            out.write("url=http://invalid.example/PRIVATE_DUPLICATE_VALUE\n")
        child = subprocess.Popen([str(executable), "--config", str(config), "--monitor"],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True, env=environment())
        try:
            assert child.wait(timeout=3) != 0, "Invalid config was accepted"
            text = child.stderr.read()
            assert_private(text)
            assert "PRIVATE_DUPLICATE_VALUE" not in text
            assert not server.snapshot(), "Invalid config initiated a network request"
            assert server.connections == 0, "Invalid config initiated a TCP connection"
            assert not child.stdout.read()
        finally:
            if child.stdin:
                child.stdin.close()
            if child.poll() is None:
                child.kill()
                child.wait(timeout=3)
    print("PASS invalid config exits before reading idle stdin or contacting the server")


def default_monitor(executable: Path) -> None:
    process = subprocess.run(command(executable, "-", monitor=False), input=CLOCK + row(0),
                             capture_output=True, text=True, env=environment(), timeout=8)
    assert process.returncode == 0, process.stderr
    assert_final(process.stderr, 1, dry=True)
    assert len([json.loads(line) for line in process.stdout.splitlines()]) == 1
    print("PASS telemetry_monitor defaults to monitoring with the existing positional CLI")



def posix_tui_cleanup(executable: Path) -> None:
    """Test-owned controlling PTY: private editor save and Ctrl+C restoration.

    Prepared for POSIX/Linux CI. Windows uses the separate real-console check
    and must not import Unix-only terminal modules.
    """
    if os.name != "posix":
        print("SKIP POSIX PTY signal/terminal restoration check on Windows")
        return

    import errno
    import fcntl
    import select
    import signal
    import struct
    import termios

    master, slave = os.openpty()
    child = None
    reader = None
    stop_reader = threading.Event()
    received = bytearray()
    changed = threading.Condition()

    def drain():
        while not stop_reader.is_set():
            try:
                readable, _, _ = select.select([master], [], [], .05)
                if not readable:
                    continue
                data = os.read(master, 65536)
                if not data:
                    break
                with changed:
                    received.extend(data)
                    changed.notify_all()
            except BlockingIOError:
                continue
            except OSError as error:
                if error.errno in (errno.EIO, errno.EBADF):
                    break
                raise

    def output() -> bytes:
        with changed:
            return bytes(received)

    def await_output(marker: bytes, after: int = 0, timeout: float = 5) -> bytes:
        deadline = time.monotonic() + timeout
        with changed:
            while marker not in received[after:]:
                if child is not None and child.poll() is not None:
                    raise AssertionError(
                        f"PTY child exited before {marker!r}: {bytes(received[-4000:])!r}")
                remaining = deadline - time.monotonic()
                assert remaining > 0, (
                    f"PTY output did not contain {marker!r}", bytes(received[-4000:]))
                changed.wait(min(.1, remaining))
            return bytes(received)

    def send_keys(keys: bytes) -> None:
        offset = 0
        deadline = time.monotonic() + 3
        while offset < len(keys):
            try:
                offset += os.write(master, keys[offset:])
            except BlockingIOError:
                assert time.monotonic() < deadline, "PTY keyboard write timed out"
                select.select([], [master], [], .05)

    def saved_interval(path: Path) -> str | None:
        try:
            for line in path.read_text(encoding="utf-8-sig").splitlines():
                key, separator, value = line.partition("=")
                if separator and key.strip() == "send_interval_ms":
                    return value.strip()
        except (FileNotFoundError, UnicodeError):
            return None
        return None

    try:
        # Establish a meaningful canonical/echo/signals baseline on this new PTY.
        baseline = termios.tcgetattr(slave)
        baseline[3] |= termios.ICANON | termios.ECHO | termios.ISIG
        baseline[6][termios.VINTR] = b"\x03"
        termios.tcsetattr(slave, termios.TCSANOW, baseline)
        original = termios.tcgetattr(slave)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        os.set_blocking(master, False)

        def controlling_terminal():
            # This child alone becomes session/group leader; no user terminal.
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
            os.tcsetpgrp(slave, os.getpgrp())

        with tempfile.TemporaryDirectory(prefix="telemetry-pty-test-") as temporary:
            config = Path(temporary) / "tui-test.ini"
            write_config(config, "-", refresh_ms=250, send_interval_ms=1000)
            env = environment()
            env["TERM"] = "xterm-256color"
            child = subprocess.Popen(
                [str(executable), "--config", str(config), "--monitor"],
                stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=slave,
                pass_fds=(slave,), preexec_fn=controlling_terminal,
                env=env, text=True)
            # Keep the parent's slave descriptor for tcgetattr after child exit.
            reader = threading.Thread(target=drain, daemon=True)
            reader.start()
            await_output(b"e: settings")
            raw = termios.tcgetattr(slave)
            assert not raw[3] & termios.ICANON, "Monitor did not enter noncanonical mode"
            assert not raw[3] & termios.ECHO, "Monitor did not disable keyboard echo"
            child.stdin.write(CLOCK + "".join(row(i) for i in range(12)))
            child.stdin.flush()
            await_output(b"accepted=12")

            mark = len(output())
            send_keys(b"e")
            await_output(b"[settings]", mark)
            mark = len(output())
            send_keys(b"8\r")
            await_output(b"Edit send_interval_ms", mark)
            mark = len(output())
            send_keys(b"\x152000\r")  # Ctrl+U, replacement, Enter validation.
            await_output(b"Draft validated", mark)
            mark = len(output())
            send_keys(b"s")
            await_output(b"SAVED", mark)
            deadline = time.monotonic() + 3
            while saved_interval(config) != "2000":
                assert time.monotonic() < deadline, "Editor did not atomically save interval=2000"
                time.sleep(.025)
            mark = len(output())
            send_keys(b"q")
            await_output(b"ACTIVE interval=1000ms", mark)
            # CSV remains open: exit must come from the PTY's foreground SIGINT.
            assert child.poll() is None
            send_keys(b"\x03")
            code = child.wait(timeout=5)
            assert code in (-signal.SIGINT, 128 + signal.SIGINT), code
            restored = termios.tcgetattr(slave)
            assert restored[3] & termios.ICANON == original[3] & termios.ICANON
            assert restored[3] & termios.ECHO == original[3] & termios.ECHO
            assert restored == original, (
                "SIGINT did not restore all saved termios attributes", original, restored)
            # The reader drains continuously; allow it to consume the final write.
            deadline = time.monotonic() + 2
            while True:
                captured = output()
                hidden = captured.rfind(b"\x1b[?25l")
                shown = captured.rfind(b"\x1b[?25h")
                if hidden < 0 or shown > hidden:
                    break
                assert time.monotonic() < deadline, "SIGINT left the terminal cursor hidden"
                time.sleep(.025)
            assert TOKEN.encode() not in captured and QUERY_SECRET.encode() not in captured
        print("PASS POSIX controlling PTY: private save applies on restart; Ctrl+C restores termios/cursor")
    finally:
        if child is not None:
            if child.poll() is None:
                # setsid made only this subprocess the leader of its own group.
                # Verify that identity before ever signalling a process group.
                try:
                    if os.getpgid(child.pid) == child.pid and os.getsid(child.pid) == child.pid:
                        os.killpg(child.pid, signal.SIGKILL)
                    else:
                        child.kill()
                except ProcessLookupError:
                    pass
                child.wait(timeout=3)
            if child.stdin is not None:
                child.stdin.close()
        stop_reader.set()
        if reader is not None:
            reader.join(timeout=1)
        # Restore our test terminal even when an assertion deliberately fails.
        if "original" in locals():
            termios.tcsetattr(slave, termios.TCSANOW, original)
        os.close(master)
        os.close(slave)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", nargs="?", type=Path,
                        default=ROOT / ("telemetry_send.exe" if os.name == "nt" else "telemetry_send"))
    parser.add_argument("--monitor-executable", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve()
    assert executable.is_file(), executable
    dry_stdout_and_counters(executable)
    with tempfile.TemporaryDirectory(prefix="telemetry-monitor-test-") as temp:
        folder = Path(temp)
        slow_http_progress(executable, folder)
        invalid_config_before_network(executable, folder)
    monitor = args.monitor_executable
    if monitor is None:
        candidate = ROOT / ("telemetry_monitor.exe" if os.name == "nt" else "telemetry_monitor")
        if candidate.is_file():
            monitor = candidate
    if monitor is not None:
        assert monitor.is_file(), monitor
        default_monitor(monitor.resolve())
    else:
        print("SKIP default-monitor binary check: pass --monitor-executable to enable")
    posix_tui_cleanup(monitor.resolve() if monitor is not None else executable)
    print("ALL MONITOR INTEGRATION CHECKS PASSED")


if __name__ == "__main__":
    main()
