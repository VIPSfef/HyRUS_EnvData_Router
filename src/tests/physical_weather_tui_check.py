#!/usr/bin/env python3
"""Physical weather TUI check. Default plan only. No observation files.
Starts the actual Windows C server in ConPTY and captures terminal frames in RAM.
Only --run opens the chosen COM port through the collector and TCP21000 server.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from types import SimpleNamespace

import physical_weather_check as checks
from physical_weather_conpty import PseudoConsole
from test_monitor_battery import Screen


def run(args):
    checks.require_live_arguments(args)
    collector = checks.BUILD / "mavlink_telemetry_data_router_headless.exe"
    checks.require(collector.is_file() and checks.SERVER.is_file(), "required_executable_missing")
    server = client = None
    captures = []
    try:
        with tempfile.TemporaryDirectory(prefix="physical-weather-tui-config-") as directory:
            config = Path(directory) / "weather-tui.ini"
            config.write_text(checks.physical_config(args, "physical-shtc3-tui"), encoding="utf-8")
            server = PseudoConsole([str(checks.SERVER), "--bind", "127.0.0.1", "--port", "21000", "--duration-ms", str(args.duration_ms + 5000)])
            checks.require(server.ready.wait(3) and server.poll() is None, "tui_c_server_could_not_listen")
            environment = os.environ.copy()
            environment.pop("MTR_PHYSICAL_TEST_NO_AUTH", None)
            client = subprocess.Popen([str(collector), "--config", str(config), "--test-weather", "--headless", "--summary"],
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment, creationflags=subprocess.CREATE_NO_WINDOW)
            out, err = checks.PipeCapture(client.stdout), checks.PipeCapture(client.stderr)
            captures.extend([out, err])
            checks.require(client.wait(timeout=args.duration_ms / 1000 + 8) == 0, "collector_exit_nonzero")
            checks.require(server.wait(timeout=8) == 0, "tui_c_server_exit_nonzero")
            server.close()
            for capture in captures:
                capture.thread.join(timeout=2)
                checks.require(not capture.thread.is_alive() and not capture.overflow, "collector_capture_failed")
            checks.require(not server.overflow, "terminal_ram_capture_overflow")
            raw = server.text()
            screen = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", raw)
            checks.require("\x1b[" in raw and "[test server]" in screen and "LISTENING" in screen, "ansi_dashboard_not_active")
            # ConPTY can emit only changed cells, so inspect reconstructed rows
            # with the same VT reader as the synthetic dashboard regression.
            displayed = Screen(SimpleNamespace(child=server))
            compact = "\n".join(displayed.line(row) for row in range(48))
            expected = ["records=", "keys=2", "IDs=", "last_rx=", "T ", "RH ", "/8009/", "WEATHER seq="]
            for token in expected:
                checks.require(token in compact, "tui_missing_" + token.replace(" ", "_"))
            checks.require(re.search(r"T [-0-9.]+C", compact) and re.search(r"RH [0-9.]+%", compact), "tui_human_units_missing")
            if args.position == "unknown":
                checks.require("GPS -- (no coordinate received)" in compact, "tui_unknown_position_not_explicit")
            stopped = re.search(r"STOPPED requests=(\d+) accepted=(\d+) records=(\d+) rejected=(\d+)", screen)
            checks.require(stopped is not None, "tui_stop_summary_missing")
            requests, accepted, records, rejected = map(int, stopped.groups())
            checks.require(requests == accepted == records and accepted >= 3 and rejected == 0, "tui_http204_not_all_valid")
            summary = re.search(r"rejected=(\d+) sequence_gaps=(\d+) raw_dropped=(\d+) queue_windows_dropped=(\d+) queue_samples_dropped=(\d+) pending=(\d+)", err.text())
            checks.require(summary is not None and all(int(n) == 0 for n in summary.groups()), "collector_loss_or_pending")
            return {"status":"PASS", "actual_windows_c_server_tui":True, "conpty":True, "terminal_frames_kept_in_ram":True,
                    "serial":args.serial,"serial_baud":args.baud,
                    "requests":requests,"accepted_http204":accepted,"records":records,"rejected":rejected,"pending_after_ack":0,
                    "temperature_celsius_label":True,"humidity_percent_label":True,"compact_key_id_order_and_age_visible":True,
                    "unknown_position_explicit":args.position == "unknown","observations_written_to_file":False,
                    "note":"TUI content check; independent raw mean/variance is covered by the plain JSON test"}
    finally:
        if client and client.poll() is None:
            client.terminate()
            try:client.wait(timeout=3)
            except subprocess.TimeoutExpired:client.kill();client.wait(timeout=3)
        if server:server.close()
        for capture in captures:capture.thread.join(timeout=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    checks.add_run_arguments(parser)
    parser.add_argument("--position", choices=("any","unknown"), default="unknown")
    args = parser.parse_args()
    if not args.run:
        print(json.dumps({"status":"PLAN_ONLY","server":"actual Windows C TUI over ConPTY","serial":args.serial,"serial_baud":args.baud,
                          "duration_ms":args.duration_ms,"observation_files":False}))
        return 0
    try:
        print(json.dumps(run(args),ensure_ascii=False))
        return 0
    except Exception as exc:
        print(json.dumps({"status":"FAIL","reason":str(exc) if isinstance(exc,checks.CheckFailure) else type(exc).__name__,"observations_written_to_file":False}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
