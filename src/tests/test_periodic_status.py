"""Independent HTTP status cadence and honest vehicle/sensor liveness.

Only private loopback sockets, disposable configs, and synthetic MAVLink are
used. Captured HTTP bodies and observations remain in memory. No real device,
production configuration, application window, or telemetry file is accessed.
"""
from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
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
from test_test_stream import request, response_status

STATUS_SCHEMA = "mavlink_router_status_v1"
WEATHER_SCHEMA = "mavlink_weather_v1"


class Capture:
    """Fail status independently from sample ACKs; keep actual wire in RAM."""
    def __init__(self, reject_status=False, retry_weather=False):
        self.requests, self.errors = [], []
        self.changed = threading.Condition()
        self.weather_requests = self.status_requests = 0
        self.awaiting_status_ack = False
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
                    raw = self.rfile.read(length)
                    body = json.loads(raw)
                    code = 204
                    with owner.changed:
                        if body["schema"] == STATUS_SCHEMA:
                            owner.status_requests += 1
                            # Make ACK isolation a causal sequence, independent
                            # of which periodic status slot meets the retry:
                            # reject weather -> ACK status -> ACK weather.
                            if owner.awaiting_status_ack:
                                owner.awaiting_status_ack = False
                            elif reject_status and owner.status_requests % 3:
                                code = 500
                        else:
                            if retry_weather and not owner.weather_requests:
                                owner.awaiting_status_ack = True
                            if owner.awaiting_status_ack:
                                code = 202
                            owner.weather_requests += 1
                        owner.requests.append((time.monotonic(), code, raw, body))
                        owner.changed.notify_all()
                    self.send_response(code)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                except (ConnectionResetError, BrokenPipeError):
                    pass
                except BaseException as error:
                    with owner.changed:
                        owner.errors.append(repr(error))
                        owner.changed.notify_all()
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
        assert not self.thread.is_alive() and not self.errors, self.errors

    def status(self):
        return [(received, code, body["records"][0])
                for received, code, _, body in self.requests if body["schema"] == STATUS_SCHEMA]

    def wait_status(self, predicate, after=0, timeout=3):
        deadline = time.monotonic() + timeout
        with self.changed:
            while True:
                for item in self.status()[after:]:
                    if predicate(item[2]):
                        return item[2]
                remaining = deadline - time.monotonic()
                assert remaining > 0 and not self.errors, (self.errors, self.status()[after:])
                self.changed.wait(min(remaining, .05))


class Router:
    def __init__(self, executable, directory, capture, name, extra_args=(), **overrides):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
            reservation.bind(("127.0.0.1", 0))
            self.port = reservation.getsockname()[1]
        settings = {
            "url": f"http://127.0.0.1:{capture.port}/ingest",
            "stream": "periodic-status", "session": "periodic-status",
            "input_mode": "mavlink", "serial_port": "none", "auto_scan": 0,
            "websocket_url": "", "udp_enabled": 1, "udp_bind": "127.0.0.1",
            "udp_port": self.port, "qgc_enabled": 0, "target_system": 1,
            "target_component": 1, "probe_ms": 1000, "link_timeout_ms": 350,
            "idle_warn_ms": 500, "reconnect_ms": 100, "refresh_ms": 250,
            "duration_ms": 11000, "send_interval_ms": 1000,
            "connect_timeout_ms": 300, "io_timeout_ms": 300,
            "origin_auto": 0, "origin_lat": 37, "origin_lon": 127,
            "origin_alt": 100, "max_windows": 4,
        }
        settings.update(overrides)
        self.config = directory / f"{name}.ini"
        self.original = "".join(f"{key}={value}\n" for key, value in settings.items())
        self.config.write_text(self.original, encoding="utf-8")
        self.child = subprocess.Popen([str(executable), "--config", str(self.config), *extra_args],
                                      cwd=directory, stdin=subprocess.DEVNULL,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def finish(self, timeout=15):
        stdout, stderr = self.child.communicate(timeout=timeout)
        assert self.child.returncode == 0 and not stdout, (self.child.returncode, stdout[-2000:], stderr[-5000:])
        assert self.config.read_text(encoding="utf-8") == self.original, "runtime edited saved settings"
        return stderr

    def close(self):
        if self.child.poll() is None:
            self.child.kill()
        self.child.communicate(timeout=3)


def decimal(value, nullable=False):
    if nullable and value is None:
        return
    assert isinstance(value, str) and value.isascii() and value.isdecimal(), value
    assert str(int(value)) == value and 0 <= int(value) < 2**64, value


def validate_status(record):
    assert record["sample_count"] == 0 and isinstance(record["session"], str), record
    for key in ("seq", "generation", "reported_monotonic_ms", "run_elapsed_ms"):
        decimal(record[key])
    assert int(record["reported_monotonic_ms"]) >= int(record["run_elapsed_ms"]), record
    source = record["source"]
    assert type(source["connected"]) is bool and isinstance(source["endpoint"], str), source
    for link in [source] + record["devices"]:
        assert 0 <= link["sysid"] <= 255 and 0 <= link["compid"] <= 255, link
        for state, age, states in (("heartbeat", "hb_age_ms", ("WAIT", "LIVE", "LOST")),
                                   ("data", "data_age_ms", ("WAIT", "LIVE", "STALE"))):
            assert link[state] in states, link
            decimal(link[age], nullable=True)
            assert (link[age] is None) == (link[state] == "WAIT"), link
    for device in record["devices"]:
        decimal(device["rx_age_ms"])
    for name, unit in (("temperature", "K"), ("humidity", "1")):
        metric = record["weather"][name]
        decimal(metric["observations"])
        decimal(metric["rx_age_ms"], nullable=True)
        assert metric["unit"] == unit and metric["state"] in ("UNKNOWN", "FRESH", "STALE"), metric
        if metric["state"] == "UNKNOWN":
            assert metric["value"] is None and metric["rx_age_ms"] is None and metric["observations"] == "0", metric
        else:
            assert metric["value"] is not None and int(metric["observations"]) > 0, metric
    for key in ("received", "estimated_lost", "duplicates"):
        decimal(record["packets"][key])


def cadence_case(executable, directory):
    with Capture() as capture:
        router = Router(executable, directory, capture, "ten-second", duration_ms=21200,
                        status_interval_ms=10000, send_interval_ms=60000)
        try:
            router.finish(timeout=25)
            statuses = capture.status()
            assert len(statuses) >= 2, ("no-input status waited for measurements or 60-second sender", statuses)
            for _, code, record in statuses:
                validate_status(record)
                assert code == 204 and not record["source"]["connected"], record
                assert record["source"]["heartbeat"] == "WAIT", record
                assert record["weather"]["temperature"]["state"] == "UNKNOWN", record
            for previous, current in zip(statuses, statuses[1:]):
                elapsed = current[0] - previous[0]
                assert 9.7 <= elapsed <= 10.8, ("10-second cadence", elapsed)
            assert len(capture.requests) == len(statuses), "status manufactured a sample batch"
            print("PASS actual 10-second no-input status, independent of 60-second measurement interval")
            return deepcopy(capture.requests[-1][3])
        finally:
            router.close()


def liveness_case(executable, directory):
    with Capture(reject_status=True, retry_weather=True) as capture:
        router = Router(executable, directory, capture, "liveness", status_interval_ms=1000, duration_ms=18000)
        stop = threading.Event()
        phase = "quiet"
        expected, producer_errors = {}, []
        vehicle, gcs = Wire(), Wire(255)

        def produce():
            nonlocal phase
            index, previous_phase = 0, "quiet"
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
                    while not stop.is_set():
                        active = phase
                        payload = b""
                        if active != "quiet":
                            payload += gcs.packet(0, struct.pack("<IBBBBB", 0, 6, 8, 0, 4, 3), 50, component=190)
                        if active in ("weather", "data"):
                            boot_ms = 1000 + index * 100
                            if previous_phase == "weather" and active == "data":
                                vehicle.sequence[1] += 3
                            payload += vehicle.navigation(boot_ms - 50, index * 2)
                            if active == "weather":
                                heartbeat = vehicle.heartbeat()
                                payload += heartbeat + heartbeat  # Duplicate has fresh receipt, not a new sample.
                                temperature, humidity = 20 + index * .25, 40 + index * .5
                                payload += atmos_packet(vehicle, boot_ms * 1000, temperature, humidity)
                                expected[str(boot_ms * 1000)] = (temperature + 273.15, humidity / 100)
                            payload += vehicle.navigation(boot_ms + 50, index * 2 + 2)
                            index += 1
                        if payload:
                            source.sendto(payload, ("127.0.0.1", router.port))
                        previous_phase = active
                        stop.wait(.1)
            except BaseException as error:
                producer_errors.append(repr(error))

        producer = threading.Thread(target=produce, daemon=True)
        producer.start()
        try:
            initial = capture.wait_status(lambda record: record["source"]["heartbeat"] == "WAIT")
            assert not initial["source"]["connected"] and initial["sample_count"] == 0, initial
            assert initial["weather"]["temperature"]["state"] == "UNKNOWN", initial
            phase = "weather"
            fresh = capture.wait_status(lambda record: record["source"]["connected"] and
                                        record["weather"]["temperature"]["state"] == "FRESH")
            assert fresh["source"]["heartbeat"] == fresh["source"]["data"] == "LIVE", fresh
            phase = "data"
            data_only = capture.wait_status(lambda record: record["source"]["heartbeat"] == "LOST" and
                                            record["source"]["data"] == "LIVE" and
                                            record["weather"]["temperature"]["state"] == "STALE")
            assert not data_only["source"]["connected"] and data_only["source"]["state"] == "HEARTBEAT_LOST", data_only
            phase = "other"
            stale = capture.wait_status(lambda record: record["source"]["heartbeat"] == "LOST" and
                                        record["source"]["data"] == "STALE")
            later = capture.wait_status(lambda record: int(record["seq"]) > int(stale["seq"]))
            assert not stale["source"]["connected"] and not later["source"]["connected"], (stale, later)
            by_id = {(item["sysid"], item["compid"]): item for item in later["devices"]}
            assert by_id[(255, 190)]["heartbeat"] == "LIVE" and by_id[(1, 1)]["heartbeat"] == "LOST", by_id
            for metric in ("temperature", "humidity"):
                before, after = stale["weather"][metric], later["weather"][metric]
                assert before["state"] == after["state"] == "STALE", (before, after)
                assert before["value"] == after["value"] and before["observations"] == after["observations"], (before, after)
                assert int(after["rx_age_ms"]) > int(before["rx_age_ms"]), (before, after)
            packets = later["packets"]
            assert int(packets["received"]) > 0 and int(packets["estimated_lost"]) == 3 and int(packets["duplicates"]) > 0, packets
            phase = "quiet"
            silent = capture.wait_status(lambda record: int(record["seq"]) > int(later["seq"]))
            silent_later = capture.wait_status(lambda record: int(record["seq"]) > int(silent["seq"]))
            for item in (silent, silent_later):
                link = item["source"]
                assert not link["connected"] and (link["sysid"], link["compid"]) == (1, 1), item
                assert link["heartbeat"] == "LOST" and link["data"] == "STALE", item
                assert len(item["devices"]) == 2, item
            assert int(silent_later["source"]["hb_age_ms"]) > int(silent["source"]["hb_age_ms"]), (silent, silent_later)
            assert int(silent_later["weather"]["temperature"]["rx_age_ms"]) > int(silent["weather"]["temperature"]["rx_age_ms"]), (silent, silent_later)
            phase = "weather"
            recovered = capture.wait_status(lambda record: int(record["seq"]) > int(silent_later["seq"]) and
                                             record["source"]["connected"] and
                                             record["weather"]["temperature"]["state"] == "FRESH")
            assert recovered["source"]["heartbeat"] == recovered["source"]["data"] == "LIVE", recovered
            assert int(recovered["source"]["hb_age_ms"]) < 350, recovered
            assert int(recovered["weather"]["temperature"]["rx_age_ms"]) < 500, recovered
            phase = "other"
            router.finish()
            assert not producer_errors, producer_errors
            statuses = capture.status()
            for _, code, record in statuses:
                validate_status(record)
                assert code in (500, 204), code
            assert {code for _, code, _ in statuses} == {500, 204}, statuses
            # A failed status is replaced by a newly timed snapshot, never a
            # stale FIFO backlog that later pretends the disconnected FC is live.
            sequence = [int(item[2]["seq"]) for item in statuses]
            stamp = [int(item[2]["reported_monotonic_ms"]) for item in statuses]
            assert all(a < b for a, b in zip(sequence, sequence[1:])), sequence
            assert all(a < b for a, b in zip(stamp, stamp[1:])), stamp
            measurement_requests = [entry for entry in capture.requests if entry[3]["schema"] != STATUS_SCHEMA]
            assert measurement_requests and measurement_requests[0][1] == 202, measurement_requests
            # Windows monotonic() can put two ordered requests on one clock
            # tick. The capture's locked arrival order is the relevant proof.
            first_retry = next(i for i, item in enumerate(capture.requests)
                               if item[3]["schema"] != STATUS_SCHEMA and item[1] == 202)
            first_ack = next(i for i, item in enumerate(capture.requests)
                             if item[3]["schema"] != STATUS_SCHEMA and item[1] == 204)
            assert any(item[3]["schema"] == STATUS_SCHEMA and item[1] == 204
                       for item in capture.requests[first_retry + 1:first_ack]), (
                "fixture did not ACK status while a weather batch awaited ACK",
                [(item[1], item[3]["schema"]) for item in capture.requests])
            pending, confirmed, actual = {}, set(), {}
            for _, code, _, payload in measurement_requests:
                assert payload["schema"] == WEATHER_SCHEMA, payload
                for record in payload["records"]:
                    identity = record["session"], record["seq"]
                    assert identity not in confirmed, ("ACKed sample was sent again", identity)
                    if identity in pending:
                        assert pending[identity] == record, "status failure changed queued measurement"
                    pending[identity] = record
                    if code == 204:
                        confirmed.add(identity)
                        for metric, groups in record["weather"]["measurements"].items():
                            for group in groups:
                                for sample in group["samples"]:
                                    identity = metric, sample["t_us"]
                                    assert identity not in actual and sample["t_us"] in expected, sample
                                    actual[identity] = sample["value"]
            assert set(pending) == confirmed, "failed status stranded an unconfirmed weather batch"
            for stamp, values in expected.items():
                for metric, value in zip(("temperature", "humidity"), values):
                    assert math.isclose(actual[(metric, stamp)], value, rel_tol=1e-13, abs_tol=1e-12), (metric, stamp, value)
            assert len(actual) == 2 * len(expected), "periodic status fabricated sensor observations"
            print("PASS WAIT/LIVE/LOST, GCS isolation, full silence/reconnect ages, stale values, packet counts, HTTP500 status/sample ACK isolation")
        finally:
            stop.set()
            producer.join(timeout=2)
            router.close()
            assert not producer.is_alive() and not producer_errors, producer_errors


def disabled_case(executable, directory):
    with Capture() as capture:
        router = Router(executable, directory, capture, "disabled", duration_ms=1300)
        try:
            router.finish()
            assert not capture.requests, "default-disabled status posted to existing endpoints"
            print("PASS default-disabled status preserves existing receiver contract")
        finally:
            router.close()


def server_case(executable, directory, valid):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    child = subprocess.Popen([str(executable), "--bind", "127.0.0.1", "--port", str(port),
                              "--plain", "--duration-ms", "2200", "--max-requests", "100"],
                             cwd=directory, stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 2
        while True:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.05):
                    break
            except (ConnectionRefusedError, socket.timeout):
                assert child.poll() is None and time.monotonic() < deadline, "C test server did not start"
                time.sleep(.025)

        def send(body):
            return response_status(port, request(json.dumps(body, separators=(",", ":")).encode("utf-8")))

        assert send(valid) == 204, valid
        malformed = []
        for path, value in ((["sample_count"], 1), (["seq"], 1),
                            (["source", "hb_age_ms"], "0"),
                            (["source", "connected"], "false"),
                            (["weather", "temperature", "value"], 293.15),
                            (["weather", "humidity", "observations"], "1"),
                            (["packets", "received"], "18446744073709551616")):
            body = deepcopy(valid)
            field = body["records"][0]
            for part in path[:-1]:
                field = field[part]
            field[path[-1]] = value
            malformed.append(body)
        for body in malformed:
            assert send(body) != 204, ("malformed status accepted", body)
        assert send(valid) == 204, "malformed status poisoned subsequent request"
        stdout, stderr = child.communicate(timeout=5)
        assert child.returncode == 0, (stdout, stderr)
        decoded = []
        for line in stdout.decode("utf-8", errors="replace").splitlines():
            if line.startswith("{"):
                decoded.append(json.loads(line))
        assert decoded == valid["records"] * 2, (decoded, stdout, stderr)
        summary = re.search(rb"STOPPED requests=\d+ accepted=(\d+) records=(\d+) rejected=\d+ statuses=(\d+)", stderr)
        assert summary and summary.groups() == (b"2", b"0", b"2"), ("status counted as measurement", stderr)
        print("PASS C test server accepts status and rejects invalid ages/counts/types without corrupting next request")
    finally:
        if child.poll() is None:
            child.kill()
        child.communicate(timeout=3)


def terminal_case(executable, valid):
    from test_test_server_tui import Dashboard, Terminal
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    terminal = Terminal([str(executable), "--bind", "127.0.0.1", "--port", str(port),
                         "--duration-ms", "5000"])
    screen = Dashboard(terminal)
    try:
        screen.wait(lambda: "LISTENING" in screen.line(0) and "statuses=0" in screen.line(2),
                    "status dashboard ready")
        before = screen.checkpoint()
        for count in (1, 2):
            body = deepcopy(valid)
            body["records"][0]["seq"] = str(count)
            assert response_status(port, request(json.dumps(body).encode("utf-8"))) == 204
            screen.wait(lambda: "records=0" in screen.line(2) and
                        f"statuses={count}" in screen.line(2) and
                        "link=DOWN" in screen.line(3) and
                        any(f"#{count} STATUS seq={count}" in screen.line(row)
                            for row in range(6, 30)), "status shown separately from measurements")
        screen.stable_updates(before)
        print("PASS status dashboard shows disconnected state and separate counts without full redraw")
    finally:
        terminal.close()


def main():
    assert 2 <= len(sys.argv) <= 3, "usage: test_periodic_status.py ROUTER [SERVER]"
    executable = Path(sys.argv[1]).resolve()
    server = Path(sys.argv[2]).resolve() if len(sys.argv) == 3 else None
    with tempfile.TemporaryDirectory(prefix="router-periodic-status-") as temporary:
        directory = Path(temporary)
        # Independent ports/configs keep the real 10-second cadence test from
        # inflating suite runtime; they never share process or wire state.
        with ThreadPoolExecutor(max_workers=3) as workers:
            cadence = workers.submit(cadence_case, executable, directory)
            liveness = workers.submit(liveness_case, executable, directory)
            disabled = workers.submit(disabled_case, executable, directory)
            valid = cadence.result()
            liveness.result()
            disabled.result()
        if server is not None:
            server_case(server, directory, valid)
            terminal_case(server, valid)


if __name__ == "__main__":
    main()
