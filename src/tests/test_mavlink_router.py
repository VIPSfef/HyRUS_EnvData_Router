"""End-to-end MAVLink WebSocket -> production router -> HTTP confirmation.

Uses only Python's standard library and local loopback sockets. stdin is closed:
no CSV adapter or manual input participates. Wire CRC generation deliberately
uses a bit-at-a-time algorithm independent of the C decoder.
"""
import base64
from collections import Counter
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
from pathlib import Path
import re
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
EXE = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build" / (
    "windows-x64/mavlink_telemetry_data_router_headless.exe" if sys.platform == "win32"
    else "linux-x64/mavlink_telemetry_data_router_headless")
UTC_OFFSET = 1_800_000_000_000_000


def crc_byte(crc, byte):
    crc ^= byte
    for _ in range(8):
        crc = (crc >> 1) ^ (0x8408 if crc & 1 else 0)
    return crc


class Wire:
    def __init__(self):
        self.sequence = Counter()

    def packet(self, message, payload, extra, component=1):
        sequence = self.sequence[component] % 256
        self.sequence[component] += 1
        header = bytes((len(payload), 0, 0, sequence, 1, component,
                        message & 255, (message >> 8) & 255, message >> 16))
        crc = 0xFFFF
        for byte in header + payload + bytes((extra,)):
            crc = crc_byte(crc, byte)
        return b"\xfd" + header + payload + struct.pack("<H", crc)

    def heartbeat(self, camera=False):
        return self.packet(0, struct.pack("<IBBBBB", 0, 30 if camera else 2,
                           8 if camera else 12, 128, 4, 3), 50,
                           component=100 if camera else 1)

    def clock(self, boot_ms):
        return self.packet(2, struct.pack("<QI", UTC_OFFSET + boot_ms * 1000,
                                         boot_ms), 137)

    def navigation(self, boot_ms, index):
        return self.packet(33, struct.pack("<IiiiihhhH", boot_ms,
                           370_000_000 + index, 1_270_000_000,
                           100_000, 0, 100, 200, -50, 0), 104)

    def named(self, boot_ms, key, value):
        return self.packet(251, struct.pack("<If10s", boot_ms, value,
                                            key.encode("ascii")), 170)


def websocket_frame(payload):
    length = len(payload)
    if length < 126:
        return bytes((0x82, length)) + payload
    if length < 65536:
        return b"\x82\x7e" + struct.pack("!H", length) + payload
    return b"\x82\x7f" + struct.pack("!Q", length) + payload


class WebSocketSource:
    def __init__(self, sample_count=0, add_key=False, paced=False):
        self.sample_count, self.add_key, self.paced = sample_count, add_key, paced
        self.errors, self.upgrades, self.expected = [], [], {}
        self.stop = threading.Event()
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.listener.settimeout(.1)
        self.port = self.listener.getsockname()[1]
        self.thread = threading.Thread(target=self.serve, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def serve(self):
        conn = None
        upgraded = False
        try:
            while not self.stop.is_set():
                try:
                    conn, _ = self.listener.accept()
                    break
                except socket.timeout:
                    pass
            if conn is None:
                return
            conn.settimeout(2)
            request = b""
            while b"\r\n\r\n" not in request:
                data = conn.recv(4096)
                if not data:
                    raise AssertionError("router closed before WebSocket upgrade")
                request += data
                assert len(request) < 16384
            self.upgrades.append(request)
            assert request.startswith(b"GET /mavlink HTTP/1.1\r\n"), request
            headers = {}
            for line in request.split(b"\r\n")[1:]:
                if b":" in line:
                    key, value = line.split(b":", 1)
                    headers[key.lower()] = value.strip()
            assert headers[b"upgrade"].lower() == b"websocket"
            assert headers[b"sec-websocket-version"] == b"13"
            nonce = headers[b"sec-websocket-key"]
            assert len(base64.b64decode(nonce)) == 16
            accept = base64.b64encode(hashlib.sha1(
                nonce + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
            conn.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                         b"Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
            upgraded = True
            wire = Wire()
            # A camera is a visible device, not the selected autopilot clock.
            greeting = wire.heartbeat() + wire.heartbeat(camera=True)
            # Split a MAVLink header across TCP and WebSocket boundaries.
            conn.sendall(websocket_frame(greeting[:3]))
            conn.sendall(websocket_frame(greeting[3:]))
            if self.sample_count:
                packets = []
                for index in range(self.sample_count):
                    boot_ms = 1000 + index * 10
                    if index % 50 == 0:
                        packets.append(wire.clock(boot_ms))
                    packets.append(wire.navigation(boot_ms, index))
                    values = [("temp", 20 + index / 100), ("pm25", 10 + index / 10)]
                    if self.add_key and index >= self.sample_count // 2:
                        values.append(("humidity", 40 + index / 100))
                    for key, value in values:
                        # Float32 is the actual wire value, not a decimal ideal.
                        actual = struct.unpack("<f", struct.pack("<f", value))[0]
                        self.expected[(boot_ms * 1000, key)] = actual
                        packets.append(wire.named(boot_ms, key, value))
                    if self.paced and ((index + 1) % 10 == 0 or index + 1 == self.sample_count):
                        # Source event time advances only 100ms per group. Real
                        # receipt time advances 750ms, spanning multiple HTTP
                        # ticks inside one source event-second. Generate each
                        # group now, so interleaved heartbeat wire sequence IDs
                        # remain ordered rather than overtaking future data.
                        conn.sendall(websocket_frame(b"".join(packets)))
                        packets.clear()
                        if index + 1 < self.sample_count:
                            until = time.monotonic() + .75
                            while not self.stop.is_set() and time.monotonic() < until:
                                conn.sendall(websocket_frame(wire.heartbeat() + wire.heartbeat(camera=True)))
                                self.stop.wait(min(.25, max(0, until - time.monotonic())))
                if packets:
                    conn.sendall(websocket_frame(b"".join(packets)))
            conn.settimeout(.05)
            next_heartbeat = time.monotonic()
            while not self.stop.is_set():
                if time.monotonic() >= next_heartbeat:
                    conn.sendall(websocket_frame(wire.heartbeat() + wire.heartbeat(camera=True)))
                    next_heartbeat = time.monotonic() + .25
                try:
                    if not conn.recv(4096):
                        break
                except socket.timeout:
                    pass
        except (ConnectionResetError, BrokenPipeError, ConnectionAbortedError) as error:
            if not upgraded:
                self.errors.append(repr(error))
        except Exception as error:
            self.errors.append(repr(error))
        finally:
            if conn is not None:
                conn.close()

    def __exit__(self, *args):
        self.stop.set()
        self.thread.join(timeout=3)
        self.listener.close()
        assert not self.thread.is_alive(), "WebSocket source failed to stop"


class ConfirmationServer:
    def __init__(self, retry=False):
        self.records, self.errors = [], []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def handle(self):
                # Native clients may reset an idle keep-alive connection at exit.
                try:
                    super().handle()
                except (ConnectionResetError, BrokenPipeError):
                    pass

            def do_POST(self):
                try:
                    length = int(self.headers["Content-Length"])
                    assert 0 < length < 2_000_000
                    body = self.rfile.read(length)
                    payload = json.loads(body)
                    status = 202 if retry and not owner.records else 204
                    owner.records.append((time.monotonic(), status, payload))
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
        self.thread = threading.Thread(target=lambda: self.server.serve_forever(.05), daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *args):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        assert not self.thread.is_alive(), "HTTP server failed to stop"


def run_router(source, server, name):
    serial = "COM4095" if sys.platform == "win32" else "/dev/mavlink-test-does-not-exist"
    settings = {
        "url": f"http://127.0.0.1:{server.port}/observations", "stream": "integration",
        "session": name, "origin_lat": 37, "origin_lon": 127, "origin_alt": 100, "origin_auto": 0,
        "input_mode": "mavlink", "serial_port": serial, "serial_baud": 115200,
        "websocket_url": f"ws://127.0.0.1:{source.port}/mavlink", "auto_scan": 0,
        "probe_ms": 500, "link_timeout_ms": 1000, "reconnect_ms": 100,
        "duration_ms": 4500, "send_interval_ms": 1000, "connect_timeout_ms": 500,
        "io_timeout_ms": 500, "refresh_ms": 250, "max_windows": 4,
        "system_time_uncert_us": 2000, "unit_map": "temp:C;pm25:ug/m3",
    }
    with tempfile.TemporaryDirectory(prefix="mavlink-router-test-") as directory:
        config = Path(directory) / "router.ini"
        config.write_text("".join(f"{key}={value}\n" for key, value in settings.items()), encoding="utf-8")
        started = time.monotonic()
        run = subprocess.run([str(EXE), "--config", str(config), "--interactive"],
                             stdin=subprocess.DEVNULL, capture_output=True,
                             text=True, encoding="utf-8", errors="replace", timeout=15)
        duration = time.monotonic() - started
    detail = (name, run.returncode, run.stdout[-1000:], run.stderr[-7000:])
    assert run.returncode == 0, detail
    assert 4 <= duration < 12, (name, duration, "input stopped on closed stdin or watchdog stalled")
    assert not source.errors and not server.errors, (name, source.errors, server.errors, detail)
    assert source.upgrades, (name, "configured serial failure did not fall back to WebSocket", detail)
    assert "selected=1.1" in run.stderr and "devices=2" in run.stderr, detail
    assert re.search(r"1\.1 (?:type=2 autop=12|quadrotor\(2\) PX4\(12\))", run.stderr), detail
    assert re.search(r"1\.100 (?:type=30 autop=8|camera\(30\) none\(8\))", run.stderr), detail
    assert "LOCKED" in run.stderr and f"127.0.0.1:{source.port}/mavlink" in run.stderr, detail
    assert re.search(r"rejected=0 sequence_gaps=0 raw_dropped=0 queue_windows_dropped=0 "
                     r"queue_samples_dropped=0 pending=0", run.stderr), detail
    return run


def check_payloads(source, server, retry):
    assert server.records, "observations were received but never posted"
    by_identity = {}
    acknowledged = set()
    for index, (timestamp, status, payload) in enumerate(server.records):
        assert payload["schema"] == 3 and payload["stream"] == "integration"
        assert payload["frame"] == "ENU" and payload["origin_wgs84"] == [37, 127, 100]
        assert payload["source"] == {"sysid": 1, "compid": 1, "epoch": 1, "altitude_datum": "MSL"}, payload["source"]
        assert payload["clock_model"] and payload["clock_model"]["anchor_uncert_max_us"] == 2000
        assert all(int(value) == 0 for value in payload["loss"].values()), payload["loss"]
        batches = payload["batches"]
        assert batches and all(batch["n"] > 0 and batch["raw"] for batch in batches), "empty HTTP batch"
        if index:
            assert timestamp - server.records[index - 1][0] >= .90, "HTTP requests exceed configured 1 Hz"
        for batch in batches:
            # Each immutable batch carries the clock snapshot used when it
            # was sealed; the envelope model describes only the first batch.
            model = batch["clock_model"]
            assert model and model["anchors"] > 0, ("missing per-batch clock model", batch)
            assert model["anchor_uncert_max_us"] == 2000, model
            anchor = batch["raw_clock_anchor"]
            assert anchor["uncert_us"] == 2000, anchor
            assert isinstance(anchor["boot_us"], str) and isinstance(anchor["utc_us"], str), anchor
            assert int(anchor["utc_us"]) == UTC_OFFSET + int(anchor["boot_us"]), anchor
            identity = (payload["stream"], payload["session"], batch["batch_id"])
            assert identity not in acknowledged, ("confirmed batch was sent again", identity)
            if identity in by_identity:
                # Full dictionary equality includes raw rows, statistics,
                # clock_model, and raw_clock_anchor after a 202 response.
                assert batch == by_identity[identity], ("unconfirmed batch or clock metadata changed on retry", identity)
            else:
                by_identity[identity] = batch
            if status == 204:
                acknowledged.add(identity)
    assert set(by_identity) == acknowledged, "unconfirmed data remained after healthy 204 responses"
    if retry:
        assert server.records[0][1] == 202 and server.records[1][1] == 204
        first = server.records[0][2]
        second = server.records[1][2]
        assert first["session"] == second["session"]
        assert {batch["batch_id"] for batch in first["batches"]} <= {
            batch["batch_id"] for batch in second["batches"]}, "202 data was forgotten"
    actual = {}
    for batch in by_identity.values():
        assert batch["raw_n"] == batch["n"] == len(batch["raw"]) and batch["raw_dropped"] == 0
        for stat in batch["stats"]:
            values = [row[11] for row in batch["raw"] if row[3] == stat["key"]]
            assert stat["n"] == len(values)
            assert math.isclose(stat["mean"], statistics.mean(values), rel_tol=1e-12, abs_tol=1e-12)
            assert stat["min"] == min(values) and stat["max"] == max(values)
            expected_variance = statistics.variance(values) if len(values) > 1 else None
            assert (stat["variance"] is None and expected_variance is None) or (
                expected_variance is not None and math.isclose(stat["variance"], expected_variance,
                                                              rel_tol=1e-11, abs_tol=1e-12))
            assert stat["position_error_n"] == 0 and stat["position_error_mean_m"] is None
            assert stat["time_uncert_mean_us"] == stat["time_uncert_max_us"] == 1000
        for row in batch["raw"]:
            assert len(row) == 17 and isinstance(row[0], str)
            boot_us, key = int(row[0]), row[3]
            identity = (boot_us, key)
            assert identity not in actual, ("duplicate observation", identity)
            actual[identity] = row[11]
            assert row[4] == {"temp": "C", "pm25": "ug/m3"}.get(key, "unknown"), row
            latitude_delta = (370_000_000 + (boot_us // 1000 - 1000) // 10) * 1e-7 - 37
            sine = math.sin(math.radians(37))
            meridian = 6378137 * (1 - .0066943799901413165) / (1 - .0066943799901413165 * sine * sine) ** 1.5
            expected_north = latitude_delta * math.pi / 180 * meridian
            assert row[5] == 0 and row[7] == 0 and math.isclose(row[6], expected_north, abs_tol=1e-8), row
            assert row[8:11] == [2, 1, .5], row
            assert row[12] == 1000 and row[13] == 0, row
            assert row[14] is None, "missing position accuracy must remain unknown"
            assert isinstance(row[15], str) and abs(int(row[15]) - (UTC_OFFSET + boot_us)) <= 1, row
            assert 2000 <= row[16] <= 10000, row
    assert actual.keys() == source.expected.keys(), (
        "missing/unexpected observations", len(actual), len(source.expected),
        sorted(source.expected.keys() - actual.keys())[:10], sorted(actual.keys() - source.expected.keys())[:10])
    for key, value in actual.items():
        assert math.isclose(value, source.expected[key], rel_tol=1e-7, abs_tol=1e-6), (key, value, source.expected[key])
    return len(actual)


def adjacent_config_without_arguments():
    interactive=EXE.with_name("mavlink_telemetry_data_router_interactive.exe" if sys.platform=="win32" else "mavlink_telemetry_data_router_interactive")
    assert interactive.is_file(), interactive
    with tempfile.TemporaryDirectory(prefix="router-adjacent-") as temp:
        folder=Path(temp); binary=folder/"실행"; binary.mkdir(); working=folder/"working";working.mkdir()
        copied=binary/interactive.name;shutil.copy2(interactive,copied)
        config=binary/"telemetry.ini"
        original="url=-\nstream=adjacent\nsession=adjacent\ninput_mode=mavlink\nserial_port=none\nauto_scan=0\nduration_ms=1500\n"
        config.write_text(original,encoding="utf-8")
        started=time.monotonic()
        result=subprocess.run([str(copied)],cwd=working,stdin=subprocess.DEVNULL,capture_output=True,timeout=6)
        assert result.returncode==0,result.stderr
        assert 1.4<time.monotonic()-started<5,result.stderr
        assert b"ACTIVE stream=adjacent" in result.stderr and b"NO_CHANNEL" in result.stderr,result.stderr
        assert b"No saved configuration" not in result.stderr
        assert config.read_text(encoding="utf-8")==original
        assert not (working/"telemetry.ini").exists()
    print("No arguments, adjacent UTF-8-path saved configuration, closed stdin: PASS")


def main():
    assert EXE.is_file(), f"build the production router first: {EXE}"
    adjacent_config_without_arguments()
    with WebSocketSource() as source, ConfirmationServer() as server:
        run_router(source, server, "heartbeat-only")
        assert not server.records, "heartbeat-only input generated an empty HTTP request"
    print("Heartbeat-only input with closed stdin: no HTTP requests, device discovery active: PASS")
    with WebSocketSource(300) as source, ConfirmationServer(retry=True) as server:
        run_router(source, server, "confirmation-retry")
        count = check_payloads(source, server, retry=True)
        assert count == 600
    print("Native binary MAVLink -> HTTP 202 retry -> 204, 600 positioned/timed raw observations: PASS")
    with WebSocketSource(30, add_key=True) as source, ConfirmationServer() as server:
        run_router(source, server, "fallback-new-key")
        count = check_payloads(source, server, retry=False)
        assert count == 75
    print("Invalid serial -> WebSocket fallback, autopilot/camera discovery, added humidity key: PASS")
    with WebSocketSource(40, paced=True) as source, ConfirmationServer() as server:
        run_router(source, server, "paced-one-source-second")
        count = check_payloads(source, server, retry=False)
        assert count == 80
        batches = [batch for _, _, payload in server.records for batch in payload["batches"]]
        identities = {batch["batch_id"] for batch in batches}
        assert len(identities) >= 2, "wall-clock HTTP ticks did not split the source event-second"
        assert {batch["window_us"] for batch in batches} == {"1000000"}, batches
        assert len(server.records) >= 2, "only the final source tail was transmitted"
    print("Paced 400ms source interval over 2.25s receipt time, 80 observations in distinct 1Hz batches: PASS")


if __name__ == "__main__":
    main()