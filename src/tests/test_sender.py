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

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build" / ("windows-x64" if os.name == "nt" else "linux-x64")
EXE = (Path(sys.argv[1]) if len(sys.argv) > 1 else BUILD / ("telemetry_send.exe" if os.name == "nt" else "telemetry_send")).resolve()
BASE = 1_000_000
UTC_BASE = 1_800_000_000_000_000


def clock(t=0):
    return f"@clock,{t},{UTC_BASE + t},1000,200\n"


def row(seq, t=None, key="sensor.pm25", unit="ug/m3", value=None, **kw):
    """Input: t,seq,quality,key,unit,value,p3,v3,time_uncert,nav_age,pos_error."""
    fields = [str(BASE + seq * 10_000 if t is None else t), str(seq), "7",
              key, unit, repr(10 + seq * .5 if value is None else value),
              repr(seq * .25), repr(-seq * .5), "3", "25", "-50", "0",
              "5", "1000", "0.5"]
    for i, value in kw.items():
        fields[int(i)] = str(value)
    return ",".join(fields) + "\n"


def args(url="-", summary=False):
    return [str(EXE), url, "env1", "test-session", "37", "127", "100"] + (["--summary"] if summary else [])


def run(data, summary=False, with_clock=True):
    p = subprocess.run(args(summary=summary), input=(clock() if with_clock else "") + data,
                       capture_output=True, text=True, timeout=15)
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


def finish(child, timeout=15):
    child.stdin.close()
    child.stdin = None
    return child.communicate(timeout=timeout)


# Three measurement seconds preserve all 300 moving samples and their order.
p, payloads = run("".join(row(i) for i in range(300)))
assert p.returncode == 0, p.stderr
ws = windows(payloads)
assert len(ws) == 3 and all(b["n"] == 100 for b in ws)
assert all(p["schema"] == 2 and p["clock_model"]["anchors"] == 1 for p in payloads)
assert all(p["time_basis"] == "pixhawk_boot_us" for p in payloads)
raw = [s for b in ws for s in b["raw"]]
assert len(raw) == 300
for i, s in enumerate(raw):
    assert len(s) == 17 and int(s[0]) == BASE + i * 10_000
    assert s[1:5] == [i, 7, "sensor.pm25", "ug/m3"]
    assert s[5:12] == [i * .25, -i * .5, 3, 25, -50, 0, 10 + i * .5]
    assert s[12] == 5 and s[13:15] == [1000, 0.5]
    assert s[15] == str(UTC_BASE + BASE + i * 10_000)
    assert s[16] >= 1100
assert all(raw[i][16] < raw[i + 1][16] for i in range(len(raw) - 1))
for b in ws:
    st = b["stats"][0]
    assert st["key"] == "sensor.pm25" and st["unit"] == "ug/m3" and st["n"] == 100
    assert math.isclose(st["variance"], 210.41666666666666, rel_tol=1e-12)
    uncert = [s[12] for s in b["raw"]]
    pos_error = [s[14] + math.sqrt(sum(v * v for v in s[8:11])) * s[12] / 1e6
                 for s in b["raw"]]
    assert math.isclose(st["time_uncert_mean_us"], sum(uncert) / 100, rel_tol=1e-12)
    assert st["time_uncert_max_us"] == max(uncert)
    assert math.isclose(st["position_error_mean_m"], sum(pos_error) / 100, rel_tol=1e-12)
    assert math.isclose(st["position_error_max_m"], max(pos_error), rel_tol=1e-12)
assert ws[-1]["stats"][0]["position_error_mean_m"] == ws[0]["stats"][0]["position_error_mean_m"]
assert .5 < ws[0]["stats"][0]["position_error_mean_m"] < .501
raw_bytes = sum(len(json.dumps(p, separators=(",", ":"))) for p in payloads)
p, payloads = run("".join(row(i) for i in range(300)), summary=True)
assert p.returncode == 0, p.stderr
assert all(not b["raw_included"] and b["raw"] == [] for b in windows(payloads))
assert [b["stats"] for b in windows(payloads)] == [b["stats"] for b in ws]
summary_bytes = sum(len(json.dumps(p, separators=(",", ":"))) for p in payloads)
assert summary_bytes < raw_bytes
print(f"PASS moving100Hz/UTC/uncertainty/statistics; 3s JSON raw={raw_bytes}B summary={summary_bytes}B")

# Four independent keys at each time may arrive in either key order.
metrics = [("sensor.pm25", "ug/m3"), ("sensor.temperature", "degC"),
           ("sensor.humidity", "%"), ("sensor.wind", "m/s")]
canonical = []
for order in (metrics, list(reversed(metrics))):
    p, payloads = run("".join(row(i, key=k, unit=u) for i in range(100) for k, u in order))
    assert p.returncode == 0, p.stderr
    b = windows(payloads)[0]
    assert b["n"] == b["raw_n"] == 400 and b["raw_dropped"] == 0 and b["key_n"] == 4
    assert [s["key"] for s in b["stats"]] == sorted(k for k, _ in metrics)
    canonical.append(json.dumps(b, separators=(",", ":")))
assert canonical[0] == canonical[1]
p, payloads = run(row(0) + row(1, unit="mg/m3") + row(1) +
                  row(2, key="sensor.wind", unit="m/s", value=-5))
assert p.returncode == 2 and "rejected=1 " in p.stderr, p.stderr
b = windows(payloads)[0]
assert b["n"] == 3 and b["key_n"] == 2 and b["stats"][0]["n"] == 2
assert b["stats"][1]["key"] == "sensor.wind" and b["stats"][1]["mean"] == -5
print("PASS generic new key/canonical interleaving/unit-conflict rejection")

# Missing values, invalid numbers, negative unsigned, ordering and uncertainty.
bad = row(1, value=float("nan")) + row(2, **{"6": "inf"}) + row(3, **{"1": "-1"})
bad += row(4, **{"13": "200001"}) + row(5, **{"12": "20001"})
bad += row(6) + row(6) + row(5) + row(7) + row(8)[:-1] + ",junk\n"
p, payloads = run(bad)
assert p.returncode == 2 and "rejected=7 " in p.stderr, p.stderr
b = windows(payloads)[0]
assert b["n"] == 3 and b["raw"][0][11] is None and b["stats"][0]["n"] == 2
assert b["stats"][0]["mean"] == (13 + 13.5) / 2
p, payloads = run(row(0), with_clock=False)
assert p.returncode == 0 and payloads[0]["clock_model"] is None, p.stderr
assert windows(payloads)[0]["raw"][0][15:] == [None, None]
assert windows(payloads)[0]["raw"][0][12] == 5
p, payloads = run(row(0) + row(1, t=10_000_001))
assert p.returncode == 0, p.stderr
assert sum(b["n"] for b in windows(payloads)) == 2
assert windows(payloads)[-1]["raw"][0][15:] == [None, None]
assert windows(payloads)[-1]["raw"][0][12] == 5
print("PASS strict input/NaN/order/nav-age/relative uncertainty; missing-or-stale UTC retains boot observations")

# A large jump needs a fresh clock anchor and never creates empty catch-up windows.
jump = BASE + 10_000_000_000
p, payloads = run(row(0) + clock(jump) + row(1, t=jump))
assert p.returncode == 0 and len(windows(payloads)) == 2, p.stderr

# Saturation preserves per-key statistics; queue loss is explicitly accounted.
p, payloads = run("".join(row(i, t=BASE + i * 1000) for i in range(600)))
b = windows(payloads)[0]
assert p.returncode == 2 and b["n"] == 600 and b["raw_n"] == 512 and b["raw_dropped"] == 88
assert b["stats"][0]["n"] == 600 and b["stats"][0]["mean"] == 159.75
assert int(payloads[-1]["loss"]["raw_samples"]) == 88
p, payloads = run("".join(clock(BASE + i * 1_000_000) +
                         row(i, t=BASE + i * 1_000_000) for i in range(12)))
loss = payloads[-1]["loss"]
assert p.returncode == 2 and int(loss["queue_windows"]) == 4 and int(loss["queue_samples"]) == 4, p.stderr
assert int(loss["rejected"]) == 0 and len(windows(payloads)) == 8
print("PASS refreshed-clock long gap/raw capacity/queue overflow accounted")

# Actual HTTP: first ACK fails, immutable batch identities survive retries.
server = Server(statuses=(500, 204))
try:
    env = os.environ.copy()
    env["TB_TOKEN"] = "local-test-token"
    p = subprocess.run(args(server.url), input=clock() + "".join(row(i) for i in range(300)),
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
child = None
try:
    child = subprocess.Popen(args(server.url), stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    child.stdin.write(clock())
    start = time.monotonic()
    for i in range(250):
        child.stdin.write(row(i))
        child.stdin.flush()
        remaining = start + (i + 1) * .01 - time.monotonic()
        if remaining > 0:
            time.sleep(remaining)
    _, err = finish(child)
    assert child.returncode == 0, err
    ws = windows([r[3] for r in server.records])
    raw = [s for b in ws for s in b["raw"]]
    assert [s[1] for s in raw] == list(range(250))
    assert all(server.records[i][0] - server.records[i - 1][0] >= .95
               for i in range(1, len(server.records)))
finally:
    if child is not None and child.poll() is None:
        child.kill()
        child.communicate()
    server.close()
print("PASS paced100Hz during slow HTTP; zero data loss; partial EOF flush")

# A permanently unavailable server stops boundedly at EOF and reports pending data.
server = Server(statuses=(503,) * 4)
try:
    p = subprocess.run(args(server.url), input=clock() + row(0),
                       capture_output=True, text=True, timeout=15)
    assert p.returncode == 2 and "pending=1" in p.stderr and len(server.records) == 3
finally:
    server.close()
print("PASS persistent HTTP failure retains/reports pending window and exits nonzero")

# Source watermarks, rather than local idle time, close measurement windows.
child = subprocess.Popen(args(), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
try:
    child.stdin.write(clock() + row(0))
    child.stdin.flush()
    captured = []
    reader = threading.Thread(target=lambda: captured.append(child.stdout.readline()), daemon=True)
    reader.start()
    reader.join(timeout=1.2)
    assert not captured, "Idle wall-clock time must not close a source-time window"
    child.stdin.write("@watermark,2000000\n")
    child.stdin.flush()
    reader.join(timeout=4)
    assert captured and json.loads(captured[0])["batches"][0]["n"] == 1
    child.stdin.write(row(1, t=1_999_999) + row(1, t=2_000_000))
    child.stdin.flush()
    out, err = finish(child, timeout=5)
    assert child.returncode == 2 and "rejected=1 " in err, err
    remaining = windows([json.loads(line) for line in out.splitlines()])
    assert len(remaining) == 1 and remaining[0]["raw"][0][0] == "2000000"
finally:
    if child.poll() is None:
        child.kill()
        child.communicate()
print("PASS idle stays open/explicit watermark flush/t<W rejected/t==W accepted")

# Bad configuration must fail even when live stdin stays open and silent.
child = subprocess.Popen(args("ftp://127.0.0.1/invalid"), stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
try:
    assert child.wait(timeout=3) == 1
    child.stdin.close()
    assert "Invalid configuration field: url" in child.stderr.read()
finally:
    if child.poll() is None:
        child.kill()
        child.communicate()
print("PASS invalid URL fails configuration before waiting on idle stdin")
print("ALL SENDER CHECKS PASSED")
