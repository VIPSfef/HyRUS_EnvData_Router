"""Immediate UDP observations and bounded, recoverable HTTP RAM backlog.

Uses only synthetic MAVLink and test-owned loopback sockets. Telemetry and logs
stay in memory; temporary files contain configuration only. The large configured
default is tested without allocating it, and saturation uses a 1 MiB budget.
"""
from concurrent.futures import ThreadPoolExecutor
import math
from pathlib import Path
import re
import socket
import struct
import sys
import tempfile
import threading
import time

from test_mavlink_router import Wire
from test_periodic_status import Router
from test_udp_backup_router import BackupCapture, HTTPFailureSink, identity, weather_records

MIB = 1024 * 1024
BUFFER = re.compile(r"tcp_buffer bytes=(\d+) limit=(\d+) high_water=(\d+)")
QUEUE = re.compile(r"queue records=(\d+)")
LOSS = re.compile(r"loss .*?queue_windows=(\d+)")


def wait_until(predicate, message, timeout=5):
    deadline = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < deadline, message
        time.sleep(.01)


class WatchedRouter(Router):
    """Read redirected TUI snapshots without opening or manipulating a window."""
    def __init__(self, *args, **options):
        super().__init__(*args, extra_args=("--interactive",), **options)
        self.lines = []
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for line in self.child.stderr:
            self.lines.append(line.decode("utf-8", "replace"))

    def last(self, pattern):
        for line in reversed(self.lines):
            match = pattern.search(line)
            if match:
                return tuple(map(int, match.groups()))
        return None

    def close(self):
        if self.child.poll() is None:
            self.child.kill()
        self.child.wait(timeout=3)
        self.reader.join(timeout=2)
        assert not self.reader.is_alive(), "router diagnostic reader did not stop"
        assert not self.child.stdout.read(), "HTTP mode emitted telemetry on stdout"
        assert self.config.read_text(encoding="utf-8") == self.original
        self.child.stdout.close()
        self.child.stderr.close()


class Sensor:
    """Real five-byte HYGROMETER packets; unique values identify each event.

    This sensor has no acquisition timestamp, so it requires no navigation or
    clock bracket before forwarding. Arrival delay is measured independently
    from the sample's intentionally unknown acquisition time.
    """
    def __init__(self, port):
        self.wire = Wire()
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.destination = "127.0.0.1", port
        self.sent = {}

    def emit(self, index):
        raw_temperature, raw_humidity = 2000 + index, 4000 + index
        payload = struct.pack("<hHB", raw_temperature, raw_humidity, 0)
        packet = self.wire.heartbeat() + self.wire.packet(12920, payload, 20)
        self.sent[index] = time.monotonic()
        self.socket.sendto(packet, self.destination)

    def close(self):
        self.socket.close()


def observations(capture):
    result = {}
    for received, _, body in list(capture.messages):
        for record in weather_records(body):
            for metric, groups in record["weather"]["measurements"].items():
                for group in groups:
                    assert group["message_id"] == 12920
                    for sample in group["samples"]:
                        value = sample["value"]
                        index = round((value - 273.15) * 100 - 2000) if metric == "temperature" else round(value * 10000 - 4000)
                        expected = (2000 + index) / 100 + 273.15 if metric == "temperature" else (4000 + index) / 10000
                        assert math.isclose(value, expected, rel_tol=1e-12), sample
                        key = metric, index
                        assert key not in result, ("UDP resent an observation", key)
                        result[key] = received, identity(record)
    return result


def confirmed_ids(capture):
    return {identity(record) for _, code, body in list(capture.requests) if code == 204
            for record in weather_records(body)}


def immediate_case(executable, directory):
    with BackupCapture() as udp, HTTPFailureSink() as http:
        router = WatchedRouter(executable, directory, http, "immediate-memory", duration_ms=0,
                               send_interval_ms=60000, status_interval_ms=0, spatial_enabled=0,
                               link_timeout_ms=3000, udp_backup_enabled=1,
                               udp_backup_host="127.0.0.1", udp_backup_port=udp.port)
        sensor = Sensor(router.port)
        try:
            wait_until(lambda: router.last(BUFFER) is not None, "router did not publish RAM diagnostics")
            assert router.last(BUFFER)[1] == 1024 * MIB, "default is not 1 GiB"
            time.sleep(.25)
            for index in range(12):
                sensor.emit(index)
                wait_until(lambda: all((metric, index) in observations(udp) for metric in ("temperature", "humidity")),
                           f"sample {index} waited for the 60-second HTTP interval", timeout=.5)
                time.sleep(.025)
            actual = observations(udp)
            assert len(actual) == 24, ("UDP did not forward every sensor observation", len(actual))
            assert all(0 <= received - sensor.sent[index] < .5 for (metric, index), (received, _) in actual.items())
            assert not http.requests, "HTTP ignored its batching interval"
            wait_until(lambda: router.last(QUEUE) and router.last(QUEUE)[0] >= 24,
                       "HTTP did not retain more than the former eight records")
            used, limit, peak = router.last(BUFFER)
            assert 0 < used <= peak < MIB and limit == 1024 * MIB, (used, limit, peak)
            assert not (router.last(LOSS) or (0,))[0], "small backlog overflowed a 1 GiB budget"
            print("PASS each UDP observation arrives within 500 ms with a 60 s HTTP interval; more than 8 records remain in default 1 GiB RAM buffer")
        finally:
            sensor.close()
            router.close()


def saturation_case(executable, directory):
    http_gap_ms, max_windows = 500, 4
    with BackupCapture() as udp, HTTPFailureSink(recover_after=3600, first_delay=.7) as http:
        router = WatchedRouter(executable, directory, http, "saturated-memory", duration_ms=0,
                               tcp_buffer_max_mb=1, send_interval_ms=1000, status_interval_ms=0,
                               connect_timeout_ms=200, io_timeout_ms=400, spatial_enabled=0,
                               link_timeout_ms=3000, udp_backup_enabled=1,
                               http_min_interval_ms=http_gap_ms, max_windows=max_windows,
                               udp_backup_host="127.0.0.1", udp_backup_port=udp.port)
        sensor = Sensor(router.port)
        try:
            wait_until(lambda: router.last(BUFFER) is not None, "router did not publish RAM diagnostics")
            assert router.last(BUFFER)[1] == MIB
            time.sleep(.25)
            start, index = time.monotonic(), 0
            while not (router.last(LOSS) or (0,))[0]:
                assert time.monotonic() - start < 25, ("fixture did not saturate 1 MiB", router.last(BUFFER))
                sensor.emit(index)
                index += 1
                time.sleep(max(0, start + index * .025 - time.monotonic()))
            used, limit, peak = router.last(BUFFER)
            assert 0 < used <= peak <= limit == MIB, (used, limit, peak)
            pending = router.last(QUEUE)[0]
            assert pending > 8, ("RAM buffer still has the former fixed queue limit", pending)

            # A full TCP backlog cannot suppress the next incoming UDP sample.
            full_marker = index
            sensor.emit(full_marker)
            wait_until(lambda: all((metric, full_marker) in observations(udp) for metric in ("temperature", "humidity")),
                       "full TCP RAM buffer suppressed UDP", timeout=.5)
            actual = observations(udp)
            full_ids = {actual[metric, full_marker][1] for metric in ("temperature", "humidity")}
            for metric in ("temperature", "humidity"):
                assert actual[metric, full_marker][0] - sensor.sent[full_marker] < .5
            failed = [identity(record) for _, code, body in http.requests if code != 204
                      for record in weather_records(body)]
            assert failed and len(failed) > len(set(failed)), "HTTP did not retain and retry its old head while UDP succeeded"

            http.recover_after = 0
            # Recovery is deliberately paced; it must not burst through the
            # server's request budget just because hundreds of records wait.
            drain_budget = math.ceil(pending / max_windows) * http_gap_ms / 1000 + 10
            wait_until(lambda: router.last(BUFFER) and router.last(BUFFER)[0] == 0,
                       "204 responses did not release the paced RAM backlog", timeout=drain_budget)
            confirmed = confirmed_ids(http)
            assert set(failed) <= confirmed, ("HTTP discarded its unacknowledged head", set(failed) - confirmed)
            assert not full_ids <= confirmed, "full-buffer newest records displaced older queued records"
            assert router.last(QUEUE)[0] == 0
            assert peak <= router.last(BUFFER)[2] <= MIB

            # Reclaimed memory is usable by later data, not just an idle counter.
            sensor.emit(full_marker + 1)
            wait_until(lambda: all((metric, full_marker + 1) in observations(udp) for metric in ("temperature", "humidity")),
                       "UDP did not resume after HTTP recovery", timeout=.5)
            actual = observations(udp)
            recovered_ids = {actual[metric, full_marker + 1][1] for metric in ("temperature", "humidity")}
            wait_until(lambda: recovered_ids <= confirmed_ids(http), "reclaimed RAM did not accept new records", timeout=3)
            wait_until(lambda: router.last(BUFFER)[0] == 0, "new 204 did not release newly allocated memory")
            for line in router.lines:
                match = BUFFER.search(line)
                if match:
                    current, configured, high_water = map(int, match.groups())
                    assert 0 <= current <= high_water <= configured == MIB
            print(f"PASS 1 MiB cap: retained {pending} records, rejected newest TCP data only, UDP continued, 204 freed and reused memory")
        finally:
            sensor.close()
            router.close()


if __name__ == "__main__":
    executable = Path(sys.argv[1]).resolve()
    assert executable.is_file(), executable
    with tempfile.TemporaryDirectory(prefix="router-memory-buffer-") as temporary:
        with ThreadPoolExecutor(max_workers=2) as pool:
            jobs = [pool.submit(test, executable, Path(temporary)) for test in (immediate_case, saturation_case)]
            for job in jobs:
                job.result()
