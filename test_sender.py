"""Loopback integration tests; no external server, credentials or packages."""
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parent
EXE = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "telemetry_send.exe"
BASE = 1_800_000_000_000_000


def row(seq, t=None, **kw):
    fields = [str(BASE + seq * 10_000 if t is None else t), str(seq), "7",
              repr(seq * .25), repr(-seq * .5), "3", "25", "-50", "0",
              "20", "60", repr(10 + seq * .5), "30", "1000"]
    for i, value in kw.items():
        fields[int(i)] = str(value)
    return ",".join(fields) + "\n"


def args(url="-", summary=False):
    return [str(EXE), url, "env1", "test-session", "37", "127", "100"] + (["--summary"] if summary else [])


def run(data, summary=False):
    p = subprocess.run(args(summary=summary), input=data, capture_output=True, text=True, timeout=15)
    return p, [json.loads(x) for x in p.stdout.splitlines()]


class Server:
    def __init__(self, statuses=(), delay=0):
        self.records = []
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                data = self.rfile.read(int(self.headers["Content-Length"]))
                owner.records.append((time.monotonic(), self.path, dict(self.headers), json.loads(data)))
                idx = len(owner.records) - 1
                if delay:
                    time.sleep(delay)
                status = statuses[idx] if idx < len(statuses) else 204
                try:
                    self.send_response(status)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *unused):
                pass

        self.http = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.http.daemon_threads = True
        self.thread = threading.Thread(target=self.http.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.http.server_port}/ingest?q=1"

    def close(self):
        self.http.shutdown()
        self.http.server_close()
        self.thread.join()


def windows(payloads):
    return [b for p in payloads for b in p["batches"]]


# Three measurement seconds preserve all 300 moving samples and their order.
p, payloads = run("".join(row(i) for i in range(300)))
assert p.returncode == 0, p.stderr
ws = windows(payloads)
assert len(ws) == 3 and all(b["n"] == 100 for b in ws)
raw = [s for b in ws for s in b["raw"]]
assert len(raw) == 300
for i, s in enumerate(raw):
    assert int(s[0]) == BASE + i * 10_000
    assert s[1:4] == [i, 7, i * .25]
    assert s[4:9] == [-i * .5, 3, 25, -50, 0]
    assert s[9:] == [20, 60, 10 + i * .5, 30, 1000]
for b in ws:
    assert b["stats"][2]["variance"] == 210.41666666666666
raw_bytes = sum(len(json.dumps(p, separators=(",", ":"))) for p in payloads)
p, payloads = run("".join(row(i) for i in range(300)), summary=True)
assert p.returncode == 0
assert all(not b["raw_included"] and b["raw"] == [] for b in windows(payloads))
summary_bytes = sum(len(json.dumps(p, separators=(",", ":"))) for p in payloads)
print(f"PASS moving100Hz roundtrip/statistics; 3s JSON raw={raw_bytes}B summary={summary_bytes}B")

# Missing channels, invalid numbers, negative unsigned, order, stale nav.
bad = row(1, **{"10": "nan"}) + row(2, **{"3": "inf"}) + row(3, **{"1": "-1"})
bad += row(4, **{"13": "200001"}) + row(5, **{"10": "101"})
bad += row(6) + row(6) + row(5) + row(7) + row(8)[:-1] + ",junk\n"
p, payloads = run(bad)
assert p.returncode == 2 and "rejected=7" in p.stderr, p.stderr
b = windows(payloads)[0]
assert b["n"] == 3 and b["raw"][0][10] is None and b["stats"][1]["n"] == 2
print("PASS strict input/missing channel/order/nav-age rejection")

# Thousands-of-seconds jump creates only populated windows, never a catch-up loop.
p, payloads = run(row(0) + row(1, t=BASE + 10_000_000_000))
assert p.returncode == 0 and len(windows(payloads)) == 2

# Overflow counts include all statistics; first fixed-capacity raw records retained.
p, payloads = run("".join(row(i, t=BASE + i * 1000) for i in range(200)))
b = windows(payloads)[0]
assert p.returncode == 2 and b["n"] == 200 and b["raw_n"] == 128 and b["raw_dropped"] == 72
assert b["stats"][2]["n"] == 200 and b["stats"][2]["mean"] == 59.75
p, payloads = run("".join(row(i, t=BASE + i * 1_000_000) for i in range(12)))
loss = payloads[-1]["loss"]
assert p.returncode == 2 and int(loss["queue_windows"]) == 4 and int(loss["queue_samples"]) == 4
assert len(windows(payloads)) == 8
print("PASS long gap/raw capacity/queue overflow accounted")

# Actual HTTP: first ACK fails, immutable batch identities survive retries.
server = Server(statuses=(500, 204))
try:
    env = os.environ.copy()
    env["TB_TOKEN"] = "local-test-token"
    p = subprocess.run(args(server.url), input="".join(row(i) for i in range(300)),
                       capture_output=True, text=True, env=env, timeout=15)
    assert p.returncode == 0, p.stderr
    assert len(server.records) == 2
    first, second = server.records
    assert second[0] - first[0] >= .95
    assert first[1] == "/ingest?q=1"
    assert first[2]["Authorization"] == "Bearer local-test-token"
    assert first[3]["batches"] == second[3]["batches"]
    assert len(first[3]["batches"]) == 3
finally:
    server.close()
print("PASS actual HTTP/authorization/query/500 retry/immutable windows/1Hz attempts")

# A slow HTTP exchange cannot stop a live 100Hz producer or mutate in-flight data.
server = Server(delay=.6)
try:
    child = subprocess.Popen(args(server.url), stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    start = time.monotonic()
    for i in range(250):
        child.stdin.write(row(i))
        child.stdin.flush()
        remaining = start + (i + 1) * .01 - time.monotonic()
        if remaining > 0:
            time.sleep(remaining)
    child.stdin.close()
    child.stdin = None
    out, err = child.communicate(timeout=15)
    assert child.returncode == 0, err
    ws = windows([r[3] for r in server.records])
    raw = [s for b in ws for s in b["raw"]]
    assert [s[1] for s in raw] == list(range(250))
    assert all(server.records[i][0] - server.records[i-1][0] >= .95
               for i in range(1, len(server.records)))
finally:
    server.close()
print("PASS paced100Hz during slow HTTP; zero data loss; partial EOF flush")

# A permanently unavailable server stops boundedly at EOF and reports pending data.
server = Server(statuses=(503,) * 4)
try:
    p = subprocess.run(args(server.url), input=row(0), capture_output=True, text=True, timeout=15)
    assert p.returncode == 2 and "pending=1" in p.stderr and len(server.records) == 3
finally:
    server.close()
print("PASS persistent HTTP failure retains/reports pending window and exits nonzero")

# Idle input flushes an unfinished window while stdin remains open.
child = subprocess.Popen(args(), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
child.stdin.write(row(0)); child.stdin.flush()
captured = []
reader = threading.Thread(target=lambda: captured.append(child.stdout.readline()), daemon=True)
reader.start(); reader.join(timeout=4)
assert captured and json.loads(captured[0])["batches"][0]["n"] == 1
child.stdin.close(); child.stdin = None
_, err = child.communicate(timeout=5)
assert child.returncode == 0, err
print("PASS idle flush without another sample")

# Bad configuration must fail even when live stdin stays open and silent.
child = subprocess.Popen(args("ftp://127.0.0.1/invalid"), stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
assert child.wait(timeout=3) == 2
child.stdin.close()
assert "HTTP setup failed" in child.stderr.read()
print("PASS HTTP setup failure does not block on idle stdin")
print("ALL SENDER CHECKS PASSED")
