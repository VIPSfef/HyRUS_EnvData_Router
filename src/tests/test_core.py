"""Compile/run the portable C tests and check its JSON against Python math.

No network or third-party Python packages are used. Run from any directory:
    python src/tests/test_core.py [--cc /path/to/clang] [--bench]
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

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build" / ("windows-x64" if os.name == "nt" else "linux-x64")
EPOCH = 1_800_000_000_000_000
CAPACITY = 512


def run(argv: list[str]) -> str:
    result = subprocess.run(argv, cwd=ROOT, check=False, capture_output=True, text=True)
    if result.stderr:
        print(result.stderr, file=sys.stderr, end="")
    result.check_returncode()
    return result.stdout


def reject_nonfinite(value: str) -> None:
    raise AssertionError(f"Invalid JSON numeric constant: {value}")


def expect_numeric(got: object, expected: float, label: str) -> None:
    assert isinstance(got, (int, float)), (label, got)
    assert math.isclose(got, expected, rel_tol=2e-13, abs_tol=1e-11), (label, got, expected)


def observed_values(n: int) -> list[float]:
    return [123.12345678901235 + (i % 100) * 0.125 for i in range(n) if i != 1]


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
    assert result["key_n"] == (1 if n else 0)
    assert len(result["stats"]) == (1 if n else 0)
    for stat in result["stats"]:
        assert stat["key"] == "custom_metric" and stat["unit"] == "my_unit"
        values = [] if mode == "missing" else observed_values(n)
        assert stat["n"] == len(values)
        known_error_n = sum(i != 1 and mode != "missing" and mode != "unknown_position"
                            and not (mode == "mixed_position" and i % 2 == 0) for i in range(n))
        assert stat["position_error_n"] == known_error_n
        if not values:
            assert all(stat[k] is None for k in ("mean", "variance", "min", "max",
                "time_uncert_mean_us", "time_uncert_max_us", "position_error_mean_m", "position_error_max_m"))
            continue
        expect_numeric(stat["mean"], statistics.mean(values), f"{mode}.mean")
        expect_numeric(stat["min"], min(values), f"{mode}.min")
        expect_numeric(stat["max"], max(values), f"{mode}.max")
        if len(values) > 1:
            expect_numeric(stat["variance"], statistics.variance(values), f"{mode}.variance")
        else:
            assert stat["variance"] is None
        expect_numeric(stat["time_uncert_mean_us"], 123, f"{mode}.time_uncert_mean")
        expect_numeric(stat["time_uncert_max_us"], 123, f"{mode}.time_uncert_max")
        error = 0.25 + math.sqrt(35.5**2 + 2.25**2 + 0.125**2) * 123 / 1e6
        if known_error_n:
            expect_numeric(stat["position_error_mean_m"], error, f"{mode}.position_error_mean")
            expect_numeric(stat["position_error_max_m"], error, f"{mode}.position_error_max")
        else:
            assert stat["position_error_mean_m"] is None and stat["position_error_max_m"] is None
    for i, row in enumerate(result["raw"]):
        assert len(row) == 17
        expected_t = 2**64 - 1 if mode == "large_time" else epoch + i * 1000
        assert isinstance(row[0], str) and row[0] == str(expected_t)
        assert row[1:3] == [100 + i, 5]
        # Equality intentionally checks binary64 coordinates round-trip exactly.
        assert row[3:5] == ["custom_metric", "my_unit"]
        assert row[5] == 123456789.12345679
        assert row[6] == -987654321.01234567
        assert row[7] == 120.125 + i * 0.1
        assert row[8:11] == [35.5, -2.25, 0.125]
        assert row[11] == (None if i == 1 or mode == "missing" else 123.12345678901235 + (i % 100) * 0.125)
        assert row[12:14] == [123, 1234]
        assert row[14] == (None if mode == "unknown_position" or (mode == "mixed_position" and i % 2 == 0) else 0.25)
        expected_utc = str(2**64 - 1) if mode == "large_time" else None if i == 2 else str(EPOCH + i * 1000)
        assert row[15] == expected_utc
        assert row[16] == (4567 if expected_utc is not None else None)
    return len(raw.encode("utf-8")) - 1  # fixture adds one newline


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", help="C99 compiler, defaults to CC/clang/gcc")
    parser.add_argument("--executable", type=Path, help="test executable path")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--bench", action="store_true", help="time 1,000,000 accepted additions")
    args = parser.parse_args()
    executable = args.executable or BUILD / "tests" / ("test_telemetry.exe" if os.name == "nt" else "test_telemetry")
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
             "-I", str(ROOT / "src"), str(ROOT / "src" / "telemetry.c"),
             str(ROOT / "src" / "tests" / "test_telemetry.c"), "-o", str(executable), "-lm"])
    print(run([str(executable)]), end="")
    lengths = {mode: check_fixture(executable, mode)
               for mode in ("empty", "single", "normal", "missing", "overflow", "summary", "large_time", "unknown_position", "mixed_position")}
    print(f"PASS {len(lengths)} independently parsed JSON fixtures; bytes=" + json.dumps(lengths, sort_keys=True))
    if args.bench:
        bench = json.loads(run([str(executable), "--bench"]), parse_constant=reject_nonfinite)
        assert bench["samples"] == 1_000_000 and bench["seconds"] > 0
        print("MEASURED " + json.dumps(bench, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
