"""Production serial rate requests against a synthetic FC on a private POSIX PTY.

No physical port, production configuration, QGC peer, or persistent observation
log is used. Independently check every outgoing frame and command payload.
"""
from collections import Counter
import os
from pathlib import Path
import select
import struct
import subprocess
import sys
import tempfile
import time

from test_mavlink_router import Wire, crc_byte


class RateRouter:
    def __init__(self, executable, directory, enabled):
        import pty
        import tty

        self.master, self.slave = pty.openpty()
        tty.setraw(self.slave)
        os.set_blocking(self.master, False)
        self.wire = Wire()
        self.gcs = Wire(system=255)
        self.bytes = bytearray()
        self.log = bytearray()
        self.commands = []
        self.heartbeats = 0
        self.vehicle = False
        self.start = time.monotonic()
        self.next_heartbeat = self.next_navigation = self.start
        self.child = None
        values = {
            "url": "-", "stream": "rate-pty", "session": "rate-pty",
            "input_mode": "mavlink", "serial_port": os.ttyname(self.slave),
            "serial_baud": 57600, "auto_scan": 0, "udp_enabled": 0,
            "qgc_enabled": 0, "mavlink_rates_enabled": int(enabled),
            "probe_ms": 5000, "link_timeout_ms": 5000, "reconnect_ms": 100,
            "refresh_ms": 250, "duration_ms": 35000,
        }
        config = directory / ("enabled.ini" if enabled else "disabled.ini")
        config.write_text("".join(f"{key}={value}\n" for key, value in values.items()),
                          encoding="utf-8")
        try:
            self.child = subprocess.Popen(
                [str(executable), "--config", str(config), "--interactive"],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE)
            os.set_blocking(self.child.stderr.fileno(), False)
            # Serial tcflush completes before the initial router heartbeat.
            self.until(lambda: self.heartbeats > 0, "serial open", timeout=4)
        except BaseException:
            self.close()
            raise

    def write(self, data):
        offset = 0
        while offset < len(data):
            offset += os.write(self.master, data[offset:])

    def decode(self):
        while self.bytes:
            magic = self.bytes[0]
            assert magic in (0xfe, 0xfd), ("corrupt/interleaved serial frame", bytes(self.bytes))
            header = 10 if magic == 0xfd else 6
            if len(self.bytes) < header:
                return
            length = self.bytes[1]
            signed = magic == 0xfd and self.bytes[2] & 1
            total = header + length + 2 + (13 if signed else 0)
            if len(self.bytes) < total:
                return
            frame = bytes(self.bytes[:total])
            del self.bytes[:total]
            message = int.from_bytes(frame[7:10], "little") if magic == 0xfd else frame[5]
            assert message in (0, 76), ("unexpected router serial message", message)
            crc = 0xffff
            for byte in frame[1:header + length] + bytes((50 if message == 0 else 152,)):
                crc = crc_byte(crc, byte)
            assert crc == struct.unpack_from("<H", frame, header + length)[0], "invalid serial CRC"
            if message == 0:
                self.heartbeats += 1
                continue
            assert magic == 0xfd and not signed, "rate command must use MAVLink 2"
            assert frame[5:7] == bytes((255, 191)), "wrong router command identity"
            payload = frame[header:header + length].ljust(33, b"\0")
            parameters = struct.unpack_from("<7f", payload)
            command, target_system, target_component, confirmation = struct.unpack_from("<HBBB", payload, 28)
            assert command == 511, ("non-rate command sent", command)
            assert (target_system, target_component) == (1, 1), "wrong selected FC target"
            assert parameters[0] > 0 and parameters[0].is_integer(), "heartbeat/invalid stream request"
            assert parameters[1] > 0 and parameters[1].is_integer(), "rate requests must not stop a stream"
            assert parameters[2:] == (0.0,) * 5, "unrelated command parameters"
            self.commands.append((time.monotonic(), int(parameters[0]), int(parameters[1]), confirmation))

    def pump(self, timeout=.02):
        now = time.monotonic()
        if now >= self.next_heartbeat:
            heartbeat = self.wire.heartbeat() if self.vehicle else self.gcs.packet(
                0, struct.pack("<IBBBBB", 0, 6, 8, 0, 4, 3), 50, component=190)
            self.write(heartbeat)
            self.next_heartbeat = now + .5
        if self.vehicle and now >= self.next_navigation:
            boot_ms = 1000 + int((now - self.start) * 1000)
            self.write(self.wire.navigation(boot_ms, 0))
            self.next_navigation = now + .2
        descriptors = [self.master, self.child.stderr.fileno()]
        readable, _, _ = select.select(descriptors, [], [], timeout)
        if self.master in readable:
            self.bytes.extend(os.read(self.master, 65536))
            self.decode()
        if self.child.stderr.fileno() in readable:
            self.log.extend(os.read(self.child.stderr.fileno(), 65536))
        assert self.child.poll() is None, (self.child.returncode, self.log.decode(errors="replace"))

    def until(self, predicate, description, timeout=3):
        deadline = time.monotonic() + timeout
        while not predicate():
            assert time.monotonic() < deadline, (description, self.commands,
                                                  self.log.decode(errors="replace"))
            self.pump()

    def quiet(self, duration):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            self.pump(min(.02, max(0, deadline - time.monotonic())))

    def acknowledge(self, result=0, system=1):
        # Full ACK extensions target this router, rather than QGC.
        payload = struct.pack("<HBBiBB", 511, result, 0, 0, 255, 191)
        wire = self.wire if system == self.wire.system else Wire(system=system)
        self.write(wire.packet(77, payload, 143))

    def close(self):
        if self.child is not None:
            if self.child.poll() is None:
                self.child.terminate()
            self.child.wait(timeout=3)
            self.child.stderr.close()
        os.close(self.master)
        os.close(self.slave)


def enabled_case(executable, directory):
    router = RateRouter(executable, directory, True)
    try:
        router.quiet(3.5)
        assert not router.commands, "GCS traffic incorrectly selected a rate-control target"
        router.vehicle = True
        router.until(lambda: bool(router.commands), "first selected FC rate request", timeout=6)
        first = router.commands[0]
        router.acknowledge(system=2)
        router.quiet(.7)
        assert len(router.commands) == 1, "foreign FC ACK accepted"
        router.acknowledge()
        router.until(lambda: len(router.commands) == 2, "next request after accepted ACK")
        refused = router.commands[1][1]
        assert router.commands[1][0] - first[0] >= .5, "rate command burst"
        router.acknowledge(result=3)
        router.until(lambda: len(router.commands) == 3, "next request after unsupported ACK")
        timed_out = router.commands[2]
        router.until(lambda: len(router.commands) == 4, "single bounded retry", timeout=3)
        retry = router.commands[3]
        assert retry[1:3] == timed_out[1:3], (timed_out, retry)
        assert retry[0] - timed_out[0] >= 1.8, "timeout retry flooded serial"
        router.quiet(2.4)
        assert len(router.commands) == 4, "exhausted request bypassed timeout quarantine"
        router.until(lambda: len(router.commands) == 5, "continued after timeout quarantine", timeout=3)
        router.acknowledge()
        deadline = time.monotonic() + 2
        seen = len(router.commands)
        while time.monotonic() < deadline:
            router.pump()
            if len(router.commands) > seen:
                seen = len(router.commands)
                router.acknowledge()
        assert all(command[1] != refused for command in router.commands[2:]), "unsupported stream retried"
        assert all(right[0] - left[0] >= .45 for left, right in zip(router.commands, router.commands[1:])), "command flood"
        assert Counter(command[1:3] for command in router.commands)[timed_out[1:3]] == 2
        print("PASS production serial rates: selected FC only, MAVLink2/CRC/command511, accepted/refused/foreign ACK, bounded retries and quarantine")
    finally:
        router.close()


def disabled_case(executable, directory):
    router = RateRouter(executable, directory, False)
    try:
        router.vehicle = True
        router.quiet(5)
        assert not router.commands, "disabled rate controller sent a command"
        assert router.heartbeats >= 3, "serial receiver/announcements stopped when rates disabled"
        print("PASS disabled rate controller remains passive")
    finally:
        router.close()


if __name__ == "__main__":
    assert os.name != "nt", "POSIX PTY required; portable controller unit tests cover Windows"
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="router-rates-") as temporary:
        enabled_case(executable, Path(temporary))
        disabled_case(executable, Path(temporary))
