"""One HTTP request budget for backlog, status, retries and EOF; UDP stays live.

Synthetic measurements and private loopback sockets only. Independent wire
timestamps verify the actual sender rather than a duplicated scheduling model.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time

from test_memory_buffer import Sensor, observations, wait_until
from test_periodic_status import Router, STATUS_SCHEMA
from test_udp_backup_router import BackupCapture, identity, weather_records


class Capture:
    def __init__(self, statuses=(), response_delay=0):
        self.requests = []
        self.errors = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def handle(self):
                try:
                    super().handle()
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def do_POST(self):
                try:
                    received = time.monotonic()
                    raw = self.rfile.read(int(self.headers["Content-Length"]))
                    index = len(owner.requests)
                    code = statuses[index] if index < len(statuses) else 204
                    record = {"at": received, "code": code, "body": json.loads(raw)}
                    owner.requests.append(record)
                    if response_delay:
                        time.sleep(response_delay)
                    self.send_response(code)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    record["completed"] = time.monotonic()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                except BaseException as error:
                    owner.errors.append(repr(error))

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


def assert_gaps(requests, minimum_ms):
    assert len(requests) >= 3, requests
    for previous, current in zip(requests, requests[1:]):
        gap = current["at"] - previous["completed"]
        assert gap >= minimum_ms / 1000 - .035, ("POST bypassed shared completion gap", gap, requests)
    if minimum_ms >= 500:
        assert all(requests[i + 2]["at"] - requests[i]["at"] >= 1.0
                   for i in range(len(requests) - 2)), "three-request catch-up burst"


def csv_backlog(executable, directory, minimum_ms, statuses=(), response_delay=0):
    with Capture(statuses, response_delay) as http:
        config = directory / f"backlog-{minimum_ms}.ini"
        settings = {"url": f"http://127.0.0.1:{http.port}/ingest", "stream": "pacing",
                    "session": "pacing", "input_mode": "csv", "max_windows": 2,
                    "qgc_enabled": 0, "udp_enabled": 0, "token_env": "MTR_PACING_TEST_TOKEN_UNUSED"}
        # Omission proves the default protects existing saved configuration.
        if minimum_ms != 500:
            settings["http_min_interval_ms"] = minimum_ms
        config.write_text("".join(f"{key}={value}\n" for key, value in settings.items()), encoding="utf-8")
        data = "".join(f"{1_000_000 + i * 1_000_000},{i},7,sensor.temp,degC,{20 + i},0,0,0,0,0,0,5,0,0\n"
                       for i in range(10))
        result = subprocess.run([str(executable), "--config", str(config)], input=data,
                                capture_output=True, text=True, timeout=15, cwd=directory)
        assert result.returncode == 0 and not result.stdout, (result.returncode, result.stderr)
        assert_gaps(http.requests, minimum_ms)
        acknowledged = [batch for request in http.requests if request["code"] == 204
                        for batch in request["body"]["batches"]]
        assert [sample[1] for batch in acknowledged for sample in batch["raw"]] == list(range(10)), acknowledged
        assert len(acknowledged) == 10 and all(len(r["body"]["batches"]) == 2 for r in http.requests)
        if statuses:
            assert http.requests[0]["body"]["batches"] == http.requests[1]["body"]["batches"] == http.requests[2]["body"]["batches"]
            assert http.requests[1]["at"] - http.requests[0]["completed"] >= .965
            assert http.requests[2]["at"] - http.requests[1]["completed"] >= 1.965
        print(f"PASS {minimum_ms} ms HTTP gap during batched EOF drain, immutable retry and complete ordered ACKs")


def live_status_and_udp(executable, directory):
    # A gap longer than the status period also verifies that due status cannot
    # starve measurement records. The first status and first data POST fail.
    minimum_ms = 1200
    with Capture((403, 403)) as http, BackupCapture() as udp:
        router = Router(executable, directory, http, "shared-pacing", duration_ms=6500,
                        http_min_interval_ms=minimum_ms, status_interval_ms=1000,
                        spatial_enabled=0, link_timeout_ms=5000, udp_backup_enabled=1,
                        udp_backup_host="127.0.0.1", udp_backup_port=udp.port,
                        token_env="MTR_PACING_TEST_TOKEN_UNUSED")
        sensor = Sensor(router.port)
        try:
            time.sleep(.35)
            for index in range(6):
                sensor.emit(index)
                wait_until(lambda: all((metric, index) in observations(udp)
                                      for metric in ("temperature", "humidity")),
                           "HTTP pacing/backoff delayed immediate UDP", timeout=.5)
                time.sleep(.25)
            router.finish(timeout=15)
            assert_gaps(http.requests, minimum_ms)
            kinds = [r["body"]["schema"] == STATUS_SCHEMA for r in http.requests]
            assert kinds[:3] == [True, False, True], ("status starved data or vice versa", kinds)
            pending, confirmed = {}, {}
            for request in http.requests:
                for record in weather_records(request["body"]):
                    key = identity(record)
                    assert pending.setdefault(key, record) == record, "403 mutated retained measurements"
                    if request["code"] == 204:
                        assert key not in confirmed, "successful measurement was sent twice"
                        confirmed[key] = record
            assert pending and pending == confirmed, "403 discarded or stranded RAM records"
            values = [sample for record in confirmed.values()
                      for groups in record["weather"]["measurements"].values()
                      for group in groups for sample in group["samples"]]
            assert len(values) == 12, (len(values), http.requests)
            assert all(received - sensor.sent[index] < .5
                       for (_, index), (received, _) in observations(udp).items())
            print("PASS status/data share pacing and 403 backoff, no starvation, UDP remains immediate and every sample reaches HTTP")
        finally:
            sensor.close()
            router.close()


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="router-http-pacing-") as temporary:
        directory = Path(temporary)
        csv_backlog(executable, directory, 500, statuses=(403, 403))
        csv_backlog(executable, directory, 700, response_delay=.15)
        live_status_and_udp(executable, directory)


if __name__ == "__main__":
    main()
