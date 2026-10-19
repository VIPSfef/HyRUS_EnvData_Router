"""Exercise full four-window JSON with maximum keys and wide numeric strings."""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build" / ("windows-x64" if os.name == "nt" else "linux-x64")
exe = (Path(sys.argv[1]) if len(sys.argv) > 1 else BUILD / (
    "mavlink_telemetry_data_router_headless.exe" if os.name == "nt" else "mavlink_telemetry_data_router_headless")).resolve()
base = ((2**64-1)//1_000_000-4)*1_000_000
lines = [f"@clock,{base},{base-1_000_000},1000,200\n"]
for window in range(4):
    for i in range(512):
        key = "metric_"+str(i % 16).zfill(2)+"x"*22
        unit = "ug/m3"+"x"*10
        t = base+window*1_000_000+i*1000
        seq = window*32+i//16
        fields = [str(t), str(seq), str(2**32-1), key, unit,
                  "-1.2345678901234568e-300"] + ["-1.2345678901234568e-300"]*3
        fields += ["-1.23456789e-37"]*3+["20000", "200000", "1.23456789e-37"]
        lines.append(",".join(fields)+"\n")
p = subprocess.run([str(exe), "-", "limit", "limit-session", "0", "0", "0"],
                   input="".join(lines), capture_output=True, text=True, timeout=15)
assert p.returncode == 0, p.stderr
payloads = [json.loads(x) for x in p.stdout.splitlines()]
batches = [b for payload in payloads for b in payload["batches"]]
assert len(batches) == 4
assert all(b["n"] == b["raw_n"] == 512 and b["raw_dropped"] == 0 for b in batches)
assert all(len(b["stats"]) == 16 and len(b["raw"][0]) == 17 for b in batches)
assert sum(b["raw_n"] for b in batches) == 2048
print(f"PASS four full windows/16 maximum keys/64-bit times/wide doubles; JSON {len(p.stdout.encode())}B")
