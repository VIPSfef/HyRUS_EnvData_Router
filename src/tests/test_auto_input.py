"""Production-router source race/failover tests; synthetic MAVLink only.

All sockets and PTYs belong to this test. No QGC port or real UART is opened.
"""
import base64
import hashlib
import math
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from test_mavlink_router import ConfirmationServer, UTC_OFFSET, Wire, websocket_frame

ROOT = Path(__file__).resolve().parents[2]
EXE = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build" / (
    "windows-x64/mavlink_telemetry_data_router_headless.exe" if sys.platform == "win32"
    else "linux-x64/mavlink_telemetry_data_router_headless")


class UdpReservation:
    """Hold destination ownership while all producer/server sockets are created.

    Releasing bind(0) before allocating producer sockets can let a producer
    reclaim its own destination, especially in parallel Linux test runs. The
    router receives the reservation and releases it immediately before Popen.
    """
    def __init__(self):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.port = self.socket.getsockname()[1]

    def __enter__(self):
        return self

    def release(self):
        self.socket.close()

    def __exit__(self, *unused):
        self.release()


class Source:
    """Independently controlled silent, heartbeat, or CRC-corrupt input."""
    def __init__(self, kind, destination=0):
        self.kind, self.wire = kind, Wire()
        self.lock, self.stop, self.ready = threading.RLock(), threading.Event(), threading.Event()
        self.heartbeats = self.corrupt = False
        self.replies, self.errors = [], []
        self.connection = self.master = self.slave = None
        if kind == "udp":
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.socket.bind(("127.0.0.1", 0))
            self.socket.settimeout(.025)
            self.destination = ("127.0.0.1", destination)
            self.ready.set()
            self.endpoint = f"udp://127.0.0.1:{destination}"
        elif kind == "ws":
            self.socket = socket.socket()
            self.socket.bind(("127.0.0.1", 0))
            self.socket.listen()
            self.socket.settimeout(.05)
            self.endpoint = f"ws://127.0.0.1:{self.socket.getsockname()[1]}/mavlink"
        else:
            import pty
            self.master, self.slave = pty.openpty()
            os.set_blocking(self.master, False)
            self.socket = None
            self.endpoint = os.ttyname(self.slave)
            self.ready.set()
        self.thread = threading.Thread(target=self.serve, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def send(self, factory):
        with self.lock:
            if not self.ready.is_set():
                return False
            payload = factory(self.wire)
            try:
                if self.kind == "udp":
                    self.socket.sendto(payload, self.destination)
                elif self.kind == "ws":
                    self.connection.sendall(websocket_frame(payload))
                else:
                    assert os.write(self.master, payload) == len(payload)
                return True
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                return False

    def emit(self, factory, timeout=2):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            if self.send(factory):
                return
            time.sleep(.025)
        raise AssertionError(f"{self.kind} did not become writable")

    def upgrade(self):
        try:
            connection, _ = self.socket.accept()
        except socket.timeout:
            return
        connection.settimeout(.5)
        request = b""
        while b"\r\n\r\n" not in request:
            part = connection.recv(4096)
            if not part:
                connection.close()
                return
            request += part
            assert len(request) < 16384
        headers = {}
        for line in request.split(b"\r\n")[1:]:
            if b":" in line:
                key, value = line.split(b":", 1)
                headers[key.lower()] = value.strip()
        accept = base64.b64encode(hashlib.sha1(headers[b"sec-websocket-key"] +
                   b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
        connection.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                           b"Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
        connection.settimeout(.025)
        with self.lock:
            self.connection = connection
            self.ready.set()

    def serve(self):
        try:
            next_heartbeat = time.monotonic()
            while not self.stop.is_set():
                if self.kind == "ws" and not self.ready.is_set():
                    self.upgrade()
                    continue
                if (self.heartbeats or self.corrupt) and time.monotonic() >= next_heartbeat:
                    def heartbeat(wire):
                        packet = wire.heartbeat()
                        return packet[:-1] + bytes((packet[-1] ^ 0x80,)) if self.corrupt else packet
                    self.send(heartbeat)
                    next_heartbeat = time.monotonic() + .08
                if self.kind == "udp":
                    try:
                        self.replies.append(self.socket.recvfrom(65535))
                    except (socket.timeout, ConnectionResetError, ConnectionRefusedError):
                        pass
                elif self.kind == "ws":
                    try:
                        if not self.connection.recv(4096):
                            with self.lock:
                                self.connection.close()
                                self.connection = None
                                self.ready.clear()
                    except socket.timeout:
                        pass
                    except (ConnectionResetError, ConnectionAbortedError):
                        with self.lock:
                            self.connection.close()
                            self.connection = None
                            self.ready.clear()
                else:
                    try:
                        os.read(self.master, 4096)
                    except BlockingIOError:
                        pass
                    self.stop.wait(.025)
        except Exception as error:
            if not self.stop.is_set():
                self.errors.append(repr(error))

    def __exit__(self, *unused):
        self.stop.set()
        self.thread.join(timeout=2)
        assert not self.thread.is_alive(), f"{self.kind} producer failed to stop"
        if self.connection is not None:
            self.connection.close()
        if self.socket is not None:
            self.socket.close()
        if self.master is not None:
            os.close(self.master)
            os.close(self.slave)
        assert not self.errors, self.errors


class Router:
    def __init__(self, executable, server, stream, reservation, duration, probe_ms=4000):
        port = reservation.port
        self.directory = tempfile.TemporaryDirectory(prefix="auto-input-config-")
        path = Path(self.directory.name) / "router.ini"
        settings = {
            "url": f"http://127.0.0.1:{server.port}/observations",
            "stream": "auto-input", "session": "source-isolation",
            "input_mode": "mavlink", "serial_port": stream.endpoint if stream.kind == "serial" else "none",
            "websocket_url": stream.endpoint if stream.kind == "ws" else "",
            "serial_baud": 115200, "auto_scan": 0, "udp_enabled": 1,
            "qgc_enabled": 0,
            "udp_bind": "127.0.0.1", "udp_port": port,
            "probe_ms": probe_ms, "link_timeout_ms": 450, "reconnect_ms": 100,
            "connect_timeout_ms": 250, "io_timeout_ms": 100,
            "duration_ms": duration, "send_interval_ms": 1000, "refresh_ms": 250,
            "origin_auto": 0, "origin_lat": 37, "origin_lon": 127, "origin_alt": 100,
            "system_time_uncert_us": 2000, "max_windows": 4,
        }
        path.write_text("".join(f"{key}={value}\n" for key, value in settings.items()), encoding="utf-8")
        self.lines, self.stdout = [], []
        self.started = time.monotonic()
        reservation.release()
        self.process = subprocess.Popen([str(executable), "--config", str(path), "--interactive"],
                                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        text=True, encoding="utf-8", errors="replace", bufsize=1)
        self.stderr_thread = threading.Thread(target=self.read_lines, daemon=True)
        self.stdout_thread = threading.Thread(target=lambda: self.stdout.append(self.process.stdout.read()), daemon=True)
        self.stderr_thread.start()
        self.stdout_thread.start()

    def read_lines(self):
        for line in self.process.stderr:
            self.lines.append((time.monotonic(), line.rstrip()))

    def diagnostic(self):
        return "\n".join(line for _, line in self.lines)[-10000:]

    def statuses(self, since=0):
        result = []
        for timestamp, line in self.lines:
            match = re.fullmatch(r"source status=(\S+) active=(.*?)(?: baud=(\d+))?", line)
            if match and timestamp >= since:
                result.append((timestamp, match.group(1), match.group(2)))
        return result

    def locked(self, endpoint, since=0, timeout=2.4):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            matches = [entry for entry in self.statuses(since) if entry[1] == "LOCKED" and entry[2] == endpoint]
            if matches:
                return matches[-1][0]
            assert self.process.poll() is None, self.diagnostic()
            time.sleep(.025)
        raise AssertionError(("source did not lock", endpoint, self.statuses(since), self.diagnostic()))

    def finish(self):
        try:
            assert self.process.wait(timeout=18) == 0, self.diagnostic()
            self.stderr_thread.join(timeout=2)
            self.stdout_thread.join(timeout=2)
            assert not self.stderr_thread.is_alive() and not self.stdout_thread.is_alive()
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait(timeout=2)
            self.process.stdout.close()
            self.process.stderr.close()
            self.directory.cleanup()

    def abort(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=2)
        self.stderr_thread.join(timeout=2)
        self.stdout_thread.join(timeout=2)
        self.process.stdout.close()
        self.process.stderr.close()
        self.directory.cleanup()


def rows(server):
    return [(payload, row) for _, _, payload in server.records
            for batch in payload["batches"] for row in batch["raw"]]


def wait_key(server, key, router, timeout=2.2):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        matches = [(payload, row) for payload, row in rows(server) if row[3] == key]
        if matches:
            return matches
        time.sleep(.025)
    raise AssertionError(("observation was not confirmed", key, router.diagnostic()))


def complete_sample(wire, boot_ms, key, value, offset, latitude_index):
    clock = lambda boot: wire.packet(2, struct.pack("<QI", UTC_OFFSET + offset + boot * 1000, boot), 137)
    return (clock(boot_ms) + wire.navigation(boot_ms, latitude_index) +
            wire.named(boot_ms + 50, key, value) + wire.navigation(boot_ms + 100, latitude_index) +
            clock(boot_ms + 100) + wire.navigation(boot_ms + 400, latitude_index))


def udp_during_silent_probe(executable, kind):
    with UdpReservation() as reservation, Source(kind) as stream, Source("udp", reservation.port) as udp, ConfirmationServer() as server:
        port = reservation.port
        udp.corrupt = True
        router = Router(executable, server, stream, reservation, 5000)
        try:
            assert stream.ready.wait(1.5), ("non-UDP source was not opened", router.diagnostic())
            time.sleep(1.2)
            assert not [entry for entry in router.statuses() if entry[1] in ("LOCKED", "WAIT_HEARTBEAT")], router.diagnostic()
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as competitor:
                try:
                    competitor.bind(("127.0.0.1", port))
                except OSError:
                    pass
                else:
                    raise AssertionError("UDP listener was not held exclusively during the other source probe")
            valid_started = time.monotonic()
            udp.corrupt = False
            udp.heartbeats = True
            locked_at = router.locked(udp.endpoint, valid_started)
            assert locked_at - router.started < 3.5, ("UDP waited for the 4000ms serial/WS probe", locked_at - router.started)
            router.finish()
            assert not server.records and not udp.replies, (server.records, udp.replies)
        except BaseException:
            router.abort()
            raise
    print(f"UDP wins during silent {kind} probe, corrupt frames do not lock, exclusive bind/no UDP replies: PASS")


def udp_probe_lease(executable):
    with UdpReservation() as reservation, Source("ws") as stream, Source("udp", reservation.port) as noise, Source("udp", reservation.port) as good, ConfirmationServer() as server:
        port = reservation.port
        noise.corrupt = True
        router = Router(executable, server, stream, reservation, 4500, probe_ms=400)
        stop = threading.Event()
        bind_results = []

        def compete():
            while not stop.is_set():
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as competitor:
                    try:
                        competitor.bind(("127.0.0.1", port))
                    except OSError:
                        bind_results.append(True)
                    else:
                        bind_results.append(False)
                stop.wait(.002)

        worker = threading.Thread(target=compete, daemon=True)
        try:
            assert stream.ready.wait(1.5), router.diagnostic()
            worker.start()
            # Cross multiple peer/parser probe epochs while CRC noise arrives.
            time.sleep(1.15)
            assert not [entry for entry in router.statuses() if entry[1] in ("LOCKED", "WAIT_HEARTBEAT")], router.diagnostic()
            noise.corrupt = False
            switched = time.monotonic()
            good.heartbeats = True
            router.locked(good.endpoint, switched)
            stop.set()
            worker.join(timeout=1)
            assert not worker.is_alive()
            assert len(bind_results) > 100 and all(bind_results), ("UDP socket released during probe reset", len(bind_results), bind_results.count(False))
            router.finish()
            assert not server.records and not noise.replies and not good.replies
        except BaseException:
            stop.set()
            if worker.is_alive():
                worker.join(timeout=1)
            router.abort()
            raise
    print("Invalid-peer probe expiry accepts a new peer while >100 exclusive-bind attempts remain blocked: PASS")


def bidirectional_failover(executable, kind):
    with UdpReservation() as reservation, Source(kind) as stream, Source("udp", reservation.port) as udp, ConfirmationServer() as server:
        port = reservation.port
        stream.heartbeats = True
        router = Router(executable, server, stream, reservation, 14000)
        try:
            router.locked(stream.endpoint)
            stream.emit(lambda wire: complete_sample(wire, 1000, "source_a", 11, 0, 0))
            wait_key(server, "source_a", router)
            alternate_started = time.monotonic()
            udp.heartbeats = True
            time.sleep(1.1)
            latest = router.statuses(alternate_started)
            assert latest and all(state == "LOCKED" and endpoint == stream.endpoint
                                  for _, state, endpoint in latest), router.diagnostic()
            assert all(row[3] == "source_a" for _, row in rows(server))
            first_loss = time.monotonic()
            stream.heartbeats = False
            router.locked(udp.endpoint, first_loss)
            # Same IDs/elapsed time as A; inherited navigation would accept it.
            udp.emit(lambda wire: wire.named(1050, "orphan_b", 99))
            time.sleep(.55)
            assert not any(row[3] == "orphan_b" for _, row in rows(server)), router.diagnostic()
            udp.emit(lambda wire: complete_sample(wire, 3000, "source_b", 22, 86_400_000_000, 1000))
            wait_key(server, "source_b", router)
            second_loss = time.monotonic()
            udp.heartbeats = False
            stream.heartbeats = True
            router.locked(stream.endpoint, second_loss)
            stream.emit(lambda wire: wire.named(3050, "orphan_c", 99))
            time.sleep(.55)
            assert not any(row[3] == "orphan_c" for _, row in rows(server)), router.diagnostic()
            stream.emit(lambda wire: complete_sample(wire, 7000, "source_c", 33, 172_800_000_000, 2000))
            wait_key(server, "source_c", router)
            router.finish()
            assert not server.errors and not udp.replies, (server.errors, udp.replies)
            actual = rows(server)
            assert {row[3] for _, row in actual} == {"source_a", "source_b", "source_c"}, actual
            expected = {"source_a": (11, 1_050_000, 0, 0),
                        "source_b": (22, 3_050_000, 86_400_000_000, 1000),
                        "source_c": (33, 7_050_000, 172_800_000_000, 2000)}
            sessions = {}
            for payload, row in actual:
                key = row[3]
                value, boot_us, offset, latitude_index = expected[key]
                assert row[11] == value and int(row[0]) == boot_us, row
                assert row[15] is not None and abs(int(row[15]) - UTC_OFFSET - offset - boot_us) <= 1, row
                expected_north = latitude_index * 1e-7 * math.pi / 180 * 6_357_000
                assert math.isclose(row[6], expected_north, rel_tol=.01, abs_tol=.01), row
                sessions.setdefault(key, set()).add(payload["session"])
                assert {other[3] for batch in payload["batches"] for other in batch["raw"]} == {key}, payload
            assert len({next(iter(value)) for value in sessions.values()}) == 3, sessions
        except BaseException:
            router.abort()
            raise
    print(f"{kind} -> UDP -> {kind} timeout failover, stable lock, isolated UTC/navigation/session/HTTP data: PASS")


def main():
    assert EXE.is_file(), f"build the production router first: {EXE}"
    with tempfile.TemporaryDirectory(prefix="auto-input-binary-") as temporary:
        executable = Path(temporary) / EXE.name
        shutil.copy2(EXE, executable)
        udp_during_silent_probe(executable, "ws")
        udp_probe_lease(executable)
        bidirectional_failover(executable, "ws")
        if sys.platform != "win32":
            udp_during_silent_probe(executable, "serial")
            bidirectional_failover(executable, "serial")


if __name__ == "__main__":
    main()
