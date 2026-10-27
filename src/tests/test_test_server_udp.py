"""Diagnostic server UDP backup, shared validation and bounded display dedup.

All packets and captures are synthetic, in RAM, on ephemeral loopback ports.
No device, user terminal, installed server, or production port is accessed.
"""
import json
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import threading
import time

from test_test_server_tui import heartbeat_body
from test_test_stream import request, response_status

HEADER = struct.Struct("!4sBBHQQIIHHHH")
PAYLOAD = 1200 - HEADER.size


def fragments(body, sequence, nonce=0x123456789ABCDEF0):
    if not isinstance(body, bytes):
        body = json.dumps(body, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    count = (len(body) + PAYLOAD - 1) // PAYLOAD
    return [HEADER.pack(b"MTRU", 1, 0, HEADER.size, nonce, sequence, len(body),
                        index * PAYLOAD, index, count, len(body[index * PAYLOAD:(index + 1) * PAYLOAD]), 0)
            + body[index * PAYLOAD:(index + 1) * PAYLOAD] for index in range(count)]


def ports():
    for _ in range(100):
        with socket.socket() as tcp, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            tcp.bind(("127.0.0.1", 0))
            port = tcp.getsockname()[1]
            if port == 65535 or port in (21000, 14540, 14550, 14551):
                continue
            try:
                udp.bind(("127.0.0.1", port + 1))
                return port, port + 1
            except OSError:
                pass
    raise AssertionError("no private TCP/UDP port pair available")


class Server:
    def __init__(self, executable, port, udp=None, duration=5500):
        args = [str(executable), "--bind", "127.0.0.1", "--port", str(port),
                "--plain", "--duration-ms", str(duration)]
        if udp is not None:
            args.extend(("--udp-port", str(udp)))
        self.child = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE)
        self.records, self.errors, self.stderr = [], [], bytearray()
        self.changed = threading.Condition()

        def read(stream, output):
            try:
                for line in iter(stream.readline, b""):
                    with self.changed:
                        if output:
                            self.records.append(json.loads(line))
                            assert len(self.records) <= 100, "unbounded test output"
                        else:
                            self.stderr.extend(line)
                            assert len(self.stderr) < 1024 * 1024
                        self.changed.notify_all()
            except BaseException as error:
                with self.changed:
                    self.errors.append(repr(error))
                    self.changed.notify_all()

        self.readers = [threading.Thread(target=read, args=(self.child.stdout, True), daemon=True),
                        threading.Thread(target=read, args=(self.child.stderr, False), daemon=True)]
        for reader in self.readers:
            reader.start()
        self.wait(lambda: b"LISTENING" in self.stderr, "server ready")

    def wait(self, predicate, description, timeout=2):
        end = time.monotonic() + timeout
        with self.changed:
            while not predicate():
                assert not self.errors and self.child.poll() is None, (description, self.errors, self.stderr)
                left = end - time.monotonic()
                assert left > 0, (description, self.records, self.stderr)
                self.changed.wait(min(left, .05))

    def finish(self):
        assert self.child.wait(timeout=8) == 0, self.stderr
        for reader in self.readers:
            reader.join(timeout=2)
            assert not reader.is_alive()
        assert not self.errors, self.errors
        return bytes(self.stderr)

    def close(self):
        if self.child.poll() is None:
            self.child.kill()
        self.child.wait(timeout=3)
        for reader in self.readers:
            reader.join(timeout=2)
        self.child.stdout.close()
        self.child.stderr.close()


def send(udp, destination, body, sequence, reverse=False):
    packets = fragments(body, sequence)
    for packet in reversed(packets) if reverse else packets:
        assert udp.sendto(packet, destination) == len(packet)


def integration(executable):
    tcp_port, udp_port = ports()
    server = Server(executable, tcp_port)
    destination = ("127.0.0.1", udp_port)
    expected = []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.bind(("127.0.0.1", 0))
        udp.settimeout(.1)
        try:
            first = heartbeat_body(1)
            send(udp, destination, first, 1)
            expected.extend(json.loads(first)["records"])
            server.wait(lambda: len(server.records) == 1, "default TCP+1 UDP receive")
            try:
                reply = udp.recv(1200)
                raise AssertionError(("UDP must not send ACK", reply))
            except socket.timeout:
                pass
            assert response_status(tcp_port, request(first)) == 204

            second = heartbeat_body(2)
            assert response_status(tcp_port, request(second)) == 204
            expected.extend(json.loads(second)["records"])
            send(udp, destination, second, 2)
            changed = json.loads(second)
            changed["records"][0]["heartbeat"]["custom_mode"] = 123
            send(udp, destination, changed, 3)
            expected.extend(changed["records"])
            server.wait(lambda: len(server.records) == 3, "same ID with changed bytes is not hidden")

            examples = Path(__file__).resolve().parents[2] / "docs" / "api" / "examples"
            weather = json.loads((examples / "synthetic-weather-unknown.json").read_text(encoding="utf-8"))
            weather["records"][0]["future_metadata"] = "synthetic-\u03bc" * 250
            packets = fragments(weather, 4)
            assert len(packets) > 2
            udp.sendto(packets[-1], destination)
            udp.sendto(packets[-1], destination)  # Duplicate fragment before completion.
            for packet in reversed(packets[:-1]):
                udp.sendto(packet, destination)
            expected.extend(weather["records"])
            status = json.loads((examples / "synthetic-status-disconnected.json").read_text(encoding="utf-8"))
            send(udp, destination, status, 5, reverse=True)
            expected.extend(status["records"])
            server.wait(lambda: len(server.records) == 5, "shared weather/status/GPS validation after reordered fragments")

            # Hold one incomplete HTTP request while sending UDP. Its 5s socket
            # deadline must not defer the independent nonblocking UDP receiver.
            slow_body = heartbeat_body(4)
            whole = request(slow_body)
            prefix = len(whole) - len(slow_body) + 10
            with socket.create_connection(("127.0.0.1", tcp_port), timeout=2) as slow:
                slow.sendall(whole[:prefix])
                time.sleep(.15)
                third = heartbeat_body(3)
                started = time.monotonic()
                send(udp, destination, third, 6)
                server.wait(lambda: len(server.records) == 6, "UDP progresses while HTTP body stalls", timeout=.8)
                assert time.monotonic() - started < .8
                expected.extend(json.loads(third)["records"])
                slow.sendall(whole[prefix:])
                assert b" 204 " in slow.recv(512)
                expected.extend(json.loads(slow_body)["records"])
            server.wait(lambda: len(server.records) == 7, "HTTP parser intact after UDP callbacks")

            # A complete but invalid JSON/schema payload is never displayed.
            send(udp, destination, b"{}", 7)
            send(udp, destination, b"{bad-json", 8)
            malformed = bytearray(fragments(heartbeat_body(90), 9)[0])
            malformed[4] = 2
            udp.sendto(malformed, destination)
            incomplete = fragments(weather, 10)
            udp.sendto(incomplete[0], destination)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as other:
                other.bind(("127.0.0.1", 0))
                for packet in incomplete[1:]:
                    other.sendto(packet, destination)  # Another peer cannot finish the first peer's message.
            time.sleep(2.2)
            stderr = server.finish()
            assert server.records == expected, (server.records, expected)
            summary = re.search(rb"udp_messages=(\d+) udp_accepted=(\d+) udp_rejected=(\d+) udp_drop=(\d+) cross_duplicates=(\d+)", stderr)
            assert summary, stderr
            messages, accepted, rejected, dropped, duplicates = map(int, summary.groups())
            assert (messages, accepted, rejected, duplicates) == (8, 6, 2, 2), summary.groups()
            assert dropped >= 2, ("incomplete peer messages not counted", dropped)
            malformed_count = re.search(rb"udp_errors=(\d+)", stderr)
            assert malformed_count and int(malformed_count[1]) >= 1, ("malformed frame not counted", stderr)
            print("PASS UDP default port, fragments/reorder/duplicate/UTF8, shared JSON/GPS validation, cross-transport exact dedup, slow HTTP isolation, no ACK")
        finally:
            server.close()


def options(executable):
    result = subprocess.run([str(executable), "--port", "65535"], capture_output=True, timeout=3)
    assert result.returncode == 2 and b"--udp-port" in result.stderr, result.stderr
    tcp_port, default_udp = ports()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        override = reservation.getsockname()[1]
    server = Server(executable, tcp_port, udp=override, duration=900)
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(("127.0.0.1", default_udp))  # Override did not occupy TCP+1.
            send(udp, ("127.0.0.1", override), heartbeat_body(1), 1)
        server.wait(lambda: len(server.records) == 1, "explicit UDP port")
        server.finish()
    finally:
        server.close()
    tcp_port, default_udp = ports()
    server = Server(executable, tcp_port, udp=0, duration=500)
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(("127.0.0.1", default_udp))
        assert response_status(tcp_port, request(heartbeat_body(1))) == 204
        server.finish()
    finally:
        server.close()
    tcp_port, default_udp = ports()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as occupied:
        occupied.bind(("127.0.0.1", default_udp))
        server = Server(executable, tcp_port, duration=500)
        try:
            assert b"UDP ERROR" in server.stderr
            assert response_status(tcp_port, request(heartbeat_body(1))) == 204
            server.finish()
        finally:
            server.close()
    print("PASS UDP port override/disable/boundary and occupied UDP port keeps HTTP available")


if __name__ == "__main__":
    assert len(sys.argv) == 2, "usage: test_test_server_udp.py TEST_SERVER_EXE"
    executable = Path(sys.argv[1]).resolve()
    assert executable.is_file(), executable
    integration(executable)
    options(executable)
