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

import physical_weather_check as checks
from physical_weather_conpty import PseudoConsole


def run(args):
    checks.require(6000 <= args.duration_ms <= 20000, "duration_outside_6_to_20_seconds")
    checks.require(re.fullmatch(r"COM[1-9][0-9]*", args.serial, re.IGNORECASE) is not None, "explicit_com_port_required")
    collector = checks.BUILD / "mavlink_telemetry_data_router_headless.exe"
    checks.require(collector.is_file() and checks.SERVER.is_file(), "required_executable_missing")
    server = client = None
    captures = []
    try:
        with tempfile.TemporaryDirectory(prefix="physical-weather-tui-config-") as directory:
            config = Path(directory) / "weather-tui.ini"
            config.write_text("\n".join(["input_mode=mavlink", "serial_port=" + args.serial, "serial_baud=115200", "auto_scan=0", "udp_enabled=0",
                "url=http://127.0.0.1:21000/ingest", "stream=physical-shtc3-tui", "session=physical-shtc3-tui", "token_env=MTR_PHYSICAL_TEST_NO_AUTH",
                "duration_ms=" + str(args.duration_ms), "send_interval_ms=1000", "origin_auto=1", "summary=1", ""]), encoding="utf-8")
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
            expected = ["temperature msg=8009", "humidity msg=8009", " RH fraction", " K", "mean=", "variance=", "boot_us=", "dt=unknown"]
            for token in expected:
                checks.require(token in screen, "tui_missing_" + token.replace(" ", "_"))
            if args.position == "unknown":
                checks.require("acquisition position=unknown" in screen and "q=24000000" in screen, "tui_unknown_position_not_explicit")
            stopped = re.search(r"STOPPED requests=(\d+) accepted=(\d+) records=(\d+) rejected=(\d+)", screen)
            checks.require(stopped is not None, "tui_stop_summary_missing")
            requests, accepted, records, rejected = map(int, stopped.groups())
            checks.require(requests == accepted == records and accepted >= 3 and rejected == 0, "tui_http204_not_all_valid")
            summary = re.search(r"rejected=(\d+) sequence_gaps=(\d+) raw_dropped=(\d+) queue_windows_dropped=(\d+) queue_samples_dropped=(\d+) pending=(\d+)", err.text())
            checks.require(summary is not None and all(int(n) == 0 for n in summary.groups()), "collector_loss_or_pending")
            return {"status":"PASS", "actual_windows_c_server_tui":True, "conpty":True, "terminal_frames_kept_in_ram":True,
                    "requests":requests,"accepted_http204":accepted,"records":records,"rejected":rejected,"pending_after_ack":0,
                    "temperature_kelvin_label":True,"humidity_fraction_label":True,"statistics_and_boot_time_visible":True,
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
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--serial", default="COM5")
    parser.add_argument("--duration-ms", type=int, default=12000)
    parser.add_argument("--position", choices=("any","unknown"), default="unknown")
    args = parser.parse_args()
    if not args.run:
        print(json.dumps({"status":"PLAN_ONLY","server":"actual Windows C TUI over ConPTY","serial":args.serial,"duration_ms":args.duration_ms,"observation_files":False}))
        return 0
    try:
        print(json.dumps(run(args),ensure_ascii=False))
        return 0
    except Exception as exc:
        print(json.dumps({"status":"FAIL","reason":str(exc) if isinstance(exc,checks.CheckFailure) else type(exc).__name__,"observations_written_to_file":False}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())