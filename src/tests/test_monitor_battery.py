"""Battery presentation through the production router and an owned terminal.

All MAVLink traffic is synthetic, on a private loopback UDP port. The test
opens no serial device, user terminal, QGC port, or production destination.
"""
import os
from pathlib import Path
import re
import socket
import struct
import sys
import tempfile
import threading
import time

from test_mavlink_router import Wire
from test_monitor_restart import terminal_session


def battery_packet(wire, percent, voltage=16420, detailed=False, identity=3):
    """Use independent wire offsets and CRC extras, not production structs."""
    payload = bytearray(36 if detailed else 31)
    if detailed:
        struct.pack_into("<10H", payload, 10,
                         voltage if voltage is not None else 65535, *([65535] * 9))
        payload[32] = identity
        struct.pack_into("<b", payload, 35, percent)
        return wire.packet(147, payload, 154)
    struct.pack_into("<H", payload, 14, voltage if voltage is not None else 65535)
    struct.pack_into("<b", payload, 30, percent)
    return wire.packet(1, payload, 124)


class Screen:
    """Small VT reader for row content/color, including ConPTY delta output.

    ConPTY may split an application row into changed cells or normalize SGR;
    assertions therefore inspect the displayed cells, not contiguous output.
    """
    def __init__(self, terminal):
        self.terminal = terminal
        self.offset = 0
        self.pending = ""
        self.row = self.column = 0
        self.color = 39
        self.cells = [[(" ", 39)] * 160 for _ in range(48)]
        self.full_clears = 0

    def raw(self):
        if os.name == "nt":
            return self.terminal.child.text()
        with self.terminal.changed:
            return self.terminal.received.decode("utf-8", "replace")

    def erase(self, first, last):
        for position in range(first, last):
            self.cells[position // 160][position % 160] = (" ", self.color)

    def control(self, parameters, command):
        values = [int(item) if item else 0 for item in parameters.lstrip("?").split(";")]
        first = values[0]
        amount = first or 1
        if command in "Hf":
            self.row = min(47, amount - 1)
            self.column = min(159, (values[1] or 1) - 1 if len(values) > 1 else 0)
        elif command == "G":
            self.column = min(159, amount - 1)
        elif command == "d":
            self.row = min(47, amount - 1)
        elif command == "A":
            self.row = max(0, self.row - amount)
        elif command == "B":
            self.row = min(47, self.row + amount)
        elif command == "C":
            self.column = min(159, self.column + amount)
        elif command == "D":
            self.column = max(0, self.column - amount)
        elif command == "J":
            here = self.row * 160 + self.column
            if first in (2, 3):
                self.full_clears += 1
                self.erase(0, 48 * 160)
            elif first == 0:
                self.erase(here, 48 * 160)
            elif first == 1:
                self.erase(0, here + 1)
        elif command == "K":
            start = self.row * 160
            self.erase(start if first in (1, 2) else start + self.column,
                       start + self.column + 1 if first == 1 else start + 160)
        elif command == "m":
            for value in values:
                if value == 0 or value == 39:
                    self.color = 39
                elif 30 <= value <= 37 or 90 <= value <= 97:
                    self.color = value

    def refresh(self):
        raw = self.raw()
        text = self.pending + raw[self.offset:]
        self.offset = len(raw)
        self.pending = ""
        index = 0
        while index < len(text):
            value = text[index]
            if value == "\x1b":
                match = re.match(r"\x1b\[([0-9;?]*)([ -/]*[A-Za-z~])", text[index:])
                if match:
                    self.control(match[1], match[2])
                    index += len(match[0])
                    continue
                if text[index:].startswith("\x1b]"):
                    ending = re.search(r"\x07|\x1b\\", text[index + 2:])
                    if ending:
                        index += 2 + ending.end()
                        continue
                # Incomplete terminal escape remains for the next read.
                if len(text) - index < 3 or text[index + 1] in "[]":
                    self.pending = text[index:]
                    break
                index += 2
                continue
            if value == "\r":
                self.column = 0
            elif value == "\n":
                self.row = min(47, self.row + 1)
            elif value == "\b":
                self.column = max(0, self.column - 1)
            elif value >= " ":
                if self.column >= 160:
                    self.column = 0
                    self.row = min(47, self.row + 1)
                self.cells[self.row][self.column] = (value, self.color)
                self.column += 1
            index += 1

    def line(self, row):
        self.refresh()
        return "".join(value for value, _ in self.cells[row]).rstrip()

    def battery(self):
        return self.line(1)

    def await_battery(self, fragments, color, timeout=3):
        deadline = time.monotonic() + timeout
        while True:
            line = self.battery()
            family = self.cells[1][0][1]
            family = family - 60 if family >= 90 else family
            if all(fragment in line for fragment in fragments) and family == color:
                assert line.startswith("BATTERY"), line
                assert self.line(0).startswith("[monitor]"), self.line(0)
                return line
            assert self.terminal.child.poll() is None, self.raw()[-6000:]
            assert time.monotonic() < deadline, (fragments, color, line, self.cells[1][:12], self.raw()[-3000:])
            time.sleep(.025)


class Source:
    def __init__(self, port):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.destination = ("127.0.0.1", port)
        self.wire = Wire()
        self.lock = threading.Lock()
        self.done = threading.Event()
        self.heartbeats = True
        self.battery = None
        self.errors = []
        self.thread = threading.Thread(target=self.run, daemon=True)

    def run(self):
        try:
            while not self.done.is_set():
                with self.lock:
                    packets = self.wire.heartbeat() if self.heartbeats else b""
                    if self.battery is not None:
                        packets += battery_packet(self.wire, **self.battery)
                    if packets:
                        self.socket.sendto(packets, self.destination)
                self.done.wait(.08)
        except Exception as error:
            self.errors.append(error)

    def update(self, **values):
        with self.lock:
            self.battery = values

    def emit(self, factory):
        with self.lock:
            self.socket.sendto(factory(self.wire), self.destination)

    def close(self):
        self.done.set()
        self.thread.join(timeout=2)
        self.socket.close()
        assert not self.errors and not self.thread.is_alive(), self.errors


def run(executable, folder):
    # Keep the destination reserved until the source socket exists, preventing
    # ephemeral-port reuse from accidentally making the source receive itself.
    reservation = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    reservation.bind(("127.0.0.1", 0))
    port = reservation.getsockname()[1]
    source = Source(port)
    config = folder / "battery.ini"
    settings = {
        "url": "-", "stream": "battery-test", "session": "battery-test",
        "input_mode": "mavlink", "serial_port": "none", "auto_scan": 0,
        "udp_bind": "127.0.0.1", "udp_enabled": 1, "udp_port": port,
        "qgc_enabled": 0, "refresh_ms": 250, "probe_ms": 1000,
        "link_timeout_ms": 1500, "reconnect_ms": 100, "duration_ms": 0,
        "origin_auto": 0, "origin_lat": 37, "origin_lon": 127, "origin_alt": 100,
    }
    config.write_text("".join(f"{key}={value}\n" for key, value in settings.items()), encoding="utf-8")
    reservation.close()
    terminal = terminal_session(executable, config)
    source.thread.start()
    screen = Screen(terminal)
    try:
        screen.await_battery(["unknown"], 30)
        initial_clears = screen.full_clears
        for percent, color, label in ((78, 32, "OK"), (41, 32, "OK"),
                                      (40, 33, "LOW"), (21, 33, "LOW"),
                                      (20, 31, "CRITICAL"), (0, 31, "CRITICAL")):
            source.update(percent=percent)
            screen.await_battery(["16.42 V", f"{percent}%", label], color)
        source.update(percent=-1)
        voltage_only = screen.await_battery(["16.42 V", "remaining=unknown"], 39)
        assert "%" not in voltage_only and "STALE" not in voltage_only, voltage_only
        source.update(percent=40, voltage=None)
        screen.await_battery(["unknown V", "40%", "LOW"], 33)
        source.update(percent=-1, voltage=None)
        unknown = screen.await_battery(["unknown"], 30)
        assert "%" not in unknown and "16.42" not in unknown, unknown

        source.update(percent=65, voltage=15200, detailed=True)
        screen.await_battery(["#3", "15.20 V", "65%"], 32)
        with source.lock:
            source.battery = None
        # A lower-priority aggregate, another vehicle, and a corrupt packet
        # must never overwrite the selected battery's displayed values.
        ignored = (
            lambda wire: battery_packet(wire, 100, 25000),
            lambda wire: battery_packet(Wire(system=2), 95, 30000, detailed=True),
            lambda wire: (lambda packet: packet[:-1] + bytes((packet[-1] ^ 0x80,)))(
                battery_packet(wire, 55, 40000, detailed=True)),
        )
        for packet in ignored:
            source.emit(packet)
            time.sleep(.4)
            line = screen.await_battery(["#3", "15.20 V", "65%"], 32)
            assert "25.00" not in line and "30.00" not in line and "40.00" not in line, line

        source.update(percent=65, voltage=15200, detailed=True)
        with source.lock:
            source.heartbeats = False
        stale = screen.await_battery(["15.20 V", "65%", "STALE"], 30, timeout=3)
        assert int(re.search(r"rx_age=(\d+)ms", stale)[1]) < 1000, stale
        with source.lock:
            source.heartbeats = True
        recovered = screen.await_battery(["15.20 V", "65%"], 32)
        assert "STALE" not in recovered, recovered

        with source.lock:
            source.battery = None
        expired = screen.await_battery(["15.20 V", "65%", "STALE"], 30, timeout=6)
        assert int(re.search(r"rx_age=(\d+)ms", expired)[1]) >= 5000, expired
        assert "heartbeat=LIVE" in screen.line(6) or any(
            "heartbeat=LIVE" in screen.line(index) for index in range(48)), screen.raw()[-6000:]
        assert screen.full_clears == initial_clears, "periodic battery updates cleared the entire terminal"
        terminal.same_process()
        print("PASS battery TUI: top placement, voltage/percent, threshold colors, unknowns, selected-vehicle/CRC isolation, source priority, heartbeat recovery, sample expiry, delta repaint")
    finally:
        source.close()
        terminal.close()


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="monitor-battery-") as temporary:
        run(executable, Path(temporary))


if __name__ == "__main__":
    main()
