"""Selected-vehicle heartbeat/data freshness amid unrelated radio/GCS traffic.

Production router, disposable loopback UDP, and independently generated CRCs.
No serial device, user terminal, HTTP destination, or telemetry log is used.
"""
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from test_mavlink_router import Wire


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="router-liveness-") as temporary:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserve:
            reserve.bind(("127.0.0.1", 0))
            port = reserve.getsockname()[1]
        settings = {
            "url": "-", "stream": "liveness", "session": "liveness",
            "input_mode": "mavlink", "serial_port": "none", "auto_scan": 0,
            "udp_bind": "127.0.0.1", "udp_enabled": 1, "udp_port": port,
            "target_system": 1, "target_component": 1,
            "refresh_ms": 250, "probe_ms": 1000, "link_timeout_ms": 350,
            "reconnect_ms": 100, "duration_ms": 15000,
        }
        config = Path(temporary) / "router.ini"
        config.write_text("".join(f"{key}={value}\n" for key, value in settings.items()), encoding="utf-8")
        child = subprocess.Popen([str(executable), "--config", str(config), "--interactive"],
                                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
        lines, errors = [], []
        condition, stop = threading.Condition(), threading.Event()
        phase = "data"
        vehicle, radio, gcs = Wire(), Wire(51), Wire(255)
        heartbeat = None

        def read():
            for line in child.stderr:
                with condition:
                    lines.append(line.rstrip())
                    condition.notify_all()

        def send():
            nonlocal heartbeat
            boot = 1000
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
                    while not stop.is_set():
                        payload = b""
                        if phase != "quiet":
                            payload += radio.packet(109, struct.pack("<HHBBBBB", 0, 0, 100, 100, 100, 0, 0), 185, component=68)
                            payload += gcs.packet(0, struct.pack("<IBBBBB", 0, 6, 8, 0, 4, 3), 50, component=190)
                        if phase in ("data", "both"):
                            payload += vehicle.navigation(boot, 0)
                            boot += 40
                        if phase in ("heartbeat", "both"):
                            # Deliberately replay the identical sequence when
                            # recovering: receipt age must precede dedup.
                            if heartbeat is None or phase == "both":
                                heartbeat = vehicle.heartbeat()
                            payload += heartbeat
                        if payload:
                            source.sendto(payload, ("127.0.0.1", port))
                        stop.wait(.04)
            except Exception as error:
                errors.append(error)

        def await_line(pattern, after=0, timeout=3):
            until = time.monotonic() + timeout
            with condition:
                while True:
                    for index in range(after, len(lines)):
                        if re.search(pattern, lines[index]):
                            return index + 1
                    assert child.poll() is None and not errors, (child.returncode, errors, lines[-80:])
                    remaining = until - time.monotonic()
                    assert remaining > 0, (pattern, lines[after:])
                    condition.wait(min(remaining, .05))

        def enter(next_phase):
            nonlocal phase
            with condition:
                marker = len(lines)
                phase = next_phase
                return marker

        reader = threading.Thread(target=read, daemon=True)
        transmitter = threading.Thread(target=send, daemon=True)
        reader.start(); transmitter.start()
        try:
            await_line(r"source status=WAIT_HEARTBEAT")
            await_line(r"vehicle 1\.1 heartbeat=WAIT hb_age=unknown data=LIVE data_age=\d+ms")
            marker = enter("heartbeat")
            await_line(r"source status=LOCKED", marker)
            await_line(r"vehicle 1\.1 heartbeat=LIVE hb_age=\d+ms data=STALE data_age=\d+ms", marker)
            marker = enter("other")
            await_line(r"source status=HEARTBEAT_LOST", marker)
            await_line(r"vehicle 1\.1 heartbeat=LOST hb_age=\d+ms data=STALE data_age=\d+ms", marker)
            # The same exact heartbeat must recover without reconnecting or
            # being replaced by radio/GCS identities.
            marker = enter("heartbeat")
            await_line(r"source status=LOCKED", marker)
            await_line(r"vehicle 1\.1 heartbeat=LIVE hb_age=\d+ms data=STALE data_age=\d+ms", marker)
            marker = enter("data")
            await_line(r"source status=HEARTBEAT_LOST", marker)
            await_line(r"vehicle 1\.1 heartbeat=LOST hb_age=\d+ms data=LIVE data_age=\d+ms", marker)
            # A known sequence omission is estimated only within the selected
            # stream; radio/GCS sequence spaces must not affect its numerator.
            vehicle.sequence[1] += 3
            marker = enter("both")
            await_line(r"source status=LOCKED", marker)
            packet_marker = await_line(r"vehicle 1\.1 heartbeat=LIVE hb_age=\d+ms data=LIVE data_age=\d+ms", marker)
            packet_pattern = r"packets rx=(\d+) gap~=(\d+) loss~=([\d.]+)% dup=(\d+)"
            packet_end = await_line(packet_pattern, packet_marker)
            count = re.search(packet_pattern, lines[packet_end - 1])
            received, gaps, loss, duplicates = count.groups()
            received, gaps, duplicates = int(received), int(gaps), int(duplicates)
            assert gaps == 3 and duplicates > 0, count.group(0)
            expected_loss = 100 * gaps / (received - duplicates + gaps)
            assert abs(float(loss) - expected_loss) <= .0051, (count.group(0), expected_loss)
            marker = enter("quiet")
            await_line(r"source status=PROBING", marker)
            assert not errors, errors
            print("PASS selected heartbeat/data freshness: radio/GCS isolation, stale HB, duplicate-sequence recovery, data-only, complete silence")
        finally:
            stop.set(); transmitter.join(timeout=2)
            if child.poll() is None:
                child.terminate()
            child.wait(timeout=3)
            reader.join(timeout=2)
            child.stderr.close()


if __name__ == "__main__":
    main()
