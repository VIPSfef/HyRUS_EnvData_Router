"""Independent latest sensor delivery while production HTTP retries are blocked.

Private loopback sockets and synthetic MAVLink only. HTTP/UDP bodies remain in
RAM; disposable files contain only configuration. Every child has a <=35s bound.
"""
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from test_hygrometer_router import atmos_packet
from test_mavlink_router import Wire
from test_periodic_status import Router, STATUS_SCHEMA, WEATHER_SCHEMA, validate_status

HEADER = struct.Struct("!4sBBHQQIIHHHH")
PAYLOAD_BYTES = 1160


class BackupCapture:
    """Independent MTRU decoder, preserving transfer and JSON identities."""
    def __init__(self):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.settimeout(.05)
        self.port = self.socket.getsockname()[1]
        self.messages, self.errors, self.parts = [], [], {}
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def run(self):
        try:
            while not self.stop.is_set():
                try:
                    data, peer = self.socket.recvfrom(65536)
                except socket.timeout:
                    continue
                assert HEADER.size == 40 and 40 < len(data) <= 1200, len(data)
                magic, version, kind, size, nonce, seq, total, offset, index, count, length, reserved = HEADER.unpack_from(data)
                assert (magic, version, kind, size, reserved) == (b"MTRU", 1, 0, 40, 0)
                assert 0 < total <= 2 * 1024 * 1024 and count == (total + PAYLOAD_BYTES - 1) // PAYLOAD_BYTES
                assert index < count and offset == index * PAYLOAD_BYTES
                assert length == min(PAYLOAD_BYTES, total - offset) and len(data) == size + length
                key = peer, nonce, seq
                assembly = self.parts.setdefault(key, (total, count, {}))
                assert assembly[:2] == (total, count), "transfer geometry changed"
                fragments = assembly[2]
                assert index not in fragments or fragments[index] == data[size:], "conflicting duplicate"
                fragments[index] = data[size:]
                if len(fragments) == count:
                    body = b"".join(fragments[i] for i in range(count))
                    assert len(body) == total
                    self.messages.append((time.monotonic(), key, json.loads(body)))
                    del self.parts[key]
        except BaseException as error:
            self.errors.append(repr(error))

    def __exit__(self, *unused):
        self.stop.set()
        self.thread.join(timeout=1)
        self.socket.close()
        assert not self.thread.is_alive() and not self.errors, self.errors


class HTTPFailureSink:
    def __init__(self, recover_after=0, first_delay=0):
        self.requests, self.errors = [], []
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.recover_after, self.first_delay = recover_after, first_delay
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
                    assert self.path == "/ingest"
                    length = int(self.headers["Content-Length"])
                    assert 0 < length <= 2 * 1024 * 1024
                    body = json.loads(self.rfile.read(length))
                    received = time.monotonic()
                    code = 500 if received - owner.started < owner.recover_after else 204
                    with owner.lock:
                        first = not owner.requests
                        owner.requests.append((received, code, body))
                    if first:
                        owner.stop.wait(owner.first_delay)
                    self.send_response(code)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                except (ConnectionResetError, BrokenPipeError):
                    pass
                except BaseException as error:
                    owner.errors.append(repr(error))

            def log_message(self, *unused):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_port
        self.thread = threading.Thread(target=lambda: self.server.serve_forever(.025), daemon=True)

    def __enter__(self):
        self.started = time.monotonic()
        self.thread.start()
        return self

    def __exit__(self, *unused):
        self.stop.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=1)
        assert not self.thread.is_alive() and not self.errors, self.errors


class Sensor:
    def __init__(self, port):
        self.port = port
        self.stop = threading.Event()
        self.errors, self.sent = [], []
        self.thread = threading.Thread(target=self.run, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def run(self):
        vehicle = Wire()
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
                # Give the test-owned UDP receiver time to bind before sample0.
                if self.stop.wait(.35):
                    return
                started = time.monotonic()
                index = 0
                while not self.stop.is_set():
                    boot_ms = 1000 + index * 1000
                    temperature, humidity = 20 + index * .25, 40 + index * .5
                    gps = bytearray(52)
                    struct.pack_into("<Q", gps, 0, (boot_ms - 50) * 1000)
                    gps[28] = 3
                    estimator = struct.pack("<Q8fH", (boot_ms - 50) * 1000, *([0.] * 8), 54)
                    packet = (vehicle.heartbeat() + vehicle.navigation(boot_ms - 50, index) +
                              vehicle.packet(24, gps, 24) + vehicle.packet(230, estimator, 163) +
                              atmos_packet(vehicle, boot_ms * 1000, temperature, humidity) +
                              vehicle.navigation(boot_ms + 50, index + 1))
                    source.sendto(packet, ("127.0.0.1", self.port))
                    self.sent.append((time.monotonic(), index))
                    index += 1
                    self.stop.wait(max(0, started + index - time.monotonic()))
        except BaseException as error:
            self.errors.append(repr(error))

    def __exit__(self, *unused):
        self.stop.set()
        self.thread.join(timeout=1)
        assert not self.thread.is_alive() and not self.errors, self.errors


def finish(router, timeout=35):
    try:
        stdout, stderr = router.child.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        # Retain bounded diagnostics if a future sender regression stalls EOF.
        diagnostic = (error.stderr or b"")[-6000:].decode("utf-8", "replace")
        raise AssertionError(("router did not finish HTTP drain", timeout, diagnostic)) from error
    assert router.child.returncode in (0, 2) and not stdout, (router.child.returncode, stdout[-1000:], stderr[-3000:])
    assert router.config.read_text(encoding="utf-8") == router.original
    return stderr.decode("utf-8", "replace")


def weather_records(body):
    if body.get("schema") == WEATHER_SCHEMA:
        yield from body["records"]
    elif body.get("schema") == "mavlink_router_bundle_v1":
        # Mixed batches are not expected for this weather-only fixture.
        raise AssertionError("unexpected mixed envelope in a weather-only fixture")


def identity(record):
    return record["session"], record["generation"], record["seq"]


def samples(record):
    for metric, groups in record["weather"]["measurements"].items():
        for group in groups:
            assert group["message_id"] == 8009
            for sample in group["samples"]:
                yield metric, sample


def failure_case(executable, directory):
    with BackupCapture() as udp, HTTPFailureSink(recover_after=13, first_delay=3) as http:
        started = time.monotonic()
        router = Router(executable, directory, http, "failure", duration_ms=20000,
                        link_timeout_ms=1800, idle_warn_ms=1500, status_interval_ms=1000,
                        connect_timeout_ms=200, io_timeout_ms=4000, spatial_enabled=0,
                        udp_backup_enabled=1, udp_backup_host="127.0.0.1", udp_backup_port=udp.port)
        try:
            with Sensor(router.port) as source:
                log = finish(router)
            runtime = time.monotonic() - started
            assert not udp.errors and not http.errors, (udp.errors, http.errors)
            drops = re.search(r"queue_windows_dropped=(\d+)", log)
            assert drops and int(drops[1]) == 0, ("default RAM buffer lost unacknowledged measurements", log[-3000:])
            udp_weather = [(when, record) for when, _, body in udp.messages for record in weather_records(body)]
            statuses = [(when, body["records"][0]) for when, _, body in udp.messages if body.get("schema") == STATUS_SCHEMA]
            assert len(udp_weather) >= 15 and len(statuses) >= 15, (len(udp_weather), len(statuses), log[-2000:])
            before_recovery = [(when, record) for when, record in udp_weather if when < http.started + 13]
            assert len(before_recovery) >= 10, "HTTP failure stalled UDP samples"
            status_before = [(when, record) for when, record in statuses if when < http.started + 13]
            assert len(status_before) >= 10, "HTTP failure stalled independent UDP status"
            for _, record in statuses:
                validate_status(record)
            assert max(b[0] - a[0] for a, b in zip(statuses, statuses[1:])) < 2.2, "UDP status waited on HTTP"
            first_http = http.requests[0][0]
            assert sum(first_http < when < first_http + 2.8 for when, _ in udp_weather) >= 2, "HTTP no-response period blocked UDP"

            by_id = {}
            observed = {}
            for when, record in udp_weather:
                key = identity(record)
                assert key not in by_id, ("UDP replayed a historical measurement", key)
                by_id[key] = record
                for metric, sample in samples(record):
                    stamp = int(sample["t_us"])
                    index = (stamp - 1_000_000) // 1_000_000
                    assert 0 <= index < len(source.sent), (index, len(source.sent))
                    expected = 293.15 + index * .25 if metric == "temperature" else (40 + index * .5) / 100
                    assert math.isclose(sample["value"], expected, rel_tol=1e-12), sample
                    sample_id = metric, stamp
                    assert sample_id not in observed, ("UDP manufactured duplicate observations", sample_id)
                    observed[sample_id] = when
            assert max(stamp for metric, stamp in observed if metric == "temperature") >= 18_000_000, "UDP never reached recent sensor data"
            late_during_failure = [sample for when, record in before_recovery for metric, sample in samples(record)
                                   if metric == "temperature" and int(sample["t_us"]) >= 11_000_000]
            assert late_during_failure, "HTTP backlog prevented new UDP values"

            failed, confirmed = {}, {}
            for when, code, body in http.requests:
                for record in weather_records(body):
                    key = identity(record)
                    assert key in by_id, ("HTTP and UDP did not share record identities", key)
                    assert record == by_id[key], ("HTTP retry and UDP changed the same record", key)
                    (confirmed if code == 204 else failed).setdefault(key, []).append(when)
            assert failed and confirmed and set(failed) & set(confirmed), "failed HTTP head was not retained until a real204"
            assert any(len(attempts) >= 2 for attempts in failed.values()), "UDP success incorrectly acknowledged the HTTP queue"
            retained = {identity(record) for _, record in before_recovery}
            assert len(retained) > 8, "fixture did not exceed the former eight-record queue"
            assert retained <= set(confirmed), ("HTTP failed to recover every pre-recovery record", retained - set(confirmed))
            assert set(by_id) == set(confirmed), ("HTTP shutdown lost buffered measurements", set(by_id) - set(confirmed))
            print(f"PASS UDP continues through HTTP no-response/500; RAM retains more than 8 records, shared immutable IDs, real 204 queue release; runtime={runtime:.2f}s")
        finally:
            router.close()


def independent_case(executable, directory, enabled):
    with BackupCapture() as udp, HTTPFailureSink() as http:
        if enabled:
            # Reserve and release a separate private endpoint with no receiver.
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
        else:
            port = udp.port
        options = {"udp_backup_host": "127.0.0.1", "udp_backup_port": port}
        if enabled:
            options["udp_backup_enabled"] = 1
        router = Router(executable, directory, http, "dead-udp" if enabled else "disabled",
                        duration_ms=2400, link_timeout_ms=1800, status_interval_ms=0,
                        spatial_enabled=0, connect_timeout_ms=200, io_timeout_ms=500, **options)
        try:
            with Sensor(router.port):
                finish(router, timeout=7)
            records = [record for _, code, body in http.requests if code == 204 for record in weather_records(body)]
            assert records and router.child.returncode == 0, "UDP configuration stopped healthy HTTP"
            assert not udp.messages, "default-disabled backup emitted UDP" if not enabled else "UDP reached the wrong endpoint"
            print("PASS dead UDP endpoint leaves HTTP running" if enabled else "PASS UDP backup is disabled by default")
        finally:
            router.close()


if __name__ == "__main__":
    executable = Path(sys.argv[1]).resolve()
    assert executable.is_file(), executable
    with tempfile.TemporaryDirectory(prefix="router-udp-backup-") as temporary:
        directory = Path(temporary)
        with ThreadPoolExecutor(max_workers=3) as executor:
            jobs = [executor.submit(failure_case, executable, directory),
                    executor.submit(independent_case, executable, directory, False),
                    executor.submit(independent_case, executable, directory, True)]
            for job in jobs:
                job.result()
