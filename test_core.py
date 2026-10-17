"""Compile/run the portable C tests and check its JSON against Python math.

No network or third-party Python packages are used. Run from any directory:
    python test_core.py [--cc /path/to/clang] [--bench]
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
EPOCH = 1_800_000_000_000_000
CAPACITY = 128


def run(argv: list[str]) -> str:
    result = subprocess.run(argv, cwd=ROOT, check=True, capture_output=True, text=True)
    if result.stderr:
        print(result.stderr, file=sys.stderr, end="")
    return result.stdout


def reject_nonfinite(value: str) -> None:
    raise AssertionError(f"Invalid JSON numeric constant: {value}")


def expect_numeric(got: object, expected: float, label: str) -> None:
    assert isinstance(got, (int, float)), (label, got)
    assert math.isclose(got, expected, rel_tol=2e-13, abs_tol=1e-11), (label, got, expected)


def channel_values(n: int, channel: int) -> list[float]:
    if channel == 0:
        return [float(i % 100) for i in range(n)]
    if channel == 1:
        return [float(40 + i % 20) for i in range(n) if i != 1]
    if channel == 2:
        return [2.5 + (i % 7) * 0.25 for i in range(n) if i != 2]
    return []


def check_fixture(executable: Path, mode: str) -> int:
    raw = run([str(executable), "--fixture", mode])
    result = json.loads(raw, parse_constant=reject_nonfinite)
    n = {"empty": 0, "single": 1, "large_time": 1, "overflow": CAPACITY + 37}.get(mode, 4)
    epoch = (2**64 - 1) // 1_000_000 * 1_000_000 if mode == "large_time" else EPOCH
    assert result["window_us"] == str(epoch)
    assert result["n"] == n
    assert result["raw_n"] == min(n, CAPACITY)
    assert result["raw_dropped"] == max(0, n - CAPACITY)
    assert result["raw_included"] == (mode != "summary")
    assert len(result["raw"]) == (0 if mode == "summary" else min(n, CAPACITY))
    assert len(result["stats"]) == 4
    for channel, stat in enumerate(result["stats"]):
        values = channel_values(n, channel)
        assert stat["n"] == len(values)
        if not values:
            assert all(stat[k] is None for k in ("mean", "variance", "min", "max"))
            continue
        expect_numeric(stat["mean"], statistics.mean(values), f"{mode}.mean[{channel}]")
        expect_numeric(stat["min"], min(values), f"{mode}.min[{channel}]")
        expect_numeric(stat["max"], max(values), f"{mode}.max[{channel}]")
        if len(values) > 1:
            expect_numeric(stat["variance"], statistics.variance(values), f"{mode}.variance[{channel}]")
        else:
            assert stat["variance"] is None
    for i, row in enumerate(result["raw"]):
        assert len(row) == 14
        expected_t = 2**64 - 1 if mode == "large_time" else epoch + i * 1000
        assert isinstance(row[0], str) and row[0] == str(expected_t)
        assert row[1:3] == [100 + i, 5]
        # Equality intentionally checks binary64 coordinates round-trip exactly.
        assert row[3] == 123456789.12345679
        assert row[4] == -987654321.01234567
        assert row[5] == 120.125 + i * 0.1
        assert row[6:9] == [35.5, -2.25, 0.125]
        assert row[9] == i % 100
        assert row[10] == (None if i == 1 else 40 + i % 20)
        assert row[11] == (None if i == 2 else 2.5 + i % 7 * 0.25)
        assert row[12] is None and row[13] == 1234
    return len(raw.encode("utf-8")) - 1  # fixture adds one newline


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", help="C99 compiler, defaults to CC/clang/gcc")
    parser.add_argument("--executable", type=Path, help="test executable path")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--bench", action="store_true", help="time 1,000,000 accepted additions")
    args = parser.parse_args()
    executable = args.executable or ROOT / ".build" / ("test_telemetry.exe" if os.name == "nt" else "test_telemetry")
    executable = executable.resolve()
    if not args.skip_build:
        compiler = args.cc or os.environ.get("CC") or shutil.which("clang") or shutil.which("gcc")
        fallback = Path(r"C:\ncg_tools\llvm-mingw-20260616-ucrt-x86_64\bin\clang.exe")
        if not compiler and fallback.is_file():
            compiler = str(fallback)
        if not compiler:
            parser.error("A C99 compiler is required; specify --cc")
        executable.parent.mkdir(parents=True, exist_ok=True)
        run([compiler, "-std=c99", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
             str(ROOT / "telemetry.c"), str(ROOT / "test_telemetry.c"), "-o", str(executable), "-lm"])
    print(run([str(executable)]), end="")
    lengths = {mode: check_fixture(executable, mode)
               for mode in ("empty", "single", "normal", "overflow", "summary", "large_time")}
    print("PASS 6 independently parsed JSON fixtures; bytes=" + json.dumps(lengths, sort_keys=True))
    if args.bench:
        bench = json.loads(run([str(executable), "--bench"]), parse_constant=reject_nonfinite)
        assert bench["samples"] == 1_000_000 and bench["seconds"] > 0
        print("MEASURED " + json.dumps(bench, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
