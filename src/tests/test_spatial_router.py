"""10 Hz synthetic SENS_ATMOS -> real UDP router -> spatial/weather HTTP.

Usage: python test_spatial_router.py ROUTER [C_TEST_SERVER]
Only test-owned ephemeral loopback ports are used, never COM, 21000 or QGC.
Synthetic observations, HTTP bodies and server output stay in RAM. Temporary
files contain configuration/executable copies only. No third-party modules.
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

SAMPLE_COUNT = 60
RAM_LIMIT = 8 * 1024 * 1024
FORBIDDEN_PORTS = {21000, 14540, 14550, 14551}


def reserve(kind):
    while True:
        sock = socket.socket(socket.AF_INET, kind)
        sock.bind(("127.0.0.1", 0))
        if sock.getsockname()[1] not in FORBIDDEN_PORTS:
            return sock
        sock.close()


def source_values(index):
    # Exactly representable wire float32 values; do not compare decimal ideals.
    return 20 + index * .25, 40 + (index % 20) * .5


def navigation(wire, boot_ms, speed):
    latitude = math.radians(37)
    east_radius = 6378137 / math.sqrt(1 - .0066943799901413165 * math.sin(latitude) ** 2) * math.cos(latitude)
    east_m = speed * (boot_ms - 1000) / 1000
    longitude = round((127 + math.degrees(east_m / east_radius)) * 1e7)
    gps = bytearray(52)
    struct.pack_into("<Qiii", gps, 0, boot_ms * 1000, 370_000_000, longitude, 100_000)
    gps[28:30] = bytes((3, 14))
    struct.pack_into("<II", gps, 34, 2000, 3000)
    status = struct.pack("<Q8fH", boot_ms * 1000, *([0.] * 8), 54)
    global_position = struct.pack("<IiiiihhhH", boot_ms, 370_000_000, longitude,
                                  100_000, 0, 0, round(speed * 100), 0, 0)
    return (wire.packet(24, gps, 24) + wire.packet(230, status, 163) +
            wire.packet(33, global_position, 104))


def atmosphere(wire, index):
    temperature, humidity = source_values(index)
    return wire.packet(8009, struct.pack("<Qff", (1000 + index * 100) * 1000,
                                        temperature, humidity), 144)


def check_statistics(stats, samples):
    values = [sample["value"] for sample in samples]
    assert values and stats["n"] == len(values), "bin sample count differs from retained raw"
    mean = statistics.mean(values)
    assert math.isclose(stats["mean"], mean, rel_tol=1e-12, abs_tol=1e-12), "mean differs from independent raw mean"
    assert stats["min"] == min(values) and stats["max"] == max(values), "range differs from raw"
    if len(values) == 1:
        assert stats["variance"] is None, "one-sample variance must be null"
    else:
        variance = statistics.variance(values)
        assert math.isclose(stats["variance"], variance, rel_tol=1e-9, abs_tol=1e-12), "sample variance differs from independent n-1 result"
    if "m2" in stats:
        m2 = math.fsum((value - mean) ** 2 for value in values)
        assert math.isclose(stats["m2"], m2, rel_tol=1e-9, abs_tol=1e-12), "M2 differs from independent two-pass result"


def bin_key(record, item):
    return (record["session"], record["source"]["sysid"], record["source"]["compid"],
            item["continuity_key"], item["path_id"], item["bin_index"], item["metric"],
            item["message_id"], item["sensor_id"], item["sensor_instance"], item["source_kind"])


def validate_capture(capture, speed, stderr):
    assert not capture.errors, capture.errors
    assert len(capture.requests) >= 5, "too few 1 Hz requests"
    assert sum(len(entry[2]) for entry in capture.requests) + len(stderr) < RAM_LIMIT, "test capture RAM budget exceeded"
    first, second = capture.requests[:2]
    assert first[1] == 202 and second[1] == 204, "retry fixture did not issue 202 then 204"
    assert first[2] == second[2], "retry recomputed weather/spatial body bytes"
    lexical, acknowledged, records = {}, set(), []
    for ordinal, (received, status, body, payload) in enumerate(capture.requests):
        if ordinal:
            assert received - capture.requests[ordinal - 1][0] >= .90, "HTTP request start exceeded configured 1 Hz"
        assert payload["schema"] == "mavlink_weather_v1" and payload["stream"] == "spatial-test"
        assert len(payload["records"]) == 1
        encoded = record_bytes(body)
        for record in payload["records"]:
            identity = record["session"], record["seq"]
            assert identity not in acknowledged, "204-confirmed batch was sent again"
            if identity in lexical:
                assert lexical[identity] == encoded[identity], "unconfirmed record bytes changed"
            else:
                lexical[identity] = encoded[identity]
            if status == 204:
                acknowledged.add(identity)
                records.append(record)
    assert set(lexical) == acknowledged, "healthy 204 server left an unconfirmed batch"
    assert b"queue_windows_dropped=0 queue_samples_dropped=0 pending=0" in stderr, "ACK did not drain queue without loss"
    assert b"raw_dropped=0" in stderr and b"sequence_gaps=0" in stderr, "raw/transport loss"

    # Assemble only confirmed raw, independently of spatial bin revisions.
    raw = {"temperature": {}, "humidity": {}}
    for record in records:
        assert record["frame"] == "ENU" and record["origin_wgs84"] == [37, 127, 100]
        assert record["source"] == {"sysid": 1, "compid": 1}
        assert record["time_basis"] == "pixhawk_boot_us"
        weather = record["weather"]
        assert weather["n"] == weather["raw_n"] and weather["raw_dropped"] == 0
        total = 0
        for metric, groups in weather["measurements"].items():
            assert metric in raw and len(groups) <= 1
            for group in groups:
                assert group["message_id"] == 8009 and group["sensor_id"] == group["sensor_instance"] == 0
                assert group["raw_n"] == len(group["samples"]) and group["raw_dropped"] == 0
                check_statistics(group["statistics"], group["samples"])
                assert group["representative"] in group["samples"]
                for sample in group["samples"]:
                    stamp = int(sample["t_us"])
                    index, remainder = divmod(stamp - 1_000_000, 100_000)
                    assert remainder == 0 and 0 <= index < SAMPLE_COUNT
                    assert index not in raw[metric], "raw sample duplicated between ACKed windows"
                    temperature, humidity = source_values(index)
                    expected = temperature + 273.15 if metric == "temperature" else humidity * .01
                    assert math.isclose(sample["value"], expected, rel_tol=1e-13, abs_tol=1e-12)
                    assert sample["position_source"] == "GPS" and sample["velocity_valid"] is True
                    assert sample["estimation_age_us"] == 0 and sample["velocity_mps"] == [speed, 0, 0]
                    assert sample["navigation_segment"] == 0
                    assert math.isclose(sample["position_m"][0], speed * index * .1, abs_tol=.012)
                    assert sample["position_m"][1:] == [0, 0]
                    assert sample["time_uncertainty_us"] is None and sample["measurement_variance"] is None
                    assert sample["position_uncertainty_m"] is None, "unknown timestamp error became zero"
                    raw[metric][index] = sample
                    total += 1
        assert total == weather["n"]
    assert all(set(samples) == set(range(SAMPLE_COUNT)) for samples in raw.values()), "10 Hz raw samples lost"

    latest, history, revisions, complete_per_request = {}, {}, 0, []
    singleton_seen = False
    for record in records:
        spatial = record["spatial"]
        assert spatial["schema"] == "mavlink_spatial_v1" and spatial["si_registry"] == 1
        assert spatial["target_step_m"] == 10 and spatial["adaptive"] is True and spatial["gap_us"] == "2000000"
        counters = spatial["counters"]
        for counter in ("unknown_samples", "rejected_samples", "late_samples", "dropped_records", "inconsistent_steps"):
            assert int(counters[counter]) == 0, "unexpected spatial rejection: " + counter
        complete_bins = set()
        for item in spatial["bins"]:
            key = bin_key(record, item)
            assert item["metric"] in raw and item["source_kind"] == "GPS"
            assert item["message_id"] == 8009 and item["sensor_id"] == item["sensor_instance"] == 0
            assert item["state"] in {"partial", "complete", "interrupted"}
            assert item["width_m"] >= 10 and math.isclose(item["interval_end_m"] - item["interval_start_m"], item["width_m"], abs_tol=1e-8)
            start, end = int(item["t_start_us"]), int(item["t_end_us"])
            samples = [sample for sample in raw[item["metric"]].values() if start <= int(sample["t_us"]) <= end]
            check_statistics(item["statistics"], samples)
            assert item["representative"] in samples, "spatial representative is not a retained observation"
            distances = [speed * (int(sample["t_us"]) - 1_000_000) / 1_000_000 for sample in samples]
            assert math.isclose(item["observed_start_m"], min(distances), abs_tol=1e-8)
            assert math.isclose(item["observed_end_m"], max(distances), abs_tol=1e-8)
            assert all(item["interval_start_m"] - 1e-7 <= distance <= item["interval_end_m"] + 1e-7 for distance in distances)
            singleton_seen |= item["statistics"]["n"] == 1
            if key in latest:
                previous = latest[key]
                assert int(item["revision"]) > int(previous["revision"]), "new snapshot did not advance revision"
                assert item["statistics"]["n"] >= previous["statistics"]["n"], "1 Hz seal reset an open bin count"
                assert item["t_start_us"] == previous["t_start_us"] and item["width_m"] == previous["width_m"]
                assert previous["state"] == "partial", "terminal bin was revised again"
                revisions += 1
            latest[key] = item
            history.setdefault(key, []).append(item)
            if item["state"] == "complete":
                complete_bins.add((item["path_id"], item["bin_index"]))
        complete_per_request.append(len(complete_bins))

    # Upsert replacements, never sum partial snapshots. Each raw sample belongs
    # to one final group/bin, even when a bin spans several 1-second requests.
    covered = {metric: set() for metric in raw}
    for item in latest.values():
        assert item["state"] != "partial", "EOF failed to close final partial bin"
        start, end = int(item["t_start_us"]), int(item["t_end_us"])
        indices = {index for index, sample in raw[item["metric"]].items() if start <= int(sample["t_us"]) <= end}
        assert not covered[item["metric"]] & indices, "upserted spatial bins overlap"
        covered[item["metric"]].update(indices)
    assert all(indices == set(range(SAMPLE_COUNT)) for indices in covered.values()), "spatial upsert did not cover raw exactly once"
    final = records[-1]["spatial"]
    assert int(final["counters"]["samples_seen"]) == int(final["counters"]["spatial_samples"]) == 2 * SAMPLE_COUNT
    assert int(final["counters"]["paths_started"]) == 1
    assert math.isclose(final["path"]["s_m"], speed * (SAMPLE_COUNT - 1) * .1, abs_tol=1e-8)
    complete = [item for item in latest.values() if item["state"] == "complete" and item["metric"] == "temperature"]
    assert complete, "motion never completed a bin"
    if speed == 2:
        assert any(49 <= item["statistics"]["n"] <= 51 for item in complete), "2m/s did not retain approximately 50 samples per10m bin"
        assert any(len(items) >= 3 and items[-1]["statistics"]["n"] >= 45 and
                   int(items[-1]["t_end_us"]) - int(items[0]["t_start_us"]) >= 4_800_000 for items in history.values()), "slow bin did not survive multiple 1 Hz seals"
        assert revisions >= 4
    elif speed == 20:
        assert all(4 <= item["statistics"]["n"] <= 6 for item in complete)
        assert max(complete_per_request) >= 2, "faster motion did not produce several bins in one request"
    else:
        assert int(final["counters"]["undersampled_steps"]) > 0 and singleton_seen
        assert any(item["width_m"] >= 25 - 1e-8 and item["width_m"] > 10 and item["flags"] & 1 for item in latest.values()), ("25m sample steps did not widen/admit undersampling", sorted({(item["width_m"], item["flags"]) for item in latest.values()}))
        assert max(complete_per_request) >= 5
    return {"speed_mps": speed, "raw_samples": sum(map(len, raw.values())),
            "completed_temperature_bins": len(complete), "max_bins_per_request": max(complete_per_request),
            "revisions": revisions}, records


def client_case(executable, directory, speed):
    with CaptureServer(retry=True) as capture:
        assert capture.port not in FORBIDDEN_PORTS
        reservation = reserve(socket.SOCK_DGRAM)
        port = reservation.getsockname()[1]
        config = directory / ("spatial-" + str(speed) + ".ini")
        settings = {"url": f"http://127.0.0.1:{capture.port}/ingest", "stream": "spatial-test",
                    "session": "spatial-test", "input_mode": "mavlink", "serial_port": "none",
                    "auto_scan": 0, "websocket_url": "", "udp_enabled": 1, "udp_bind": "127.0.0.1", "udp_port": port,
                    "probe_ms": 1000, "link_timeout_ms": 1500, "reconnect_ms": 100, "duration_ms": 7000,
                    "send_interval_ms": 1000, "connect_timeout_ms": 500, "io_timeout_ms": 500,
                    "origin_auto": 0, "origin_lat": 37, "origin_lon": 127, "origin_alt": 100,
                    "spatial_step_m": 10, "spatial_gap_ms": 2000, "spatial_enabled": 1, "spatial_adaptive": 1}
        saved = "".join(f"{key}={value}\n" for key, value in settings.items())
        config.write_text(saved, encoding="utf-8")
        child = subprocess.Popen([str(executable), "--config", str(config), "--test-weather",
                                  "--server-url", settings["url"]], cwd=directory,
                                 stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        reservation.close()
        stop = threading.Event()
        errors, sent = [], []

        def produce():
            try:
                wire = Wire()
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
                    source.bind(("127.0.0.1", 0)); source.settimeout(.002)
                    destination = ("127.0.0.1", port)
                    for _ in range(8):
                        source.sendto(wire.heartbeat(), destination)
                        if stop.wait(.04): return
                    start = time.monotonic()
                    for index in range(SAMPLE_COUNT):
                        if stop.wait(max(0, start + index * .1 - time.monotonic())): return
                        boot = 1000 + index * 100
                        payload = (wire.heartbeat() + navigation(wire, boot - 50, speed) + atmosphere(wire, index) +
                                   navigation(wire, boot + 50, speed))
                        source.sendto(payload, destination); sent.append(index)
                        try:
                            source.recvfrom(65535)
                            raise AssertionError("receive-only router replied to synthetic MAVLink UDP")
                        except (socket.timeout, ConnectionResetError, ConnectionRefusedError):
                            pass
                    while not stop.wait(.1):
                        source.sendto(wire.heartbeat(), destination)
            except BaseException as error:
                errors.append(type(error).__name__ + ": " + str(error))

        producer = threading.Thread(target=produce, daemon=True)
        try:
            # The CLI test mode otherwise overrides a config URL with21000.
            # Confirm the explicit test-owned endpoint before producing data.
            startup = child.stderr.readline()
            assert settings["url"].encode("ascii") in startup, "router startup endpoint is not the reserved test endpoint"
            producer.start()
            stdout, tail = child.communicate(timeout=18)
            stderr = startup + tail
        finally:
            stop.set()
            if producer.ident is not None: producer.join(timeout=2)
            if child.poll() is None: child.kill(); child.wait(timeout=2)
        assert child.returncode == 0 and not stdout, ("router failed", child.returncode, stderr[-1500:])
        assert not producer.is_alive() and not errors, errors
        assert sent == list(range(SAMPLE_COUNT)), "finite fixture did not send all samples"
        assert config.read_text(encoding="utf-8") == saved, "runtime changed saved settings"
        return validate_capture(capture, speed, stderr)


def c_server_case(executable, directory, example):
    reservation = reserve(socket.SOCK_STREAM)
    port = reservation.getsockname()[1]; reservation.close()
    valid = {"schema": "mavlink_weather_v1", "stream": "spatial-test", "records": [example]}
    mutations = [
        lambda item: item["spatial"].update(schema="unknown_spatial"),
        lambda item: item["spatial"].update(target_step_m=0),
        lambda item: item["spatial"]["bins"][0].update(width_m=0),
        lambda item: item["spatial"]["bins"][0].update(source_kind="UNKNOWN"),
        lambda item: item["spatial"]["bins"][0].update(t_end_us="0"),
        lambda item: item["spatial"]["bins"][0]["statistics"].update(n=0),
        lambda item: item["spatial"]["bins"][0]["statistics"].update(m2=-1),
        lambda item: item["spatial"]["bins"][0]["statistics"].update(variance=-1),
    ]
    child = subprocess.Popen([str(executable), "--bind", "127.0.0.1", "--port", str(port), "--plain",
                              "--duration-ms", "5000", "--max-requests", str(len(mutations) + 3)],
                             cwd=directory, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    output, errors = bytearray(), []

    def drain():
        try:
            while True:
                part = child.stdout.read(4096)
                if not part: return
                if len(output) + len(part) > RAM_LIMIT: raise AssertionError("C server RAM output limit")
                output.extend(part)
        except BaseException as error:
            errors.append(repr(error))
    reader = threading.Thread(target=drain, daemon=True); reader.start()
    encoded = json.dumps(valid, separators=(",", ":")).encode("utf-8")
    try:
        until = time.monotonic() + 2
        while True:
            assert child.poll() is None, "C server exited before listening"
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.05): break
            except (ConnectionRefusedError, socket.timeout):
                assert time.monotonic() < until, "C server did not listen"
                time.sleep(.025)
        assert response_status(port, request(encoded)) == 204, "C server rejected captured spatial record"
        for mutate in mutations:
            payload = deepcopy(valid); mutate(payload["records"][0])
            body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
            assert response_status(port, request(body)) == 400, "C server accepted malformed spatial metadata"
        assert response_status(port, request(encoded)) == 204, "rejection poisoned a later valid request"
        child.wait(timeout=7); reader.join(timeout=2)
        stderr = child.stderr.read()
        assert child.returncode == 0 and not reader.is_alive() and not errors, "C server did not exit cleanly"
        assert b"\x1b" not in output + stderr
        assert [json.loads(line) for line in output.decode("utf-8").splitlines()] == [example, example], "C server printed rejected/missing records"
    finally:
        if child.poll() is None: child.kill(); child.wait(timeout=2)
        reader.join(timeout=2)
    print("PASS C test server: captured spatial/weather204, eight malformed spatial400, valid recovery; RAM only")


def main():
    assert 2 <= len(sys.argv) <= 3, __doc__
    router = Path(sys.argv[1]).resolve()
    server = Path(sys.argv[2]).resolve() if len(sys.argv) == 3 else None
    assert router.is_file() and (server is None or server.is_file())
    with tempfile.TemporaryDirectory(prefix="spatial-router-test-") as temporary:
        directory = Path(temporary)
        executable = directory / router.name; shutil.copy2(router, executable)
        results, example = [], None
        for speed in (2, 20, 250):
            result, records = client_case(executable, directory, speed)
            results.append(result)
            if speed == 250:
                example = next(record for record in records if record["spatial"]["bins"])
            print("PASS " + json.dumps(result, sort_keys=True))
        assert len({item["raw_samples"] for item in results}) == 1
        assert results[1]["completed_temperature_bins"] > results[0]["completed_temperature_bins"]
        if server is not None:
            copied_server = directory / server.name; shutil.copy2(server, copied_server)
            c_server_case(copied_server, directory, example)


if __name__ == "__main__":
    main()
