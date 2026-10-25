"""Official SENS_ATMOS and values-only HYGROMETER -> router -> TCP HTTP.

A single atmospheric sensor is used. SENS_ATMOS has a boot timestamp but no
hardware ID or timestamp uncertainty. HYGROMETER has no acquisition timestamp;
bytes after its official five-byte payload MUST NOT become local extensions.
Only test-owned sockets and temporary configs are used. All observations and
HTTP bodies remain in memory. Usage: python test_hygrometer_router.py ROUTER [SERVER]
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
               "quality", "seq", "position_source", "estimation_age_us"}


def values(index):
    """Quarter-degree/half-percent steps are exactly representable in float32."""
    phase = index % 20
    return 20 + index * .25, 0.0 if phase == 19 else 40 + phase * .5


def atmos_packet(wire, timestamp, temperature, humidity, component=1):
    # Official ASLUAV SENS_ATMOS 8009, CRC_EXTRA144. MAVLink 2 allows trailing
    # zero-byte omission: RH0 truncates the standard16-byte payload to12 bytes.
    payload = struct.pack("<Qff", timestamp, temperature, humidity).rstrip(b"\0")
    return wire.packet(8009, payload or b"\0", 144, component=component)


def hygrometer_packet(wire, index):
    temperature, humidity = values(index)
    humidity = 0.0 if index == 19 else 40 + index * .5
    payload = struct.pack("<hHB", round(temperature * 100), round(humidity * 100), 0)
    if index % 2:
        # Deliberately retain bytes from the abandoned local extension format.
        # A receiver must ignore all of them, even with valid bracketing NAV.
        payload += struct.pack("<QII", 1_000_000 + index * 100_000, 0x01020304, 1)
    return wire.packet(12920, payload, 20)


def validate_samples(record, mode, expected, actual):
    assert record["frame"] == "ENU" and record["origin_wgs84"] == [37, 127, 100], record
    assert record["time_basis"] == "pixhawk_boot_us" and record["host_time_basis"] == "collector_monotonic_ms", record
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
        assert 1 <= len(groups) <= (2 if mode == "coexist" else 1), groups
        messages = set()
        for group in groups:
            protocol = group["message_id"]
            assert protocol in ((8009, 12920) if mode == "coexist" else (12920,) if mode == "hygrometer" else (8009,)), group
            assert protocol not in messages, "same protocol duplicated its single sensor group"
            messages.add(protocol)
            known_time = protocol == 8009 and mode != "atmos-zero"
            known_position = known_time and mode != "atmos-no-nav"
            assert group["sensor_id"] == group["sensor_instance"] == 0, "official wire carries no hardware ID"
            samples, stats = group["samples"], group["statistics"]
            assert group["raw_n"] == len(samples) == stats["n"] and group["raw_dropped"] == 0, group
            assert samples and group["representative"] in samples, group
            raw_values = [sample["value"] for sample in samples]
            assert math.isclose(stats["mean"], statistics.mean(raw_values), rel_tol=1e-13, abs_tol=1e-13), group
            assert stats["min"] == min(raw_values) and stats["max"] == max(raw_values), group
            if len(samples) > 1:
                assert math.isclose(stats["variance"], statistics.variance(raw_values), rel_tol=1e-10, abs_tol=1e-13), group
                assert group["representative"]["value"] != stats["mean"], "window mean attached to actual sample location"
            else:
                assert stats["variance"] is None, group
            assert stats["time_known_n"] == (len(samples) if known_time else 0), group
            if known_time:
                stamps = [int(sample["t_us"]) for sample in samples]
                assert int(stats["t_start_us"]) == min(stamps) and int(stats["t_end_us"]) == max(stamps), group
            else:
                assert stats["t_start_us"] is None and stats["t_end_us"] is None, group
            for sample in samples:
                assert set(sample) == SAMPLE_KEYS, sample
                # This fixture supplies fused position but no GPS fix/status.
                # It is never promoted to a trusted GPS anchor.
                assert sample["position_source"] == ("ESTGPS" if known_position else "UNKNOWN"), sample
                assert bool(sample["quality"] & 0x10000000) == known_position, sample
                assert sample["estimation_age_us"] is None, sample
                assert all(sample[key] is None for key in ("measurement_variance", "time_uncertainty_us",
                           "navigation_uncertainty_m", "position_uncertainty_m")), "unknown uncertainty fabricated"
                if known_time:
                    index = (int(sample["t_us"]) - 1_000_000) // 100_000
                else:
                    matches = [index for (kind, message, index), value in expected.items() if kind == metric and message == protocol and
                               math.isclose(sample["value"], value, rel_tol=1e-13, abs_tol=1e-12)]
                    assert len(matches) == 1, (sample, matches)
                    index = matches[0]
                key = metric, protocol, index
                assert key in expected and key not in actual, (key, sample)
                assert math.isclose(sample["value"], expected[key], rel_tol=1e-13, abs_tol=1e-12), (key, sample, expected[key])
                actual[key] = sample
                if known_time:
                    assert sample["t_us"] == str(1_000_000 + index * 100_000), sample
                    assert not sample["quality"] & 0x08000000, sample
                else:
                    assert sample["t_us"] is None and sample["quality"] & 0x08000000, sample
                if known_position:
                    assert sample["velocity_mps"] == [2, 1, .5], sample
                    sine = math.sin(math.radians(37))
                    meridian = 6378137 * (1 - .0066943799901413165) / (1 - .0066943799901413165 * sine * sine) ** 1.5
                    delta = ((370_000_000 + index * 2 + 1) * 1e-7 - 37) * math.pi / 180
                    assert len(sample["position_m"]) == 3 and sample["position_m"][0] == sample["position_m"][2] == 0, sample
                    assert math.isclose(sample["position_m"][1], delta * meridian, abs_tol=1e-8), sample
                    assert not sample["quality"] & 0x04000000, sample
                else:
                    assert sample["position_m"] is None and sample["velocity_mps"] is None, sample
                    assert sample["quality"] & 0x04000000, sample
            total += len(samples)
    assert total == weather["n"], weather


def validate_mixed_requests(capture, expected, stderr):
    known, acknowledged, weather_records, telemetry_batches = {}, set(), [], []
    mixed_seen = False
    for ordinal, (host_time, status, body, root) in enumerate(capture.requests):
        if ordinal:
            assert host_time - capture.requests[ordinal - 1][0] >= .90, "mixed HTTP exceeded configured1Hz"
        if root["schema"] == "mavlink_router_bundle_v1":
            mixed_seen = True
            assert 1 <= len(root["records"]) <= 4, root
            payloads = []
            for entry in root["records"]:
                assert entry["kind"] in ("weather", "telemetry"), entry
                assert entry["payload"]["schema"] == (SCHEMA if entry["kind"] == "weather" else 3), entry
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
                assert identity not in acknowledged, ("confirmed mixed batch sent again", identity)
                if identity in known:
                    assert known[identity] == item, ("mixed retry changed immutable batch", identity)
                else:
                    known[identity] = item
                if status == 204:
                    acknowledged.add(identity)
                    (weather_records if identity[0] == 'weather' else telemetry_batches).append(item)
    assert mixed_seen, "concurrent atmospheric/legacy data never shared one HTTP request"
    assert set(known) == acknowledged, "healthy204 server left mixed data unconfirmed"
    assert capture.requests[0][1] == 202 and capture.requests[1][1] == 204, capture.requests[:2]
    actual = {}
    for record in weather_records:
        validate_samples(record, "atmos", expected, actual)
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
    print("Normal mixed mode: 200 atmospheric +100 legacy samples, max_windows1,1Hz bundle, no drops,202 retry/204 release: PASS")


def client_case(executable, directory, mode, normal=False):
    with CaptureServer(retry=True) as capture:
        reservation = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        reservation.bind(("127.0.0.1", 0)); port = reservation.getsockname()[1]
        settings = {
            "url": f"http://127.0.0.1:{capture.port}/ingest", "stream": "weather-test",
            "session": "weather-test", "input_mode": "mavlink", "serial_port": "none",
            "auto_scan": 0, "websocket_url": "", "udp_enabled": 1, "udp_bind": "127.0.0.1",
            "udp_port": port, "probe_ms": 1000, "link_timeout_ms": 1000, "reconnect_ms": 100,
            "duration_ms": 14500 if normal else 6600 if mode == "hygrometer" else 4600, "send_interval_ms": 1000, "connect_timeout_ms": 500,
            "io_timeout_ms": 500, "refresh_ms": 250, "origin_auto": 0,
            "origin_lat": 37, "origin_lon": 127, "origin_alt": 100, "max_windows": 1 if normal else 4,
        }
        config = directory / ("mixed.ini" if normal else f"{mode}.ini")
        original = "".join(f"{key}={value}\n" for key, value in settings.items())
        config.write_text(original, encoding="utf-8")
        arguments = [str(executable), "--config", str(config)]
        if not normal:
            arguments += ["--test-weather", "--server-url", f"http://127.0.0.1:{capture.port}/ingest"]
        child = subprocess.Popen(arguments, cwd=directory, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        reservation.close()
        stop = threading.Event(); errors, expected = [], {}

        def produce():
            try:
                wire = Wire()
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
                    source.bind(("127.0.0.1", 0)); source.settimeout(.01)
                    # UDP has no handshake. Identify the source before samples.
                    for _ in range(8):
                        source.sendto(wire.heartbeat(), ("127.0.0.1", port)); stop.wait(.04)
                    for index in range(122 if normal else 62 if mode == "hygrometer" else 42):
                        if stop.is_set(): break
                        payload = wire.heartbeat(); boot_ms = 1000 + index * 100
                        have_nav = mode != "atmos-no-nav"
                        if have_nav: payload += wire.navigation(boot_ms - 50, index * 2)
                        if index < (100 if normal else 40 if mode == "hygrometer" else 20):
                            temperature, humidity = values(index)
                            protocols = (8009, 12920) if mode == "coexist" else (12920,) if mode == "hygrometer" else (8009,)
                            for protocol in protocols:
                                if protocol == 12920:
                                    payload += hygrometer_packet(wire, index)
                                    wire_humidity = 0.0 if index == 19 else 40 + index * .5
                                else:
                                    stamp = 0 if mode == "atmos-zero" else boot_ms * 1000
                                    payload += atmos_packet(wire, stamp, temperature, humidity)
                                    wire_humidity = humidity
                                expected[("temperature", protocol, index)] = temperature + 273.15
                                expected[("humidity", protocol, index)] = wire_humidity / 100
                            if normal: payload += wire.named(boot_ms, "reference", 400 + index)
                            elif mode != "hygrometer":
                                # Reject the entire pair, and reject another
                                # component's sample rather than borrowing FCNAV.
                                payload += atmos_packet(wire, boot_ms * 1000, math.nan, humidity)
                                payload += atmos_packet(wire, boot_ms * 1000, temperature, math.nan)
                                payload += atmos_packet(wire, boot_ms * 1000, temperature, 100.01)
                                payload += atmos_packet(wire, boot_ms * 1000, temperature, -1)
                                payload += atmos_packet(wire, boot_ms * 1000, -300, humidity)
                                payload += atmos_packet(wire, 1_800_000_000_000_000, temperature, humidity)
                                payload += atmos_packet(wire, boot_ms * 1000, 99, 99, component=100)
                        if have_nav: payload += wire.navigation(boot_ms + 50, index * 2 + 2)
                        source.sendto(payload, ("127.0.0.1", port))
                        try:
                            response = source.recvfrom(65535)
                            raise AssertionError(("passive router sent to MAVLink source", response))
                        except (socket.timeout, ConnectionResetError, ConnectionRefusedError): pass
                        stop.wait(.09)
            except BaseException as error: errors.append(repr(error))

        producer = threading.Thread(target=produce, daemon=True); producer.start()
        try: stdout, stderr = child.communicate(timeout=22 if normal else 12)
        finally:
            stop.set(); producer.join(timeout=2)
            if child.poll() is None: child.kill(); child.wait(timeout=2)
        assert child.returncode == 0 and not stdout, (child.returncode, stdout[-2000:], stderr[-5000:])
        assert not producer.is_alive() and not errors and not capture.errors, (errors, capture.errors)
        assert (normal or b"TEST MODE" in stderr) and b"\x1b" not in stderr, stderr
        assert config.read_text(encoding="utf-8") == original, "CLI changed saved config"
        assert len(capture.requests) >= 3, (capture.requests, stderr[-4000:])
        if normal:
            validate_mixed_requests(capture, expected, stderr); return None
        lexical, acknowledged, records = {}, set(), []
        for ordinal, (host_time, status, body, payload) in enumerate(capture.requests):
            assert payload["schema"] == SCHEMA and payload["stream"] == "weather-test", payload
            assert len(payload["records"]) == 1, payload
            raw = record_bytes(body)
            if ordinal: assert host_time - capture.requests[ordinal - 1][0] >= .90, "HTTP exceeded configured1Hz"
            for record in payload["records"]:
                identity = record["session"], record["seq"]
                assert identity not in acknowledged, ("confirmed record sent again", identity)
                if identity in lexical: assert lexical[identity] == raw[identity], ("202 changed immutable record", identity)
                else: lexical[identity] = raw[identity]
                if status == 204: acknowledged.add(identity); records.append(record)
        assert capture.requests[0][1] == 202 and capture.requests[1][1] == 204, capture.requests[:2]
        assert record_bytes(capture.requests[0][2]) == record_bytes(capture.requests[1][2]), "202 batch forgotten/recomputed"
        assert set(lexical) == acknowledged, "healthy204 server left data unconfirmed"
        actual = {}
        for record in records: validate_samples(record, mode, expected, actual)
        assert actual.keys() == expected.keys(), ("missing samples", len(actual), len(expected), sorted(expected.keys() - actual.keys()), stderr[-3000:])
        print(f"{mode}: {len(actual)} SI samples; official wire units, honest time/location/uncertainty, statistics,202 retry/204 release: PASS")
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
            lambda item: item["weather"]["measurements"]["temperature"][0].pop("message_id"),
            lambda item: item["weather"].update(raw_n=129),
            lambda item: item["source"].update(sysid=256),
            lambda item: item.update(time_basis="receive_time"),
            lambda item: item["weather"]["measurements"]["humidity"][0]["representative"].update(value=65.0),
            lambda item: item["weather"]["measurements"]["temperature"][0]["statistics"].update(n=0),
            lambda item: item["weather"]["measurements"]["temperature"][0]["representative"].update(position_source="invented"),
            lambda item: item["weather"]["measurements"]["temperature"][0]["representative"].update(position_source="GPS", estimation_age_us=0),
            lambda item: item["weather"]["measurements"]["temperature"][0]["representative"].update(estimation_age_us=-1),
            lambda item: item["weather"]["measurements"]["temperature"][0]["representative"].update(estimation_age_us=4294967295),
            lambda item: item["weather"]["measurements"]["temperature"][0]["representative"].pop("position_source"),
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
    with tempfile.TemporaryDirectory(prefix="atmos-router-test-") as temporary:
        directory = Path(temporary)
        executable = directory / router.name; shutil.copy2(router, executable)
        client_case(executable, directory, "hygrometer")
        record = client_case(executable, directory, "atmos")
        client_case(executable, directory, "atmos-no-nav")
        client_case(executable, directory, "atmos-zero")
        client_case(executable, directory, "coexist")
        if server is not None:
            copied_server = directory / server.name; shutil.copy2(server, copied_server)
            server_case(copied_server, directory, record)
        client_case(executable, directory, "atmos", normal=True)


if __name__ == "__main__":
    main()
