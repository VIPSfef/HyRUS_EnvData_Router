"""Offline API structural validation. Fixtures contain synthetic values only.

Requires an already-installed jsonschema package; never installs dependencies.
Optional --test-server invokes only test-owned ephemeral loopback sockets.
"""
import argparse
from copy import deepcopy
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import threading
import time

from jsonschema import Draft202012Validator

BASE = Path(__file__).resolve().parent


def load(path):
    def invalid(token):
        raise ValueError("non-JSON constant: " + token)
    return json.loads(path.read_text(encoding="utf-8"), parse_constant=invalid)


def check_c_server(executable, example, status):
    # Self-contained so docs/api can be copied without the repository tests.
    reservation = socket.socket()
    reservation.bind(("127.0.0.1", 0))
    port = reservation.getsockname()[1]
    assert port not in {21000, 14540, 14550, 14551}
    reservation.close()
    valid = {"schema": "mavlink_weather_v1", "stream": "synthetic", "records": [example]}
    changes = [
        lambda item: item["spatial"].update(schema="unknown"),
        lambda item: item["spatial"].update(target_step_m=0),
        lambda item: item["spatial"]["bins"][0].update(width_m=0),
        lambda item: item["spatial"]["bins"][0].update(source_kind="UNKNOWN"),
        lambda item: item["spatial"]["bins"][0].update(t_end_us="0"),
        lambda item: item["spatial"]["bins"][0]["statistics"].update(n=0),
        lambda item: item["spatial"]["bins"][0]["statistics"].update(m2=-1),
        lambda item: item["spatial"]["bins"][0]["statistics"].update(variance=-1),
    ]
    cases = [(valid, 204)]
    for change in changes:
        invalid = deepcopy(valid)
        change(invalid["records"][0])
        cases.append((invalid, 400))
    cases.extend(((valid, 204), (status, 204)))
    for change in (
        lambda item: item.update(sample_count=1),
        lambda item: item.update(seq=7),
        lambda item: item["source"].update(connected=True),
        lambda item: item["source"].update(hb_age_ms=None),
        lambda item: item["weather"]["humidity"].update(value=0.5),
        lambda item: item["weather"]["temperature"].update(observations="0"),
        lambda item: item["devices"][1].update(data_age_ms="0"),
        lambda item: item["position"].update(lat_deg=91),
        lambda item: item["position"].update(rx_age_ms=None),
        lambda item: item["position"].update(source="UNKNOWN"),
        lambda item: item["position"].update(state="invented"),
    ):
        invalid = deepcopy(status)
        change(invalid["records"][0])
        cases.append((invalid, 400))
    cases.append((status, 204))
    output = bytearray()
    with tempfile.TemporaryDirectory(prefix="api-synthetic-server-") as temporary:
        directory = Path(temporary)
        copied = directory / executable.name
        shutil.copy2(executable, copied)
        child = subprocess.Popen([str(copied), "--bind", "127.0.0.1", "--port", str(port),
                                  "--plain", "--duration-ms", "5000", "--max-requests", str(len(cases) + 1)],
                                 cwd=directory, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        def drain():
            while True:
                chunk = child.stdout.read(4096)
                if not chunk: return
                assert len(output) + len(chunk) <= 8 * 1024 * 1024
                output.extend(chunk)
        reader = threading.Thread(target=drain, daemon=True)
        reader.start()
        def post(value):
            body = json.dumps(value, separators=(",", ":")).encode("utf-8")
            header = ("POST /ingest HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                      "Connection: close\r\nContent-Length: " + str(len(body)) + "\r\n\r\n").encode("ascii")
            with socket.create_connection(("127.0.0.1", port), timeout=2) as conn:
                conn.sendall(header + body)
                reply = b""
                while b"\r\n\r\n" not in reply:
                    part = conn.recv(4096)
                    assert part and len(reply) < 16384
                    reply += part
            return int(reply.split(b" ", 2)[1])
        try:
            until = time.monotonic() + 2
            while True:
                assert child.poll() is None
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=.05): break
                except (ConnectionRefusedError, socket.timeout):
                    assert time.monotonic() < until
                    time.sleep(.025)
            for body, expected in cases:
                assert post(body) == expected
            child.wait(timeout=7)
            reader.join(timeout=2)
            assert child.returncode == 0 and not reader.is_alive()
            expected_records = [record for body, code in cases if code == 204 for record in body["records"]]
            assert [json.loads(line) for line in output.decode("utf-8").splitlines()] == expected_records
        finally:
            if child.poll() is None: child.kill(); child.wait(timeout=2)
            reader.join(timeout=2)
            child.stdout.close(); child.stderr.close()
    print("PASS synthetic weather/status/GPS C server valid204 / malformed400 / recovery204; ephemeral port, RAM output only")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-server", type=Path)
    args = parser.parse_args()
    schema, api = load(BASE / "ingestion.schema.json"), load(BASE / "openapi.json")
    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema)
    checks = 1
    examples = {name: load(BASE / "examples" / name) for name in (
        "synthetic-weather-unknown.json", "synthetic-weather-gps-spatial.json", "synthetic-mixed-bundle.json",
        "synthetic-status-disconnected.json")}

    def valid(value):
        nonlocal checks
        validator.validate(value)
        checks += 1

    def invalid(value):
        nonlocal checks
        assert not validator.is_valid(value), "invalid mutation was accepted"
        checks += 1

    def mutated(value, change):
        result = deepcopy(value)
        change(result)
        invalid(result)

    for example in examples.values():
        valid(example)
    unknown = examples["synthetic-weather-unknown.json"]
    known = examples["synthetic-weather-gps-spatial.json"]
    mixed = examples["synthetic-mixed-bundle.json"]
    status = examples["synthetic-status-disconnected.json"]
    telemetry = next(entry["payload"] for entry in mixed["records"] if entry["kind"] == "telemetry")
    valid(telemetry)
    csv = deepcopy(telemetry)
    csv["schema"] = 2
    csv.pop("source")
    csv.pop("raw_clock_anchor", None)
    for batch in csv["batches"]:
        for key in ("batch_id", "clock_model", "raw_clock_anchor"):
            batch.pop(key, None)
    valid(csv)
    heartbeat = {"schema": "mavlink_router_test_v1", "stream": "synthetic", "records": [{
        "seq": 1, "source_generation": 1, "session": "synthetic.fixture", "source": "synthetic-input",
        "heartbeat": {"sysid": 1, "compid": 1, "type": 2, "autopilot": 12, "base_mode": 0,
                      "custom_mode": 0, "system_status": 4, "received_monotonic_ms": 1000},
        "position": None, "coordinate_age_ms": None}]}
    valid(heartbeat)
    # New object fields are accepted at root, envelope and nested data levels.
    future = deepcopy(known)
    future["future_metadata"] = {"version": 7}
    future["records"][0]["future_record"] = True
    future["records"][0]["weather"]["measurements"]["temperature"][0]["samples"][0]["future_sensor"] = 1
    valid(future)
    summary = deepcopy(telemetry)
    summary["batches"][0].update(raw_included=False, raw=[])
    valid(summary)
    close = deepcopy(known)
    close["records"][0]["weather"].update(n=0, raw_n=0, raw_dropped=0, measurements={"temperature": [], "humidity": []})
    for item in close["records"][0]["spatial"]["bins"]:
        item.update(state="interrupted", revision="2")
    valid(close)
    # Latest status is not a measurement, and live GCS traffic cannot hide a
    # selected vehicle heartbeat that has expired. Every age keeps its basis.
    live = deepcopy(status)
    live["records"][0]["source"].update(state="LOCKED", connected=True, heartbeat="LIVE", hb_age_ms="0")
    valid(live)
    waiting = deepcopy(status)
    waiting["records"][0]["source"].update(state="WAIT_HEARTBEAT", heartbeat="WAIT", hb_age_ms=None)
    valid(waiting)
    no_position = deepcopy(status)
    no_position["records"][0]["position"] = None
    valid(no_position)
    no_position["records"][0].pop("position")
    valid(no_position)  # Backwards-compatible optional field.
    capacity = deepcopy(status)
    device = capacity["records"][0]["devices"][0]
    capacity["records"][0]["devices"] = [dict(device, sysid=i + 1) for i in range(32)]
    valid(capacity)
    mutated(capacity, lambda item: item["records"][0]["devices"].append(dict(device, sysid=33)))
    for change in (
        lambda item: item.update(sample_count=1),
        lambda item: item.update(seq=7),
        lambda item: item.update(generation="18446744073709551616"),
        lambda item: item["source"].update(connected=True),
        lambda item: item["source"].update(hb_age_ms=None),
        lambda item: item["source"].update(heartbeat="WAIT"),
        lambda item: item["weather"]["temperature"].update(observations="0"),
        lambda item: item["weather"]["temperature"].update(value=None),
        lambda item: item["weather"]["humidity"].update(value=0.5),
        lambda item: item["weather"]["humidity"].update(state="FRESH", value=1.1, rx_age_ms="0", observations="1"),
        lambda item: item["devices"][1].update(data_age_ms="0"),
        lambda item: item["devices"][0].update(sysid=0),
        lambda item: item["position"].update(lat_deg=91),
        lambda item: item["position"].update(lon_deg=-181),
        lambda item: item["position"].update(rx_age_ms=None),
        lambda item: item["position"].update(source="UNKNOWN"),
        lambda item: item["position"].update(state="invented"),
    ):
        mutated(status, lambda item, change=change: change(item["records"][0]))
    mutated(live, lambda item: item["records"][0]["source"].update(connected=False))

    def sample(value):
        return value["records"][0]["weather"]["measurements"]["temperature"][0]["samples"][0]
    def group(value):
        return value["records"][0]["weather"]["measurements"]["temperature"][0]
    def spatial(value):
        return value["records"][0]["spatial"]["bins"][0]

    for change in (
        lambda item: sample(item).pop("velocity_valid"),
        lambda item: sample(item).update(t_us=None),
        lambda item: sample(item).update(t_us=1000000),
        lambda item: sample(item).update(estimation_age_us=1),
        lambda item: sample(item).update(quality=-1),
        lambda item: group(item)["statistics"].update(variance=0),
        lambda item: item["records"][0].update(seq="1"),
        lambda item: spatial(item).update(width_m=0),
        lambda item: spatial(item).update(source_kind="UNKNOWN"),
        lambda item: spatial(item)["statistics"].pop("m2"),
        lambda item: spatial(item)["statistics"].update(variance=0),
        lambda item: item["records"][0]["clock_model"].update(offset_us="9223372036854775808"),
    ):
        mutated(known, change)
    mutated(unknown, lambda item: sample(item).update(velocity_valid=True))
    mutated(unknown, lambda item: group(item)["statistics"].update(time_known_n=0, t_start_us="1"))
    mutated(telemetry, lambda item: item["batches"][0].pop("stats"))
    mutated(telemetry, lambda item: item["field_order"].reverse())
    mutated(telemetry, lambda item: item["batches"][0]["raw"][0].pop())
    mutated(telemetry, lambda item: item["batches"][0]["raw"][0].__setitem__(12, 20001))
    mutated(telemetry, lambda item: item["batches"][0]["raw"][0].__setitem__(13, 200001))
    mutated(telemetry, lambda item: item["batches"][0]["raw"][0].__setitem__(15, None))
    mutated(telemetry, lambda item: item["batches"][0].update(raw_included=False))
    mutated(telemetry, lambda item: item["loss"].update(rejected="18446744073709551616"))
    mutated(heartbeat, lambda item: item["records"][0].update(coordinate_age_ms=0))
    mutated(mixed, lambda item: item["records"][0].update(kind="telemetry"))
    invalid({"schema": "mavlink_spatial_v1"})
    invalid({"schema": 4})

    # Check exact64-bit decimal grammar independently of entire envelopes.
    for definition, good, bad in (
        ("U64String", ["0", "1", "18446744073709551615"], ["-1", "00", "+1", "18446744073709551616", 1]),
        ("I64String", ["0", "9223372036854775807", "-9223372036854775808"], ["-0", "01", "9223372036854775808", "-9223372036854775809"]),
    ):
        primitive = Draft202012Validator({"$defs": schema["$defs"], "$ref": "#/$defs/" + definition})
        for item in good:
            primitive.validate(item); checks += 1
        for item in bad:
            assert not primitive.is_valid(item); checks += 1

    # Internal schema references resolve locally: no registry/network fetch.
    def references(value):
        if isinstance(value, dict):
            if "$ref" in value:
                reference = value["$ref"]
                assert reference.startswith("#/$defs/") and reference[8:] in schema["$defs"]
            for child in value.values(): references(child)
        elif isinstance(value, list):
            for child in value: references(child)
    references(schema)
    checks += 1
    assert api["openapi"] == "3.1.1"
    operation = api["paths"]["/ingest"]["post"]
    assert operation["requestBody"]["content"]["application/json"]["schema"]["$ref"] == "./ingestion.schema.json"
    assert {} in operation["security"] and {"bearerAuth": []} in operation["security"]
    assert "204" in operation["responses"] and "content" not in operation["responses"]["204"]
    assert api["components"]["securitySchemes"]["bearerAuth"]["scheme"] == "bearer"
    for example in operation["requestBody"]["content"]["application/json"]["examples"].values():
        valid(example["value"])
    checks += 5
    print(f"PASS {checks} offline API schema/example/boundary checks; OpenAPI local shape checked, no dedicated OpenAPI metaschema validator")

    if args.test_server:
        executable = args.test_server.resolve()
        assert executable.is_file(), executable
        check_c_server(executable, known["records"][0], status)


if __name__ == "__main__":
    main()
