"""Standard/local-extended HYGROMETER -> real router -> TCP HTTP confirmation.

Only test-owned loopback sockets and temporary config files are used. Samples,
HTTP bodies and server log output stay in memory; no observation file is made.
Usage: python test_hygrometer_router.py ROUTER_EXE [TEST_SERVER_EXE]
"""
from copy import deepcopy
import json
import math
from pathlib import Path
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time

from test_mavlink_router import Wire
from test_test_stream import CaptureServer, record_bytes, request, response_status

SCHEMA = "mavlink_weather_v1"
SAMPLE_KEYS = {"t_us", "position_m", "velocity_mps", "value", "measurement_variance",
               "time_uncertainty_us", "navigation_uncertainty_m", "position_uncertainty_m",
               "quality", "seq"}


def hygrometer(wire, index, sensor, extended, invalid=False, bounded_cycle=False):
    temperature = 2000 + 1000 * sensor + index * 10
    humidity_index = index % 20 if bounded_cycle else index
    humidity = (2500 + humidity_index * 100) if not sensor else (6500 - humidity_index * 100)
    if invalid:
        humidity = 10001
    payload = struct.pack("<hHB", temperature, humidity, sensor)
    if extended:
        payload += struct.pack("<QII", 1_000_000 + index * 100_000,
                               0x01020304 if not sensor else 0x05060708,
                               1 if not sensor else 100)
        # MAVLink 2 omits trailing zero bytes, including part of extension words.
        payload = payload.rstrip(b"\0")
    return wire.packet(12920, payload, 20)


def validate_samples(record, extended, expected, actual):
    assert record["frame"] == "ENU" and record["origin_wgs84"] == [37, 127, 100], record
    assert record["time_basis"] == "pixhawk_boot_us", record
    assert record["host_time_basis"] == "collector_monotonic_ms", record
    assert record["source"] == {"sysid": 1, "compid": 1}, record
    assert type(record["seq"]) is int and record["seq"] > 0, record
    assert type(record["generation"]) is int and record["generation"] > 0, record
    assert record["session"].startswith("weather-test."), record
    weather = record["weather"]
    assert weather["si_registry"] == 1 and isinstance(weather["host_window_ms"], str), weather
    assert int(weather["host_window_ms"]) > 0, weather
    assert 0 < weather["n"] == weather["raw_n"] <= 128 and weather["raw_dropped"] == 0, weather
    assert set(weather["measurements"]) == {"temperature", "humidity"}, weather
    total = 0
    for metric, groups in weather["measurements"].items():
        identities = set()
        for group in groups:
            sensor = group["sensor_instance"]
            assert sensor in (0, 1), group
            sensor_id = (0x01020304 if not sensor else 0x05060708) if extended else 0
            assert group["sensor_id"] == sensor_id, group
            assert (sensor_id, sensor) not in identities, group
            identities.add((sensor_id, sensor))
            samples, stats = group["samples"], group["statistics"]
            assert group["raw_n"] == len(samples) == stats["n"] and group["raw_dropped"] == 0, group
            assert samples and group["representative"] in samples, group
            values = [sample["value"] for sample in samples]
            assert math.isclose(stats["mean"], statistics.mean(values), rel_tol=1e-13, abs_tol=1e-13), group
            assert stats["min"] == min(values) and stats["max"] == max(values), group
            if len(samples) > 1:
                assert math.isclose(stats["variance"], statistics.variance(values), rel_tol=1e-10, abs_tol=1e-13), group
                assert group["representative"]["value"] != stats["mean"], "window mean was attached to sample location"
            else:
                assert stats["variance"] is None, group
            assert stats["time_known_n"] == (len(samples) if extended else 0), group
            if extended:
                stamps = [int(sample["t_us"]) for sample in samples]
                assert int(stats["t_start_us"]) == min(stamps) and int(stats["t_end_us"]) == max(stamps), group
            else:
                assert stats["t_start_us"] is None and stats["t_end_us"] is None, group
            for sample in samples:
                assert set(sample) == SAMPLE_KEYS, sample
                assert sample["measurement_variance"] is None, "sample variance fabricated from window variance"
                assert sample["navigation_uncertainty_m"] is None and sample["position_uncertainty_m"] is None, sample
                if extended:
                    index = (int(sample["t_us"]) - 1_000_000) // 100_000
                elif metric == "temperature":
                    index = round((sample["value"] - 273.15 - 20 - sensor * 10) * 10)
                else:
                    index = round((sample["value"] - .25) * 100) if not sensor else round((.65 - sample["value"]) * 100)
                key = metric, sensor, index
                assert key in expected and key not in actual, (key, sample)
                assert math.isclose(sample["value"], expected[key], rel_tol=1e-13, abs_tol=1e-12), (key, sample, expected[key])
                actual[key] = sample
                if extended:
                    assert sample["t_us"] == str(1_000_000 + index * 100_000), sample
                    assert sample["time_uncertainty_us"] == (1 if not sensor else 100), sample
                    assert sample["velocity_mps"] == [2, 1, .5], sample
                    sine = math.sin(math.radians(37))
                    meridian = 6378137 * (1 - .0066943799901413165) / (1 - .0066943799901413165 * sine * sine) ** 1.5
                    delta = ((370_000_000 + index * 2 + 1) * 1e-7 - 37) * math.pi / 180
                    assert len(sample["position_m"]) == 3 and sample["position_m"][0] == sample["position_m"][2] == 0, sample
                    assert math.isclose(sample["position_m"][1], delta * meridian, abs_tol=1e-8), sample
                    assert not sample["quality"] & 0x0C000000, sample
                else:
                    assert all(sample[key] is None for key in ("t_us", "position_m", "velocity_mps", "time_uncertainty_us")), sample
                    assert sample["quality"] & 0x0C000000 == 0x0C000000, sample
            total += len(samples)
    assert total == weather["n"], weather


def validate_mixed_requests(capture, expected, stderr):
    known, acknowledged, weather_records, telemetry_batches = {}, set(), [], []
    mixed_seen = False
    for ordinal, (host_time, status, body, root) in enumerate(capture.requests):
        if ordinal:
            assert host_time - capture.requests[ordinal - 1][0] >= .90, "mixed HTTP exceeded configured 1 Hz"
        if root["schema"] == "mavlink_router_bundle_v1":
            mixed_seen = True
            assert 1 <= len(root["records"]) <= 4, root
            payloads = []
            for entry in root["records"]:
                assert entry["kind"] in ("weather", "telemetry"), entry
                expected_schema = SCHEMA if entry["kind"] == "weather" else 3
                assert entry["payload"]["schema"] == expected_schema, entry
                payloads.append(entry["payload"])
        else:
            payloads = [root]
        for payload in payloads:
            assert payload["stream"] == "weather-test", payload
            if payload["schema"] == SCHEMA:
                entries = [(('weather', item["session"], item["seq"]), item) for item in payload["records"]]
            else:
                assert payload["schema"] == 3, payload
                assert all(int(value) == 0 for value in payload["loss"].values()), payload["loss"]
                entries = [(('telemetry', payload["session"], item["batch_id"]), item) for item in payload["batches"]]
            for identity, item in entries:
                assert identity not in acknowledged, ("mixed confirmed batch sent again", identity)
                if identity in known:
                    assert known[identity] == item, ("mixed retry changed immutable batch", identity)
                else:
                    known[identity] = item
                if status == 204:
                    acknowledged.add(identity)
                    (weather_records if identity[0] == 'weather' else telemetry_batches).append(item)
    assert mixed_seen, "concurrent weather/telemetry were never batched into one HTTP request"
    assert set(known) == acknowledged, "mixed healthy 204 server left unconfirmed data"
    assert capture.requests[0][1] == 202 and capture.requests[1][1] == 204, capture.requests[:2]
    actual = {}
    for record in weather_records:
        validate_samples(record, True, expected, actual)
    assert actual.keys() == expected.keys(), ("mixed weather loss", len(actual), len(expected), stderr[-3000:])
    legacy = {}
    for batch in telemetry_batches:
        assert batch["n"] == batch["raw_n"] == len(batch["raw"]) and batch["raw_dropped"] == 0, batch
        for row in batch["raw"]:
            assert row[3] == 'reference', row
            index = (int(row[0]) - 1_000_000) // 100_000
            assert 0 <= index < 100 and index not in legacy and row[11] == 400 + index, row
            legacy[index] = row
    assert set(legacy) == set(range(100)), ("mixed legacy loss", len(legacy), stderr[-3000:])
    assert b"queue_windows_dropped=0 queue_samples_dropped=0 pending=0" in stderr, stderr
    print("Normal mixed mode: 400 weather + 100 legacy samples, 1 Hz typed bundle, bounded queue/no drops, 202 retry/204 release: PASS")


def client_case(executable, directory, extended, normal=False):
    with CaptureServer(retry=True) as capture:
        reservation = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
        settings = {
            "url": f"http://127.0.0.1:{capture.port}/ingest", "stream": "weather-test",
            "session": "weather-test", "input_mode": "mavlink", "serial_port": "none",
            "auto_scan": 0, "websocket_url": "", "udp_enabled": 1, "udp_bind": "127.0.0.1",
            "udp_port": port, "probe_ms": 1000, "link_timeout_ms": 1000, "reconnect_ms": 100,
            "duration_ms": 14500 if normal else 4600, "send_interval_ms": 1000, "connect_timeout_ms": 500,
            "io_timeout_ms": 500, "refresh_ms": 250, "origin_auto": 0,
            "origin_lat": 37, "origin_lon": 127, "origin_alt": 100,
            "max_windows": 1 if normal else 4,
        }
        config = directory / ("mixed.ini" if normal else "extended.ini" if extended else "standard.ini")
        original = "".join(f"{key}={value}\n" for key, value in settings.items())
        config.write_text(original, encoding="utf-8")
        arguments = [str(executable), "--config", str(config)]
        if not normal:
            arguments += ["--test-weather", "--server-url", f"http://127.0.0.1:{capture.port}/ingest"]
        child = subprocess.Popen(arguments,
                                 cwd=directory, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        reservation.close()
        stop = threading.Event()
        errors, expected = [], {}

        def produce():
            try:
                wire = Wire()
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
                    source.bind(("127.0.0.1", 0))
                    source.settimeout(.01)
                    # Allow the normal passive bind/probe path to identify its
                    # heartbeat before observations start; UDP has no handshake.
                    for _ in range(8):
                        source.sendto(wire.heartbeat(), ("127.0.0.1", port))
                        stop.wait(.04)
                    for index in range(122 if normal else 42):
                        if stop.is_set():
                            break
                        payload = wire.heartbeat()
                        boot_ms = 1000 + index * 100
                        if extended:
                            payload += wire.navigation(boot_ms - 50, index * 2)
                        if index < (100 if normal else 20):
                            for sensor in (0, 1):
                                payload += hygrometer(wire, index, sensor, extended, bounded_cycle=normal)
                                expected[("temperature", sensor, index)] = (2000 + sensor * 1000 + index * 10) * .01 + 273.15
                                expected[("humidity", sensor, index)] = ((2500 + (index % 20) * 100) if not sensor else (6500 - (index % 20) * 100)) * .0001
                            # Out-of-range humidity must reject the whole pair.
                            if not normal:
                                payload += hygrometer(wire, index, 0, extended, invalid=True)
                            else:
                                payload += wire.named(boot_ms, "reference", 400 + index)
                        if extended:
                            payload += wire.navigation(boot_ms + 50, index * 2 + 2)
                        source.sendto(payload, ("127.0.0.1", port))
                        try:
                            response = source.recvfrom(65535)
                            raise AssertionError(("passive router transmitted to MAVLink source", response))
                        except (socket.timeout, ConnectionResetError, ConnectionRefusedError):
                            pass
                        stop.wait(.09)
            except BaseException as error:
                errors.append(repr(error))

        producer = threading.Thread(target=produce, daemon=True)
        producer.start()
        try:
            stdout, stderr = child.communicate(timeout=22 if normal else 12)
        finally:
            stop.set(); producer.join(timeout=2)
            if child.poll() is None:
                child.kill(); child.wait(timeout=2)
        assert child.returncode == 0 and not stdout, (child.returncode, stdout[-2000:], stderr[-5000:])
        assert not producer.is_alive() and not errors and not capture.errors, (errors, capture.errors)
        assert (normal or b"TEST MODE" in stderr) and b"\x1b" not in stderr, stderr
        assert config.read_text(encoding="utf-8") == original, "CLI mode modified saved config"
        assert len(capture.requests) >= 3, (capture.requests, stderr[-4000:])
        if normal:
            validate_mixed_requests(capture, expected, stderr)
            return None
        lexical, acknowledged, records = {}, set(), []
        for position, (host_time, status, body, payload) in enumerate(capture.requests):
            assert payload["schema"] == SCHEMA and payload["stream"] == "weather-test", payload
            assert len(payload["records"]) == 1, payload
            raw = record_bytes(body)
            if position:
                assert host_time - capture.requests[position - 1][0] >= .90, "HTTP starts exceeded configured 1 Hz"
            for record in payload["records"]:
                identity = record["session"], record["seq"]
                assert identity not in acknowledged, ("confirmed record sent again", identity)
                if identity in lexical:
                    assert lexical[identity] == raw[identity], ("202 changed immutable weather record", identity)
                else:
                    lexical[identity] = raw[identity]
                if status == 204:
                    acknowledged.add(identity); records.append(record)
        assert capture.requests[0][1] == 202 and capture.requests[1][1] == 204, capture.requests[:2]
        assert record_bytes(capture.requests[0][2]) == record_bytes(capture.requests[1][2]), "202 batch forgotten/recomputed"
        assert set(lexical) == acknowledged, "weather remained unconfirmed after healthy responses"
        actual = {}
        for record in records:
            validate_samples(record, extended, expected, actual)
        assert actual.keys() == expected.keys(), ("missing samples", len(actual), len(expected), sorted(expected.keys() - actual.keys()), stderr[-3000:])
        print(f"HYGROMETER {'extended timed/location' if extended else 'standard time/location unknown'}: {len(actual)} SI samples; separate sensors, independent statistics, 202 immutable retry and 204 release: PASS")
        return records[0]


def server_case(executable, directory, example):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
        reservation.bind(("127.0.0.1", 0)); port = reservation.getsockname()[1]
    child = subprocess.Popen([str(executable), "--port", str(port), "--plain", "--duration-ms", "2500"],
                             cwd=directory, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    output = []
    def drain_output():
        while True:
            part = child.stdout.read(4096)
            if not part:
                return
            output.append(part)
    consumer = threading.Thread(target=drain_output, daemon=True)
    consumer.start()
    valid = {"schema": SCHEMA, "stream": "weather-test", "records": [example]}
    encoded = json.dumps(valid, separators=(",", ":")).encode("utf-8")
    try:
        until = time.monotonic() + 2
        while True:
            assert child.poll() is None, child.communicate()
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.05):
                    break
            except (ConnectionRefusedError, socket.timeout):
                assert time.monotonic() < until, "C weather server did not listen"
                time.sleep(.025)
        assert response_status(port, request(encoded), fragments=True) == 204
        invalid = []
        for mutate in (
            lambda item: item["weather"].update(si_registry=999),
            lambda item: item["weather"].update(raw_n=129),
            lambda item: item["source"].update(sysid=256),
            lambda item: item.update(time_basis="receive_time"),
            lambda item: item["weather"]["measurements"]["humidity"][0]["representative"].update(value=65.0),
            lambda item: item["weather"]["measurements"]["temperature"][0]["statistics"].update(n=0),
        ):
            payload = deepcopy(valid); mutate(payload["records"][0]); invalid.append(json.dumps(payload).encode("utf-8"))
        for body in invalid + [b"\0" + encoded, b'{"schema":"mavlink_weather_v1","records":[]}']:
            assert response_status(port, request(body)) != 204, body[:1000]
        # Dispatch by the validated envelope schema, not by an optional field
        # name. Unknown future heartbeat fields must never select another parser.
        heartbeat = {"seq": 1, "source_generation": 1, "session": "extra-fields",
                     "source": "udp://127.0.0.1:34567", "heartbeat": {"sysid": 1,
                     "compid": 1, "type": 2, "autopilot": 12, "base_mode": 128,
                     "custom_mode": 0, "system_status": 4, "received_monotonic_ms": 100},
                     "position": None, "coordinate_age_ms": None, "weather": {"unrelated": []}}
        hb_body = json.dumps({"schema": "mavlink_router_test_v1", "stream": "test",
                              "records": [heartbeat]}).encode("utf-8")
        assert response_status(port, request(hb_body)) == 204
        # Rejection must not poison the next accepted request.
        assert response_status(port, request(encoded)) == 204
        child.wait(timeout=6)
        consumer.join(timeout=2)
        assert not consumer.is_alive(), "server log reader did not finish"
        stdout, stderr = b"".join(output), child.stderr.read()
        assert child.returncode == 0 and b"\x1b" not in stdout + stderr, (stdout[-1000:], stderr)
        printed = [json.loads(line) for line in stdout.decode("utf-8").splitlines()]
        assert printed == [example, heartbeat, example], (printed, stderr)
        print("C TCP weather server: validates SI/time/location/statistics, rejects malformed/range packets, prints full records in RAM: PASS")
    finally:
        if child.poll() is None:
            child.kill(); child.wait(timeout=2)


def main():
    assert 2 <= len(sys.argv) <= 3, __doc__
    router = Path(sys.argv[1]).resolve()
    server = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
    assert router.is_file(), router
    with tempfile.TemporaryDirectory(prefix="hygrometer-router-test-") as temporary:
        directory = Path(temporary)
        executable = directory / router.name; shutil.copy2(router, executable)
        client_case(executable, directory, False)
        record = client_case(executable, directory, True)
        if server is not None:
            copied_server = directory / server.name; shutil.copy2(server, copied_server)
            server_case(copied_server, directory, record)
        client_case(executable, directory, True, normal=True)


if __name__ == "__main__":
    main()
