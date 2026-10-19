"""Verify that only HTTP 204 releases queued batches, using local loopback HTTP."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build" / ("windows-x64" if os.name == "nt" else "linux-x64")
CLOCK = "@clock,0,1800000000000000,1000,200\n"


def row(seq, t_us):
    return f"{t_us},{seq},7,pm25,ug/m3,{10 + seq},0,0,3,25,-50,0,5,1000,0.5\n"


def identities(payload):
    """The documented deduplication key, with its 64-bit time kept as a string."""
    return [(payload["stream"], payload["session"], b["window_us"])
            for b in payload["batches"]]


class LocalServer:
    def __init__(self, statuses=(204,), lose_first_response=False):
        self.records = []
        self.registered_ids = set()
        self.ack_count = 0
        self.errors = []
        self.condition = threading.Condition()
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                try:
                    body = self.rfile.read(int(self.headers["Content-Length"]))
                    payload = json.loads(body)
                    batch_ids = identities(payload)
                    with owner.condition:
                        index = len(owner.records)
                        status = statuses[index] if index < len(statuses) else 204
                        new_ids = len(set(batch_ids) - owner.registered_ids)
                        # Simulate a server that records/deduplicates before replying.
                        owner.registered_ids.update(batch_ids)
                        record = {
                            "at": time.monotonic(), "payload": payload,
                            "ids": batch_ids, "new_ids": new_ids,
                            "status": None if lose_first_response and index == 0 else status,
                        }
                        owner.records.append(record)
                        owner.condition.notify_all()
                    if lose_first_response and index == 0:
                        self.close_connection = True
                        try:
                            self.connection.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                        self.connection.close()
                        return
                    self.send_response(status)
                    self.send_header("Content-Length", "0")
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.flush()
                    if status == 204:
                        with owner.condition:
                            owner.ack_count += 1
                            owner.condition.notify_all()
                except Exception as error:
                    with owner.condition:
                        owner.errors.append(repr(error))
                        owner.condition.notify_all()

            def log_message(self, *unused):
                pass

        self.http = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.http.daemon_threads = True
        self.thread = threading.Thread(target=self.http.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.http.server_port}/confirmation"

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.http.shutdown()
        self.http.server_close()
        self.thread.join(timeout=3)

    def wait_for_ack(self, count, timeout=6):
        with self.condition:
            ready = self.condition.wait_for(
                lambda: self.ack_count >= count or self.errors, timeout=timeout)
            assert ready and self.ack_count >= count, (
                f"Expected {count} ACKs; records={len(self.records)}, errors={self.errors}")
            assert not self.errors, self.errors

    def snapshot(self):
        with self.condition:
            assert not self.errors, self.errors
            return list(self.records), set(self.registered_ids)


def command(executable, url, session):
    return [str(executable), url, "confirmation", session, "37", "127", "100"]


def no_token_environment():
    env = os.environ.copy()
    env.pop("TB_TOKEN", None)
    return env


def check_intervals(records):
    for previous, current in zip(records, records[1:]):
        elapsed = current["at"] - previous["at"]
        assert elapsed >= .95, f"Request starts were only {elapsed:.6f}s apart"


def check_clean(process):
    assert process.returncode == 0, process.stderr
    assert "pending=0" in process.stderr, process.stderr
    assert "rejected=0 " in process.stderr, process.stderr


def status_confirmation(executable):
    with LocalServer(statuses=(200, 202, 204)) as server:
        process = subprocess.run(
            command(executable, server.url, "confirm-status"),
            input=CLOCK + row(0, 1_000_000), text=True, capture_output=True,
            env=no_token_environment(), timeout=12)
        check_clean(process)
        records, registered = server.snapshot()
        assert [r["status"] for r in records] == [200, 202, 204]
        first = records[0]["payload"]["batches"]
        assert all(r["payload"]["batches"] == first for r in records)
        assert all(r["ids"] == records[0]["ids"] for r in records)
        assert len(first) == len(registered) == 1
        assert records[0]["ids"][0][2] == "1000000"
        # Until 204, both attempts must be reported as retaining the same window.
        assert process.stderr.count("Send failed; retaining 1 window(s)") == 2, process.stderr
        check_intervals(records)
    print("PASS HTTP 200 -> 202 -> 204: identical batch retained until 204; pending=0")


def lost_confirmation(executable):
    with LocalServer(lose_first_response=True) as server:
        process = subprocess.run(
            command(executable, server.url, "confirm-lost"),
            input=CLOCK + row(0, 1_000_000), text=True, capture_output=True,
            env=no_token_environment(), timeout=12)
        check_clean(process)
        records, registered = server.snapshot()
        assert len(records) >= 2 and records[0]["status"] is None
        assert records[-1]["status"] == 204
        assert len(registered) == 1 and records[0]["new_ids"] == 1
        assert all(r["new_ids"] == 0 for r in records[1:])
        assert all(r["ids"] == records[0]["ids"] for r in records)
        assert all(r["payload"]["batches"] == records[0]["payload"]["batches"]
                   for r in records)
        check_intervals(records)
    print("PASS response lost after server registration: retry has the same ID; one unique batch")


def confirmed_queue_reuse(executable):
    with LocalServer() as server:
        child = subprocess.Popen(
            command(executable, server.url, "confirm-reuse"),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env=no_token_environment())
        try:
            child.stdin.write(CLOCK + row(0, 1_000_000) + "@watermark,2000000\n")
            child.stdin.flush()
            server.wait_for_ack(1)
            # Submit another completed window only after the server has sent 204.
            child.stdin.write(row(1, 2_000_000) + "@watermark,3000000\n")
            child.stdin.flush()
            server.wait_for_ack(2)
            child.stdin.close()
            child.stdin = None
            out, err = child.communicate(timeout=6)
            assert child.returncode == 0, err
            assert "pending=0" in err and "rejected=0 " in err, err
            assert not out
            records, registered = server.snapshot()
            assert len(records) == 2 and len(registered) == 2
            assert len(records[0]["ids"]) == len(records[1]["ids"]) == 1
            assert records[0]["ids"][0][2] == "1000000"
            assert records[1]["ids"][0][2] == "2000000"
            assert set(records[0]["ids"]).isdisjoint(records[1]["ids"])
            assert records[1]["payload"]["batches"][0]["raw"][0][1] == 1
            check_intervals(records)
        finally:
            if child.poll() is None:
                child.kill()
                child.communicate(timeout=3)
    print("PASS live queue reuse after 204: next request contains only the new window; clean EOF")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", nargs="?", type=Path,
                        default=BUILD / ("mavlink_telemetry_data_router_headless.exe" if os.name == "nt" else "mavlink_telemetry_data_router_headless"))
    executable = parser.parse_args().executable.resolve()
    assert executable.is_file(), f"Sender executable not found: {executable}"
    status_confirmation(executable)
    lost_confirmation(executable)
    confirmed_queue_reuse(executable)
    print("ALL CONFIRMATION CHECKS PASSED")


if __name__ == "__main__":
    main()
