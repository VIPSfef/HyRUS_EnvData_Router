"""Visible partial URL drafts with Enter-only validation and private redaction.

Uses a disposable PTY/ConPTY and temporary config. No source packets are sent,
and edited destinations remain drafts until the final save (no restart).
"""
from pathlib import Path
import sys
import tempfile

from test_monitor_restart import spare_port, terminal_session, write_config


def await_draft(terminal, value, marker):
    terminal.await_text(f"> {value}_", marker)
    assert "PRIVATE_" not in terminal.text(), "URL credentials appeared in terminal output"


def replace_draft(terminal, value, shown=None):
    marker = terminal.mark()
    terminal.keys(f"\x15{value}")
    await_draft(terminal, value if shown is None else shown, marker)


def open_field(terminal, index, name):
    marker = terminal.mark()
    terminal.keys(f"{index}\r")
    terminal.await_text(f"Edit {name}", marker)


def validate(terminal, valid):
    marker = terminal.mark()
    terminal.keys("\r")
    terminal.await_text("Draft validated" if valid else "Invalid setting", marker)


def main():
    executable = Path(sys.argv[1]).resolve()
    terminal = None
    try:
        with tempfile.TemporaryDirectory(prefix="url-editor-") as temporary:
            config = Path(temporary) / "router.ini"
            write_config(config, spare_port())
            with config.open("a", encoding="utf-8") as handle:
                handle.write("qgc_enabled=0\nmavlink_rates_enabled=0\nstatus_interval_ms=0\n")
            original = config.read_bytes()
            terminal = terminal_session(executable, config)
            terminal.open_editor()
            open_field(terminal, 1, "url")

            replace_draft(terminal, "h")
            marker = terminal.mark()
            terminal.keys("\x08")
            await_draft(terminal, "", marker)
            partial = ""
            for character in "http://":
                marker = terminal.mark()
                terminal.keys(character)
                partial += character
                await_draft(terminal, partial, marker)
            assert "[invalid URL]" not in terminal.text()
            assert "Invalid setting" not in terminal.text(), "Draft was validated before Enter"

            validate(terminal, False)
            assert config.read_bytes() == original, "Invalid draft changed saved configuration"
            marker = terminal.mark()
            terminal.keys("127.0.0.1:21000/ingest")
            expected_url = "http://127.0.0.1:21000/ingest"
            await_draft(terminal, expected_url, marker)
            validate(terminal, True)
            assert config.read_bytes() == original, "Enter saved an unrequested configuration"

            open_field(terminal, 1, "url")
            for prefix in ("ttp://", "http//", "http:/", ""):
                private = f"{prefix}PRIVATE_USER:PRIVATE_PASSWORD@example.com/ingest?PRIVATE_QUERY#PRIVATE_FRAGMENT"
                replace_draft(terminal, private, f"{prefix}example.com/ingest")
                validate(terminal, False)
                assert config.read_bytes() == original
            marker = terminal.mark()
            terminal.keys("\x1b")
            terminal.await_text("Edit cancelled", marker)

            open_field(terminal, 19, "websocket_url")
            replace_draft(terminal, "ws://")
            validate(terminal, False)
            marker = terminal.mark()
            terminal.keys("127.0.0.1:21002/telemetry")
            expected_websocket = "ws://127.0.0.1:21002/telemetry"
            await_draft(terminal, expected_websocket, marker)
            validate(terminal, True)
            marker = terminal.mark()
            terminal.keys("s")
            terminal.await_text("SAVED", marker)
            settings = dict(line.split("=", 1) for line in config.read_text(encoding="utf-8").splitlines()
                            if line and not line.startswith("#"))
            assert settings["url"] == expected_url
            assert settings["websocket_url"] == expected_websocket
            assert "PRIVATE_" not in terminal.text()
            terminal.same_process()
            print("PASS visible URL typing/backspace, Enter-only validation, retained invalid draft, private malformed URLs, HTTP/WebSocket save")
    finally:
        if terminal is not None:
            terminal.close()


if __name__ == "__main__":
    main()
