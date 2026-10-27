"""Direct Bearer entry, redaction, persistence, and pending-401 recovery.

The credentials, MAVLink source, terminal, HTTP server, and configuration are
test-owned. No real credentials, devices, or application windows are accessed.
"""
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import threading

from test_monitor_restart import Source, spare_port, terminal_session, wait_for, write_config


TOKEN_SETTING = 47
TOKEN_VARIABLE = "TB_DIRECT_ENTRY_TEST_TOKEN"
ENV_TOKEN = "SYNTHETIC_ENV_CREDENTIAL_f9edac4302"
DIRECT_TOKEN = "SYNTHETIC_DIRECT_CREDENTIAL_8e047b31-._~+/=="


@contextmanager
def inherited_token():
    previous = os.environ.get(TOKEN_VARIABLE)
    os.environ[TOKEN_VARIABLE] = ENV_TOKEN
    try:
        yield
    finally:
        if previous is None:
            os.environ.pop(TOKEN_VARIABLE, None)
        else:
            os.environ[TOKEN_VARIABLE] = previous


class Capture:
    def __init__(self):
        self.records = []
        self.errors = []
        self.accepted_tokens = {DIRECT_TOKEN}
        self.changed = threading.Condition()
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                try:
                    payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                    authorization = self.headers.get("Authorization")
                    with owner.changed:
                        status = 204 if authorization in {
                            f"Bearer {value}" for value in owner.accepted_tokens
                        } else 401
                        owner.records.append((status, authorization, payload))
                        owner.changed.notify_all()
                    self.send_response(status)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                except (BrokenPipeError, ConnectionResetError):
                    pass
                except Exception as error:
                    with owner.changed:
                        owner.errors.append(repr(error))
                        owner.changed.notify_all()

            def log_message(self, *unused):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.url = f"http://127.0.0.1:{self.server.server_port}/ingest"
        self.thread = threading.Thread(target=lambda: self.server.serve_forever(.025), daemon=True)
        self.thread.start()

    def measurements(self, after=0, status=None):
        with self.changed:
            return [(code, auth, payload) for code, auth, payload in self.records[after:]
                    if payload.get("batches") and (status is None or status == code)]

    def mark(self):
        with self.changed:
            return len(self.records)

    def allow_environment(self):
        with self.changed:
            self.accepted_tokens.add(ENV_TOKEN)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        assert not self.thread.is_alive() and not self.errors, self.errors


def assert_private(terminal):
    text = terminal.text()
    for credential in (ENV_TOKEN, DIRECT_TOKEN):
        assert credential not in text, "Credential appeared in terminal output"


def apply_token(terminal, value, restart_count):
    terminal.open_editor()
    marker = terminal.mark()
    terminal.keys(f"{TOKEN_SETTING}\r")
    terminal.await_text("Edit token", marker)
    marker = terminal.mark()
    terminal.keys(f"\x15{value}")
    terminal.await_text("> ********_" if value else "> _", marker)
    assert_private(terminal)
    marker = terminal.mark()
    terminal.keys("\r")
    terminal.await_text("Draft validated", marker)
    assert_private(terminal)
    marker = terminal.mark()
    terminal.keys("r")
    terminal.await_text(f"config restarts={restart_count} refresh=250ms", marker, timeout=10)
    terminal.same_process()
    assert_private(terminal)


def saved_token(config):
    settings = dict(line.split("=", 1) for line in config.read_text(encoding="utf-8").splitlines()
                    if line and not line.startswith("#"))
    return settings["token"]


def main():
    executable = Path(sys.argv[1]).resolve()
    server = Capture()
    terminal = source = None
    try:
        with tempfile.TemporaryDirectory(prefix="direct-token-") as temporary, inherited_token():
            config = Path(temporary) / "router.ini"
            port = spare_port()
            write_config(config, port, server.url)
            with config.open("a", encoding="utf-8") as handle:
                handle.write(f"token_env={TOKEN_VARIABLE}\nqgc_enabled=0\nmavlink_rates_enabled=0\n")
            terminal = terminal_session(executable, config)
            source = Source([port])
            source.sample_count = 2
            terminal.await_text("LOCKED")
            source.sample.set()
            wait_for(lambda: bool(server.measurements(status=401)), "Missing initial rejected measurement")
            rejected = server.measurements(status=401)[0]
            assert rejected[1] == f"Bearer {ENV_TOKEN}", "Initial environment token was not used"
            original = {batch["batch_id"]: batch for batch in rejected[2]["batches"]}

            apply_token(terminal, DIRECT_TOKEN, restart_count=1)
            accepted = server.measurements(status=204)
            assert accepted and all(auth == f"Bearer {DIRECT_TOKEN}" for _, auth, _ in accepted), \
                "Direct credential did not override the environment"
            acknowledged = {batch["batch_id"]: batch for _, _, payload in accepted for batch in payload["batches"]}
            assert all(acknowledged.get(identity) == batch for identity, batch in original.items()), \
                "Credential restart lost or altered an unacknowledged measurement"
            values = {row[payload["field_order"].index("value")] for _, _, payload in accepted
                      for batch in payload["batches"] for row in batch["raw"]}
            assert values == {17, 18}, "Credential correction discarded a queued or partial observation"
            assert "Restart blocked" not in terminal.text(), "401 backlog blocked credential correction"
            stored = saved_token(config)
            if os.name == "nt":
                assert stored.startswith("dpapi:"), "Windows credential was not DPAPI-protected"
                assert DIRECT_TOKEN not in config.read_text(encoding="utf-8"), "Plain credential persisted on Windows"
            else:
                assert stored == DIRECT_TOKEN
                assert stat.S_IMODE(config.stat().st_mode) == 0o600, "Credential file is not owner-only"
            print("PASS direct token: exact Bearer, environment override, masked TUI, pending 401 batches preserved and ACKed")

            # A new process must read/decrypt the persisted token, rather than
            # accidentally relying on the editor's in-memory draft.
            assert_private(terminal)
            terminal.close()
            terminal = None
            marker = server.mark()
            terminal = terminal_session(executable, config)
            terminal.await_text("LOCKED")
            source.sample.set()
            wait_for(lambda: bool(server.measurements(marker, status=204)), "Saved credential did not authorize after process restart")
            assert all(auth == f"Bearer {DIRECT_TOKEN}" for _, auth, _ in server.measurements(marker)), \
                "Reloaded token did not override the environment"
            assert_private(terminal)

            server.allow_environment()
            apply_token(terminal, "", restart_count=1)
            assert saved_token(config) == "", "Clearing direct token did not persist"
            marker = server.mark()
            source.sample.set()
            wait_for(lambda: bool(server.measurements(marker, status=204)), "Cleared credential did not fall back to environment")
            assert all(auth == f"Bearer {ENV_TOKEN}" for _, auth, _ in server.measurements(marker)), \
                "Clearing direct token failed to restore environment selection"
            assert_private(terminal)
            print("PASS persisted token reload, clear-and-restart environment fallback, no credential output")
    finally:
        if source is not None:
            source.close()
        if terminal is not None:
            terminal.close()
        server.close()


if __name__ == "__main__":
    main()
