"""Real RFC6455 loopback tests for the production transport, without packages."""
import base64
import hashlib
from pathlib import Path
import socket
import struct
import subprocess
import sys
import threading
import time

EXE = Path(sys.argv[1]).resolve()

def frame(opcode, payload=b'', fin=True):
    length = len(payload)
    if length < 126:
        return bytes([(0x80 if fin else 0) | opcode, length]) + payload
    if length < 65536:
        return bytes([(0x80 if fin else 0) | opcode, 126]) + struct.pack('!H', length) + payload
    return bytes([(0x80 if fin else 0) | opcode, 127]) + struct.pack('!Q', length) + payload

def scenario(name, frames=(), expected=b'', bad_accept=False, idle=False, path='/mavlink', expected_closed=None):
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen()
    port = listener.getsockname()[1]
    errors = []
    requests = []
    replies = []
    def serve():
        conn = None
        try:
            conn, _ = listener.accept()
            conn.settimeout(4)
            request = b''
            while b'\r\n\r\n' not in request:
                part = conn.recv(4096)
                if not part:
                    raise AssertionError('Client closed before upgrade request')
                request += part
                assert len(request) < 16384
            requests.append(request)
            headers = {}
            for line in request.split(b'\r\n')[1:]:
                if b':' in line:
                    key, value = line.split(b':', 1)
                    headers[key.lower()] = value.strip()
            assert headers[b'upgrade'].lower() == b'websocket'
            assert headers[b'sec-websocket-version'] == b'13'
            nonce = headers[b'sec-websocket-key']
            assert len(base64.b64decode(nonce)) == 16
            accept = base64.b64encode(hashlib.sha1(nonce + b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
            if bad_accept:
                accept = b'badacceptvalue'
            response = (b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n'
                        b'Connection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + b'\r\n\r\n')
            conn.sendall(response)
            if bad_accept:
                return
            if idle:
                # Client must cancel an outstanding native receive on close.
                try:
                    replies.append(conn.recv(1024))
                except ConnectionResetError:
                    pass
                return
            time.sleep(.03)
            for wire, delay in frames:
                conn.sendall(wire)
                if delay:
                    time.sleep(delay)
            try:
                replies.append(conn.recv(1024))
            except (ConnectionResetError, socket.timeout):
                pass
        except Exception as error:
            errors.append(repr(error))
        finally:
            if conn is not None:
                conn.close()
    worker = threading.Thread(target=serve, daemon=True)
    worker.start()
    started = time.monotonic()
    run = subprocess.run([str(EXE), 'ws-idle' if idle else 'ws', f'ws://127.0.0.1:{port}{path}'],
                         text=True, capture_output=True, timeout=7)
    duration = time.monotonic() - started
    worker.join(timeout=5)
    listener.close()
    assert not worker.is_alive(), name + ': server did not finish'
    assert not errors, (name, errors, run.stdout, run.stderr)
    assert requests and requests[0].startswith(('GET ' + path + ' HTTP/1.1\r\n').encode()), name
    if bad_accept:
        assert run.returncode == 3 and 'OPENFAIL' in run.stdout, (name, run.stdout)
    else:
        assert run.returncode == (0 if expected else 4), (name, run.returncode, run.stdout, run.stderr)
        assert 'TOTAL=' + str(len(expected)) in run.stdout, (name, run.stdout)
        if expected:
            assert expected.hex() in run.stdout.replace('CLOSED', '\n').split('\n')[0], (name, run.stdout)
        if expected_closed:
            assert expected_closed in run.stdout, (name, run.stdout)
    if idle:
        assert duration < 2, (name, 'close blocked on receive', duration)
    print(name + ': PASS')
    return replies

scenario('Fragmented binary with interleaved ping', [
    (frame(2, b'\xfd\x09', fin=False), .01),
    (frame(9, b'ping'), .01),
    (frame(0, b'\x01\x02\x03'), .01),
    (frame(8, b'\x03\xe8'), .02)], b'\xfd\x09\x01\x02\x03', expected_closed='CLOSED')
# Header and body can themselves arrive in separate TCP packets.
payload = bytes(range(256))
wire = frame(2, payload)
scenario('Partial network header and extended payload', [(wire[:1], .01), (wire[1:3], .01),
         (wire[3:37], .01), (wire[37:], .01), (frame(8, b'\x03\xe8'), .02)], payload)
scenario('Reject invalid upgrade challenge', bad_accept=True)
scenario('Reject text data', [(frame(1, b'not MAVLink'), .02)], expected_closed='CLOSED')
scenario('Idle receive is cancellable', idle=True)
scenario('Long configured URL', [(frame(2, b'\xfe'), .01), (frame(8, b'\x03\xe8'), .02)],
         b'\xfe', path='/' + 'a' * 1800 + '?source=px4')
run = subprocess.run([str(EXE), 'enumerate'], capture_output=True, text=True, timeout=5)
assert run.returncode == 0 and 'CANDIDATES=' in run.stdout
print('Bounded serial enumeration: PASS')
if sys.platform != 'win32':
    import os
    import pty
    master, slave = pty.openpty()
    process = subprocess.Popen([str(EXE), 'serial', os.ttyname(slave)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(.15)
    os.write(master, b'\xfd\x12\x34')
    time.sleep(.1)
    os.close(master)
    stdout, stderr = process.communicate(timeout=4)
    os.close(slave)
    assert process.returncode == 0 and 'fd1234' in stdout and 'TOTAL=3' in stdout, (stdout, stderr)
    print('POSIX tty pseudo-terminal binary input: PASS')