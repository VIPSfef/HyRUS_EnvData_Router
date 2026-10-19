"""Production heartbeat/coordinate test mode and optional HTTP/TCP test server.

Usage: python test_test_stream.py ROUTER_EXE [TEST_SERVER_EXE]
All sockets, configurations, and synthetic MAVLink belong to this test. No QGC
port, real serial device, production configuration, or observation file is used.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
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

from test_mavlink_router import Wire

SCHEMA = "mavlink_router_test_v1"


class CaptureServer:
    """Keep original HTTP bytes as well as an independent JSON interpretation."""
    def __init__(self, retry=False):
        self.requests, self.errors = [], []
        self.lock = threading.Lock()
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def handle(self):
                try:
                    super().handle()
                except (ConnectionResetError, BrokenPipeError):
                    pass

            def do_POST(self):
                try:
                    assert self.path == "/ingest", self.path
                    assert self.headers.get("Content-Type", "").startswith("application/json")
                    length = int(self.headers["Content-Length"])
                    assert 0 < length <= 128 * 1024, length
                    body = self.rfile.read(length)
                    payload = json.loads(body)
                    with owner.lock:
                        status = 202 if retry and not owner.requests else 204
                        owner.requests.append((time.monotonic(), status, body, payload))
                    self.send_response(status)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                except Exception as error:
                    owner.errors.append(repr(error))
                    self.send_error(400)

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_port
        self.thread = threading.Thread(target=lambda: self.server.serve_forever(.025), daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *unused):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        assert not self.thread.is_alive(), "test HTTP server did not stop"


def record_bytes(body):
    """Read each record's lexical JSON, so retries cannot silently recompute it."""
    text = body.decode("utf-8")
    match = re.search(r'"records"\s*:\s*\[', text)
    assert match, text
    cursor = match.end()
    decoder = json.JSONDecoder()
    result = {}
    while True:
        while cursor < len(text) and text[cursor] in " \r\n\t,":
            cursor += 1
        if text[cursor] == "]":
            return result
        start = cursor
        item, cursor = decoder.raw_decode(text, cursor)
        result[(item["session"], item["seq"])] = text[start:cursor].encode("utf-8")


def validate_records(server, port, require_position, retry, mode, expected_positions):
    assert not server.errors, server.errors
    assert len(server.requests) >= 3, server.requests
    acknowledged, lexical, first_seen = {}, {}, {}
    for timestamp, status, body, payload in server.requests:
        assert set(payload) == {"schema", "stream", "records"}, payload
        assert payload["schema"] == SCHEMA and payload["stream"] == "test-stream", payload
        assert 1 <= len(payload["records"]) <= 4, payload
        assert b"dust_trial" not in body and b'"batches"' not in body, body
        raw_records = record_bytes(body)
        for record in payload["records"]:
            assert set(record) == {"seq", "source_generation", "session", "source",
                                   "heartbeat", "position", "coordinate_age_ms"}, record
            assert type(record["seq"]) is int and record["seq"] > 0, record
            assert type(record["source_generation"]) is int and record["source_generation"] >= 0, record
            assert record["session"].startswith("test-session."), record
            assert record["source"] == f"udp://127.0.0.1:{port}", record
            heartbeat = record["heartbeat"]
            assert set(heartbeat) == {"sysid", "compid", "type", "autopilot", "base_mode",
                                      "custom_mode", "system_status", "received_monotonic_ms"}, heartbeat
            expected = {"sysid": 1, "compid": 1, "type": 2, "autopilot": 12,
                        "base_mode": 128, "custom_mode": 0, "system_status": 4}
            assert all(heartbeat[key] == value for key, value in expected.items()), heartbeat
            assert type(heartbeat["received_monotonic_ms"]) is int, heartbeat
            assert heartbeat["received_monotonic_ms"] >= 0, heartbeat
            position = record["position"]
            if position is None:
                assert record["coordinate_age_ms"] is None, record
            else:
                assert require_position, record
                assert set(position) == {"lat_deg", "lon_deg", "alt_msl_m", "boot_ms",
                                         "received_monotonic_ms"}, position
                assert type(position["boot_ms"]) is int and position["boot_ms"] in expected_positions, position
                expected_latitude = expected_positions[position["boot_ms"]]
                assert math.isclose(position["lat_deg"], expected_latitude, abs_tol=1e-10), position
                assert position["lon_deg"] == 127 and position["alt_msl_m"] == 100, position
                assert type(position["received_monotonic_ms"]) is int, position
                age = record["coordinate_age_ms"]
                assert type(age) is int and age >= 0, record
                # Capture happens after the most recent heartbeat; navigation
                # has its own real receipt time and is never moved to HB time.
                assert position["received_monotonic_ms"] <= heartbeat["received_monotonic_ms"] + age, record
            identity = record["session"], record["seq"]
            if identity in lexical:
                assert raw_records[identity] == lexical[identity], ("retry record bytes changed", identity)
            else:
                lexical[identity] = raw_records[identity]
                first_seen[identity] = timestamp
            if status == 204:
                assert identity not in acknowledged, ("acknowledged record sent twice", identity)
                acknowledged[identity] = record
    assert len(acknowledged) >= 3, acknowledged
    records = sorted(acknowledged.values(), key=lambda item: item["seq"])
    assert [item["seq"] for item in records] == list(range(1, len(records) + 1)), records
    heartbeat_times = [item["heartbeat"]["received_monotonic_ms"] for item in records]
    assert len(set(heartbeat_times)) == len(heartbeat_times), "one unchanged heartbeat was enqueued twice"
    assert all(right - left >= 750 for left, right in zip(heartbeat_times, heartbeat_times[1:])), heartbeat_times
    if retry:
        first, second = server.requests[:2]
        assert first[1] == 202 and second[1] == 204, server.requests
        original = record_bytes(first[2])
        repeated = record_bytes(second[2])
        assert original.keys() <= repeated.keys(), (original, repeated)
        assert all(repeated[key] == value for key, value in original.items()), (original, repeated)
    if require_position:
        positions = [item for item in records if item["position"] is not None]
        assert len(positions) >= 2, records
        if mode == "coordinates":
            assert any(item["coordinate_age_ms"] >= 500 for item in positions), positions
            assert all(item["coordinate_age_ms"] < 1000 for item in positions), positions
            assert records[-1]["position"] is None and records[-1]["coordinate_age_ms"] is None, records
        elif mode == "reboot":
            assert len({item["session"] for item in records}) == 2, records
            assert len({item["source_generation"] for item in records}) == 2, records
            old = [item for item in positions if item["position"]["boot_ms"] >= 10000]
            new = [item for item in positions if item["position"]["boot_ms"] < 10000]
            assert old and new, records
            for item in new:
                assert item["heartbeat"]["received_monotonic_ms"] >= item["position"]["received_monotonic_ms"], (
                    "old pre-reboot heartbeat was paired with new epoch position", item)
            assert old[0]["session"] != new[0]["session"], records
    return records


def client_case(executable, directory, mode, implicit_config=False):
    duration = 1800 if mode == "no-heartbeat" else 5600 if mode == "reboot" else 4400
    with CaptureServer(retry=mode == "heartbeat") as server:
        # Keep the destination reserved until after process creation. If the
        # child's first bind races us, its normal reconnect path retries; no
        # other test can take this ephemeral port while its config is written.
        reservation = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
        config = directory / ("telemetry.ini" if implicit_config else f"{mode}.ini")
        settings = {
            "url": "http://127.0.0.1:1/should-be-overridden", "stream": "test-stream",
            "session": "test-session", "input_mode": "mavlink", "serial_port": "none",
            "auto_scan": 0, "websocket_url": "", "udp_enabled": 1,
            "udp_bind": "127.0.0.1", "udp_port": port,
            "probe_ms": 2000, "link_timeout_ms": 4000 if mode == "reboot" else 1000, "reconnect_ms": 100,
            "duration_ms": duration, "send_interval_ms": 1000,
            "connect_timeout_ms": 500, "io_timeout_ms": 500, "refresh_ms": 250,
        }
        saved_settings = "".join(f"{key}={value}\n" for key, value in settings.items())
        config.write_text(saved_settings, encoding="utf-8")
        arguments = [str(executable)]
        if not implicit_config:
            arguments += ["--config", str(config)]
        arguments += ["--test-heartbeat", "--server-url", f"http://127.0.0.1:{server.port}/ingest"]
        child = subprocess.Popen(arguments, cwd=directory, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        reservation.close()
        stop = threading.Event()
        errors, replies, expected_positions = [], [], {}

        def produce():
            try:
                wire = Wire()
                started, index = time.monotonic(), 0
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
                    source.bind(("127.0.0.1", 0))
                    source.settimeout(.01)
                    while not stop.is_set():
                        payload = b""
                        elapsed = time.monotonic() - started
                        have_heartbeat = mode != "no-heartbeat" and not (mode == "reboot" and 1.2 <= elapsed < 2.7)
                        if have_heartbeat:
                            payload += wire.heartbeat()
                        have_navigation = mode == "no-heartbeat" or mode == "reboot" or (mode == "coordinates" and elapsed < 1.5)
                        if have_navigation:
                            boot_ms = (10000 if elapsed < 1.2 else 1000) + index * 100 if mode == "reboot" else 5000 + index * 100
                            expected_positions[boot_ms] = 37 + index * 1e-7
                            payload += wire.navigation(boot_ms, index)
                            payload += wire.named(boot_ms + 50, "dust_trial", 42.5)
                            # This independent GPS sample must never replace a
                            # GLOBAL_POSITION_INT coordinate while keeping the
                            # latter's boot/receipt timestamps.
                            raw_gps = struct.pack("<QiiiHHHHBB", 1_800_000_000_000_000,
                                380_000_000, 1_280_000_000, 200_000,
                                65535, 65535, 65535, 65535, 3, 10)
                            payload += wire.packet(24, raw_gps, 24)
                        if have_heartbeat and mode in ("coordinates", "reboot"):
                            payload += wire.heartbeat()
                        source.sendto(payload, ("127.0.0.1", port))
                        try:
                            replies.append(source.recvfrom(65535))
                        except (socket.timeout, ConnectionResetError, ConnectionRefusedError):
                            pass
                        index += 1
                        stop.wait(.09)
            except BaseException as error:
                errors.append(repr(error))

        producer = threading.Thread(target=produce, daemon=True)
        producer.start()
        try:
            stdout, stderr = child.communicate(timeout=12)
        finally:
            stop.set()
            producer.join(timeout=2)
            if child.poll() is None:
                child.kill()
                child.wait(timeout=2)
        assert child.returncode == 0, (child.returncode, stdout[-2000:], stderr[-6000:])
        assert config.read_text(encoding="utf-8") == saved_settings, "test CLI overwrote persisted server/input settings"
        assert not producer.is_alive() and not errors and not replies, (errors, replies)
        assert b"TEST MODE" in stderr, stderr
        assert b"\x1b" not in stderr, stderr
        assert not stdout, stdout
        if mode == "no-heartbeat":
            assert not server.requests and not server.errors, server.requests
            print("Test mode: NAV/weather without identified heartbeat makes no HTTP request: PASS")
        else:
            records = validate_records(server, port, mode in ("coordinates", "reboot"),
                                       mode == "heartbeat", mode, expected_positions)
            print(f"Test mode {mode}: {len(records)} acknowledged immutable records, no weather payload, real/null navigation: PASS")


def response_status(port, request, fragments=False):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
        connection.settimeout(2)
        if fragments:
            boundary = request.index(b"\r\n\r\n") + 4
            header, body = request[:boundary], request[boundary:]
            chunks = [header[:1], header[1:17], header[17:-1], header[-1:] + body[:2], body[2:11], body[11:]]
            for chunk in chunks:
                connection.sendall(chunk)
                time.sleep(.005)
        else:
            connection.sendall(request)
        response = b""
        while b"\r\n\r\n" not in response:
            part = connection.recv(4096)
            if not part:
                break
            response += part
            assert len(response) <= 16384, response
        if not response:
            return 0
        match = re.match(rb"HTTP/1\.[01] (\d{3})(?: |\r\n)", response)
        assert match, response
        return int(match.group(1))


def request(body, path="/ingest", headers=None):
    values = {"Host": "127.0.0.1", "Content-Type": "application/json",
              "Content-Length": str(len(body)), "Connection": "close"}
    if headers:
        values.update(headers)
    head = f"POST {path} HTTP/1.1\r\n" + "".join(f"{key}: {value}\r\n" for key, value in values.items())
    return head.encode("ascii") + b"\r\n" + body


def server_case(executable, directory):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    child = subprocess.Popen([str(executable), "--bind", "127.0.0.1", "--port", str(port),
                              "--plain", "--duration-ms", "5000", "--max-requests", "100"],
                             cwd=directory, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
    valid = {"schema": SCHEMA, "stream": "test-stream", "records": [{
        "seq": 1, "source_generation": 1, "session": "test-session.0123456789abcdef.1",
        "source": "udp://127.0.0.1:34567", "heartbeat": {"sysid": 1, "compid": 1,
        "type": 2, "autopilot": 12, "base_mode": 128, "custom_mode": 0,
        "system_status": 4, "received_monotonic_ms": 12345}, "position": None,
        "coordinate_age_ms": None}]}
    encoded = json.dumps(valid, separators=(",", ":")).encode("utf-8")
    try:
        until = time.monotonic() + 2
        while True:
            assert child.poll() is None, child.communicate()
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.05):
                    break
            except (ConnectionRefusedError, socket.timeout):
                assert time.monotonic() < until, "C test server did not listen"
                time.sleep(.025)
        assert response_status(port, request(encoded), fragments=True) == 204
        invalid_record = json.loads(encoded)
        invalid_record["records"][0]["heartbeat"].pop("sysid")
        cases = [
            request(b'{"schema":'),
            request(b"\0" + encoded),
            request(encoded.replace(b'"test-stream"', b'"\\\0"')),
            b"POST /ingest HTTP/1.1\r\nHost: localhost\r\nBad\0Name: value\r\nContent-Length: 2\r\n\r\n{}",
            request(json.dumps(invalid_record).encode("utf-8")),
            request(b'{"schema":"wrong","stream":"x","records":[]}'),
            request(encoded, headers={"Content-Length": "NaN"}),
            request(encoded, headers={"Content-Length": "131073"}),
            request(encoded, headers={"Transfer-Encoding": "chunked"}),
            request(encoded, path="/wrong"),
            b"POST /ingest HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\n{}",
        ]
        for malformed in cases:
            assert response_status(port, malformed) != 204, malformed
        # A bad request must not corrupt the next connection's parser state.
        with_position = json.loads(encoded)
        with_position["records"][0]["seq"] = 2
        with_position["records"][0]["position"] = {"lat_deg": 37.000025, "lon_deg": 127,
            "alt_msl_m": 100, "boot_ms": 5100, "received_monotonic_ms": 12200}
        with_position["records"][0]["coordinate_age_ms"] = 145
        assert response_status(port, request(json.dumps(with_position).encode("utf-8"))) == 204
        stdout, stderr = child.communicate(timeout=9)
        assert child.returncode == 0, (child.returncode, stdout[-4000:], stderr[-4000:])
        assert b"\x1b" not in stdout + stderr, (stdout, stderr)
        decoded = []
        decoder = json.JSONDecoder()
        for line in stdout.decode("utf-8", errors="replace").splitlines():
            start = line.find("{")
            if start >= 0:
                try:
                    payload, _ = decoder.raw_decode(line, start)
                    if isinstance(payload, dict) and "heartbeat" in payload:
                        decoded.append(payload)
                except ValueError:
                    pass
        assert decoded == [valid["records"][0], with_position["records"][0]], (decoded, stdout, stderr)
        print("C HTTP/TCP test server: fragmented request, JSON/length/path rejection, subsequent204, plain JSON log: PASS")
    finally:
        if child.poll() is None:
            child.kill()
            child.wait(timeout=2)


def main():
    assert 2 <= len(sys.argv) <= 3, __doc__
    router = Path(sys.argv[1]).resolve()
    server = Path(sys.argv[2]).resolve() if len(sys.argv) == 3 else None
    assert router.is_file(), router
    if server is not None:
        assert server.is_file(), server
    with tempfile.TemporaryDirectory(prefix="heartbeat-stream-test-") as temporary:
        directory = Path(temporary)
        executable = directory / router.name
        shutil.copy2(router, executable)
        client_case(executable, directory, "heartbeat", implicit_config=True)
        client_case(executable, directory, "coordinates")
        client_case(executable, directory, "no-heartbeat")
        client_case(executable, directory, "reboot")
        if server is not None:
            server_executable = directory / server.name
            shutil.copy2(server, server_executable)
            server_case(server_executable, directory)


if __name__ == "__main__":
    main()
