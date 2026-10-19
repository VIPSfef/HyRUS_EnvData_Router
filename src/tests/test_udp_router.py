"""Production UDP MAVLink -> HTTP, including exclusive port ownership.

All traffic stays on test-owned loopback sockets; stdin is closed. The common
MAVLink wire encoder and independent raw/statistics checks are shared with the
WebSocket integration suite. No production executable or config is changed.
"""
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

from test_mavlink_router import ConfirmationServer, Wire, check_payloads

ROOT = Path(__file__).resolve().parents[2]
EXE = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build" / (
    "windows-x64/mavlink_telemetry_data_router_headless.exe" if sys.platform == "win32"
    else "linux-x64/mavlink_telemetry_data_router_headless")


def unused_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        return reservation.getsockname()[1]


class UdpSource:
    def __init__(self, destination):
        self.destination = ("127.0.0.1", destination)
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.settimeout(.025)
        self.stop = threading.Event()
        self.errors, self.replies, self.expected = [], [], {}
        self.datagram_bytes = 0
        self.thread = threading.Thread(target=self.serve, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def receive_replies(self):
        try:
            payload, peer = self.socket.recvfrom(65535)
            self.replies.append((payload, peer))
        except socket.timeout:
            pass
        except (ConnectionResetError, ConnectionRefusedError):
            # The kernel may report an ICMP response before the app binds or
            # after it closes. This is not a MAVLink reply from the receiver.
            pass

    def serve(self):
        try:
            wire = Wire()
            # Give the app time to bind while preserving one stable peer port.
            until = time.monotonic() + .35
            while not self.stop.is_set() and time.monotonic() < until:
                self.socket.sendto(wire.heartbeat() + wire.heartbeat(camera=True), self.destination)
                self.receive_replies()
                self.stop.wait(.05)
            packets = [wire.heartbeat(), wire.heartbeat(camera=True)]
            for index in range(300):
                boot_ms = 1000 + index * 10
                if index % 50 == 0:
                    packets.append(wire.clock(boot_ms))
                packets.append(wire.navigation(boot_ms, index))
                for key, value in (("temp", 20 + index / 100), ("pm25", 10 + index / 10)):
                    actual = struct.unpack("<f", struct.pack("<f", value))[0]
                    self.expected[(boot_ms * 1000, key)] = actual
                    packets.append(wire.named(boot_ms, key, value))
            datagram = b"".join(packets)
            assert 4096 < len(datagram) <= 65507, len(datagram)
            self.datagram_bytes = len(datagram)
            assert self.socket.sendto(datagram, self.destination) == len(datagram)
            next_heartbeat = time.monotonic() + .25
            while not self.stop.is_set():
                if time.monotonic() >= next_heartbeat:
                    self.socket.sendto(wire.heartbeat() + wire.heartbeat(camera=True), self.destination)
                    next_heartbeat = time.monotonic() + .25
                self.receive_replies()
        except Exception as error:
            self.errors.append(repr(error))

    def __exit__(self, *unused):
        self.stop.set()
        self.thread.join(timeout=2)
        self.socket.close()
        assert not self.thread.is_alive(), "UDP producer failed to stop"


class OccupiedPort:
    """A fake QGC owner explicitly enabling sharing before its initial bind."""
    def __init__(self):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.settimeout(.2)
        self.port = self.socket.getsockname()[1]
        self.sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.stop = threading.Event()
        self.errors, self.received = [], []
        self.thread = threading.Thread(target=self.serve, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def serve(self):
        try:
            count = 0
            while not self.stop.is_set():
                marker = ("existing-qgc-owner-%u" % count).encode("ascii")
                self.sender.sendto(marker, ("127.0.0.1", self.port))
                received, _ = self.socket.recvfrom(1024)
                assert received == marker, (received, marker)
                self.received.append((time.monotonic(), marker))
                count += 1
                self.stop.wait(.1)
        except Exception as error:
            self.errors.append(repr(error))

    def __exit__(self, *unused):
        self.stop.set()
        self.thread.join(timeout=2)
        try:
            assert not self.thread.is_alive(), "existing owner failed to stop"
            assert not self.errors, self.errors
            marker = b"existing-qgc-owner-after-router-exit"
            self.sender.sendto(marker, ("127.0.0.1", self.port))
            assert self.socket.recvfrom(1024)[0] == marker
        finally:
            self.sender.close()
            self.socket.close()


def run_router(executable, server, port, session):
    settings = {
        "url": f"http://127.0.0.1:{server.port}/observations", "stream": "integration",
        "session": session, "origin_lat": 37, "origin_lon": 127, "origin_alt": 100,
        "origin_auto": 0, "input_mode": "mavlink", "serial_port": "none", "auto_scan": 0,
        "websocket_url": "", "udp_enabled": 1, "udp_bind": "127.0.0.1", "udp_port": port,
        "probe_ms": 5000, "link_timeout_ms": 5000, "reconnect_ms": 100, "duration_ms": 4500,
        "send_interval_ms": 1000, "connect_timeout_ms": 500, "io_timeout_ms": 500,
        "refresh_ms": 250, "max_windows": 4, "system_time_uncert_us": 2000,
        "unit_map": "temp:C;pm25:ug/m3",
    }
    with tempfile.TemporaryDirectory(prefix="udp-router-config-") as temporary:
        config = Path(temporary) / "router.ini"
        config.write_text("".join(f"{key}={value}\n" for key, value in settings.items()), encoding="utf-8")
        started = time.monotonic()
        run = subprocess.run([str(executable), "--config", str(config), "--interactive"],
                             stdin=subprocess.DEVNULL, capture_output=True, text=True,
                             encoding="utf-8", errors="replace", timeout=15)
        elapsed = time.monotonic() - started
    detail = (session, run.returncode, run.stdout[-1000:], run.stderr[-8000:])
    assert run.returncode == 0, detail
    assert 4 <= elapsed < 12, ("closed stdin ended the receiver or watchdog stalled", elapsed, detail)
    assert not server.errors, (server.errors, detail)
    assert f"udp://127.0.0.1:{port}" in run.stderr, detail
    assert re.search(r"rejected=0 sequence_gaps=0 raw_dropped=0 queue_windows_dropped=0 "
                     r"queue_samples_dropped=0 pending=0", run.stderr), detail
    return run, detail


def udp_observations(executable):
    port = unused_port()
    with ConfirmationServer(retry=True) as server, UdpSource(port) as source:
        run, detail = run_router(executable, server, port, "udp-confirmation")
        assert not source.errors, (source.errors, detail)
        assert "LOCKED" in run.stderr and "selected=1.1" in run.stderr and "devices=2" in run.stderr, detail
        assert "quadrotor(2) PX4(12)" in run.stderr and "camera(30) none(8)" in run.stderr, detail
        assert not source.replies, ("receive-only UDP sent data back to its source", source.replies)
        count = check_payloads(source, server, retry=True)
        assert count == 600
        assert source.datagram_bytes > 4096
    print("Large UDP datagram -> HTTP 202 retry -> 204, 600 raw/time/ENU/statistics, no UDP reply: PASS")


def occupied_port(executable):
    with OccupiedPort() as owner, ConfirmationServer() as server:
        started = time.monotonic()
        run, detail = run_router(executable, server, owner.port, "udp-owned-port")
        assert "CONNECT_FAILED" in run.stderr and "UDP bind failed" in run.stderr, detail
        assert "QGC must use a different port" in run.stderr, detail
        assert "LOCKED" not in run.stderr and not server.records, detail
        assert not owner.errors and len(owner.received) >= 20, (owner.errors, len(owner.received), detail)
        assert owner.received[-1][0] - started >= 4, ("existing receiver stopped receiving", owner.received[-1], detail)
    print("Existing SO_REUSEADDR UDP owner retained its port and all marker traffic; router refused bind: PASS")


def main():
    assert EXE.is_file(), f"build the production router first: {EXE}"
    # Copy the actual production image so a concurrent build never replaces a
    # running Windows executable. Only the synthetic config is persisted.
    with tempfile.TemporaryDirectory(prefix="udp-router-binary-") as temporary:
        executable = Path(temporary) / EXE.name
        shutil.copy2(EXE, executable)
        udp_observations(executable)
        occupied_port(executable)


if __name__ == "__main__":
    main()