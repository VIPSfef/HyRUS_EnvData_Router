#!/usr/bin/env python3
"""Physical SHTC3 -> collector -> actual C HTTP server test, observations in RAM.

Default is plan only. --run is for an operator with exclusive COM ownership after closing any other
serial handle and configures the real PX4 SENS_ATMOS stream. No PX4 commands,
firmware changes, UDP injection, synthetic MAVLink, or observation files.
"""
import argparse
from collections import Counter
import json
import math
from pathlib import Path
import re
import statistics
import subprocess
import tempfile
import threading
import time
import os

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/windows-x64"
SERVER = BUILD / "tools/mavlink_telemetry_data_router_test_server.exe"
TIME_UNKNOWN = 0x08000000
POSITION_UNKNOWN = 0x04000000
CAPTURE_CAP = 8 * 1024 * 1024


class CheckFailure(Exception):
    pass


def require(condition, code):
    if not condition:
        raise CheckFailure(code)


def finite(value):
    return type(value) in (float, int) and math.isfinite(value)


def vector3(value):
    return isinstance(value, list) and len(value) == 3 and all(finite(x) for x in value)


def validate(records, arrival_times, client_stderr, server_stderr, position="any", clock_model="any"):
    """Independent calculations; return counts/status only, never sensor values."""
    require(len(records) >= 3, "fewer_than_three_weather_windows")
    require(len(records) == len(arrival_times), "arrival_count")
    seen_records, sample_keys = set(), set()
    pairs = {}
    qualities = Counter()
    counters = Counter()
    previous_host = None
    for record in records:
        identity = record["session"], record["seq"]
        require(identity not in seen_records, "duplicate_acked_record")
        seen_records.add(identity)
        require(record["time_basis"] == "pixhawk_boot_us", "wrong_sensor_clock")
        require(record["host_time_basis"] == "collector_monotonic_ms", "wrong_host_clock")
        require(record["frame"] == "ENU", "wrong_position_frame")
        source = record["source"]
        require(1 <= source["sysid"] <= 255 and 0 <= source["compid"] <= 255, "invalid_source")
        origin = record["origin_wgs84"]
        if position == "unknown":
            require(origin is None, "origin_fabricated_without_navigation")
        if clock_model == "unknown":
            require(record["clock_model"] is None, "utc_clock_model_fabricated")
        require(origin is None or (vector3(origin) and -90 <= origin[0] <= 90 and -180 <= origin[1] <= 180), "invalid_origin")
        weather = record["weather"]
        require(weather["si_registry"] == 1, "wrong_si_registry")
        host = int(weather["host_window_ms"])
        require(previous_host is None or host > previous_host, "nonincreasing_window")
        previous_host = host
        require(weather["raw_dropped"] == 0 and 0 < weather["n"] == weather["raw_n"] <= 128, "raw_loss_or_invalid_count")
        measurements = weather["measurements"]
        require(set(measurements) == {"temperature", "humidity"}, "unexpected_metric_including_pressure")
        n_total = 0
        for metric, groups in measurements.items():
            require(len(groups) <= 1, "multiple_sensor_groups_unexpected")
            # The sender may seal between the two metric callbacks; pair by
            # acquisition time across adjacent records, never fabricate a pair.
            if not groups:
                continue
            group = groups[0]
            require(group["message_id"] == 8009, "message_is_not_sens_atmos")
            require(group["sensor_id"] == group["sensor_instance"] == 0, "invented_wire_sensor_identity")
            samples, stats = group["samples"], group["statistics"]
            require(samples and group["representative"] in samples, "representative_not_actual_sample")
            require(len(samples) == group["raw_n"] == stats["n"] and group["raw_dropped"] == 0, "group_count_or_loss")
            values = [sample["value"] for sample in samples]
            require(all(finite(value) for value in values), "nonfinite_measurement")
            require(math.isclose(stats["mean"], statistics.mean(values), rel_tol=1e-13, abs_tol=1e-11), "mean_mismatch")
            require(stats["min"] == min(values) and stats["max"] == max(values), "extrema_mismatch")
            if len(values) == 1:
                require(stats["variance"] is None, "single_sample_variance_not_null")
            else:
                require(math.isclose(stats["variance"], statistics.variance(values), rel_tol=1e-9, abs_tol=1e-11), "sample_variance_mismatch")
            known_times = []
            for sample in samples:
                value = sample["value"]
                require(233.15 - 1e-5 <= value <= 398.15 + 1e-5 if metric == "temperature" else 0 <= value <= 1, "si_scale_or_shtc3_range")
                q = sample["quality"]
                known_time = sample["t_us"] is not None
                known_position = sample["position_m"] is not None
                provenance = sample["position_source"]
                require(provenance in ("GPS", "ESTGPS", "UNKNOWN"), "invalid_position_source")
                require(bool(q & 0x10000000) == (provenance == "ESTGPS"), "estgps_quality_mismatch")
                if not known_position:
                    require(provenance == "UNKNOWN" and sample["estimation_age_us"] is None, "position_provenance_fabricated")
                require(bool(q & TIME_UNKNOWN) == (not known_time), "time_quality_mismatch")
                require(bool(q & POSITION_UNKNOWN) == (not known_position), "position_quality_mismatch")
                require(known_time, "physical_sens_atmos_time_missing")
                stamp = int(sample["t_us"])
                require(0 < stamp < 946684800000000, "invalid_boot_time")
                known_times.append(stamp)
                require(sample["time_uncertainty_us"] is None, "fabricated_timestamp_uncertainty")
                require(sample["measurement_variance"] is None, "fabricated_sensor_variance")
                require(sample["position_uncertainty_m"] is None, "fabricated_combined_position_uncertainty")
                nav_error = sample["navigation_uncertainty_m"]
                if known_position:
                    require(origin is not None and vector3(sample["position_m"]) and vector3(sample["velocity_mps"]), "invalid_known_navigation")
                    require(nav_error is None or finite(nav_error) and nav_error >= 0, "invalid_nav_uncertainty")
                else:
                    require(sample["velocity_mps"] is None and nav_error is None, "unknown_navigation_not_null")
                if position != "any":
                    require(known_position == (position == "known"), "unexpected_position_availability")
                counters["position_known" if known_position else "position_unknown"] += 1
                qualities[q] += 1
                key = record["session"], metric, stamp
                require(key not in sample_keys, "duplicate_acked_sample")
                sample_keys.add(key)
                pair = pairs.setdefault((record["session"], stamp), {})
                require(metric not in pair, "duplicate_metric_timestamp")
                pair[metric] = sample
            require(stats["time_known_n"] == len(samples), "known_time_count")
            require(int(stats["t_start_us"]) == min(known_times) and int(stats["t_end_us"]) == max(known_times), "sample_time_bounds")
            n_total += len(samples)
            counters[metric] += len(samples)
        require(n_total == weather["n"], "weather_count_mismatch")
    complete = 0
    for pair in pairs.values():
        require(set(pair) == {"temperature", "humidity"}, "temperature_humidity_time_pair_missing")
        a, b = pair["temperature"], pair["humidity"]
        for name in ("position_m", "velocity_mps", "time_uncertainty_us", "navigation_uncertainty_m", "position_uncertainty_m", "quality", "position_source", "estimation_age_us"):
            require(a[name] == b[name], "paired_metadata_mismatch")
        complete += 1
    require(complete > 0, "no_time_pairs")
    gaps = [b - a for a, b in zip(arrival_times, arrival_times[1:])]
    require(not gaps or min(gaps) >= .85, "http_send_faster_than_configured_1hz")
    summary = re.search(r"rejected=(\d+) sequence_gaps=(\d+) raw_dropped=(\d+) queue_windows_dropped=(\d+) queue_samples_dropped=(\d+) pending=(\d+)", client_stderr)
    require(summary is not None and all(int(x) == 0 for x in summary.groups()), "collector_loss_or_pending_after_ack")
    stopped = re.search(r"STOPPED requests=(\d+) accepted=(\d+) records=(\d+) rejected=(\d+)", server_stderr)
    require(stopped is not None, "server_stop_summary_missing")
    requests, accepted, printed, rejected = map(int, stopped.groups())
    require(requests == accepted == printed == len(records) and rejected == 0, "server_not_all_validated_and_204_acked")
    return {"status": "PASS", "windows": len(records), "raw_samples": sum(counters[m] for m in ("temperature", "humidity")), "timestamp_pairs": complete,
            "position_known_samples": counters["position_known"], "position_unknown_samples": counters["position_unknown"],
            "quality_values": [f"0x{q:08X}" for q in sorted(qualities)], "independent_mean_variance": True,
            "no_pressure_metric": True, "unknown_uncertainties_null": True, "server_requests": requests, "http204_validated": accepted,
            "pending_after_ack": 0, "duplicate_acked_records": 0, "observations_written_to_file": False,
            "request_min_spacing_ms": round(min(gaps) * 1000, 3) if gaps else None}


class PipeCapture:
    def __init__(self, pipe, lines=False):
        self.pipe, self.lines = pipe, lines
        self.items, self.size, self.overflow = [], 0, False
        self.ready = threading.Event()
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        tail = b""
        while True:
            item = self.pipe.readline(131073) if self.lines else self.pipe.read1(8192)
            if not item:
                break
            self.size += len(item)
            if self.size <= CAPTURE_CAP:
                self.items.append((time.monotonic(), item))
            else:
                self.overflow = True
            tail = (tail + item)[-256:]
            if b"LISTENING TCP HTTP" in tail:
                self.ready.set()

    def text(self):
        return b"".join(item for _, item in self.items).decode("utf-8", errors="replace")


def run(args):
    collector = BUILD / ("mavlink_telemetry_data_router_" + args.variant + ".exe")
    require(SERVER.is_file() and collector.is_file(), "required_executable_missing")
    require(os.name == "nt", "native_windows_only")
    require(6000 <= args.duration_ms <= 20000, "duration_outside_6_to_20_seconds")
    require(re.fullmatch(r"COM[1-9][0-9]*", args.serial, re.IGNORECASE) is not None, "explicit_com_port_required")
    children = []
    captures = []
    environment = os.environ.copy()
    environment.pop("MTR_PHYSICAL_TEST_NO_AUTH", None)
    try:
        with tempfile.TemporaryDirectory(prefix="physical-weather-config-") as directory:
            config = Path(directory) / "physical-weather.ini"
            config.write_text("\n".join(["input_mode=mavlink", "serial_port=" + args.serial, "serial_baud=115200", "auto_scan=0", "udp_enabled=0",
                "url=http://127.0.0.1:21000/ingest", "stream=physical-shtc3", "session=physical-shtc3", "token_env=MTR_PHYSICAL_TEST_NO_AUTH",
                "duration_ms=" + str(args.duration_ms), "send_interval_ms=1000", "connect_timeout_ms=1000", "io_timeout_ms=1000", "origin_auto=1", "summary=1", ""]), encoding="utf-8")
            server = subprocess.Popen([str(SERVER), "--plain", "--bind", "127.0.0.1", "--port", "21000", "--duration-ms", str(args.duration_ms + 5000)],
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment, creationflags=subprocess.CREATE_NO_WINDOW)
            children.append(server)
            server_out, server_err = PipeCapture(server.stdout, True), PipeCapture(server.stderr)
            captures.extend([server_out, server_err])
            require(server_err.ready.wait(3) and server.poll() is None, "c_server_could_not_listen_on_21000")
            client = subprocess.Popen([str(collector), "--config", str(config), "--test-weather", "--headless", "--summary"],
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment, creationflags=subprocess.CREATE_NO_WINDOW)
            children.append(client)
            client_out, client_err = PipeCapture(client.stdout), PipeCapture(client.stderr)
            captures.extend([client_out, client_err])
            require(client.wait(timeout=args.duration_ms / 1000 + 8) == 0, "collector_exit_nonzero")
            require(server.wait(timeout=8) == 0, "c_server_exit_nonzero")
            for capture in captures:
                capture.thread.join(timeout=2)
                require(not capture.thread.is_alive() and not capture.overflow, "ram_capture_overflow_or_reader_stuck")
            records, arrivals = [], []
            for arrived, line in server_out.items:
                require(line.startswith(b"{"), "non_json_server_output")
                records.append(json.loads(line))
                arrivals.append(arrived)
            result = validate(records, arrivals, client_err.text(), server_err.text(), args.position, args.clock_model)
            result.update({"collector_variant_binary": args.variant, "monitor_forced_off_for_machine_check": True, "serial": args.serial,
                           "input_auto_scan": False, "udp_input": False, "duration_ms": args.duration_ms, "actual_c_server": True})
            return result
    finally:
        for process in reversed(children):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
        for capture in captures:
            capture.thread.join(timeout=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", help="explicit live run; requires COM port released and PX4 stream configured")
    parser.add_argument("--serial", default="COM5")
    parser.add_argument("--duration-ms", type=int, default=12000)
    parser.add_argument("--variant", choices=("headless", "interactive"), default="headless")
    parser.add_argument("--position", choices=("any", "known", "unknown"), default="any")
    parser.add_argument("--clock-model", choices=("any", "unknown"), default="any")
    args = parser.parse_args()
    if not args.run:
        print(json.dumps({"status":"PLAN_ONLY", "run_switch_required":True, "actual_c_server":str(SERVER), "input_serial":args.serial,
                          "duration_ms":args.duration_ms,"output":"summary JSON only; sensor values stay in RAM", "server_port":21000, "auto_scan":False, "udp_enabled":False}, ensure_ascii=False))
        return 0
    try:
        print(json.dumps(run(args), ensure_ascii=False))
        return 0
    except Exception as exc:
        print(json.dumps({"status":"FAIL", "reason":str(exc) if isinstance(exc, CheckFailure) else type(exc).__name__, "observations_written_to_file":False}, ensure_ascii=False))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
