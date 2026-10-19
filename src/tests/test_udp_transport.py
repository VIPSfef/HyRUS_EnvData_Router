"""Independent UDP socket tests: exclusive bind, datagram integrity and isolation."""
from pathlib import Path
import re
import socket
import subprocess
import sys
import time

EXE=Path(sys.argv[1]).resolve()

def reserve(reuse=False):
    sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
    if reuse:sock.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
    sock.bind(("127.0.0.1",0))
    return sock

def free_port():
    sock=reserve();port=sock.getsockname()[1];sock.close();return port

for address in ("localhost:14551","127.0.0.1:0","127.0.0.1:65536","256.1.1.1:14551","[::1]:14551","127.0.0.1:bad"):
    run=subprocess.run([str(EXE),"udp",address],capture_output=True,text=True,timeout=3)
    assert run.returncode==3 and "OPENFAIL" in run.stdout,(address,run.stdout,run.stderr)
print("Malformed bind endpoints fail before receive: PASS")

# A Qt/QGC-style socket has already bound with address reuse enabled. Our
# collector must fail rather than join/steal the existing unicast listener.
with reserve(reuse=True) as existing:
    address=("127.0.0.1",existing.getsockname()[1])
    run=subprocess.run([str(EXE),"udp",f"{address[0]}:{address[1]}"],capture_output=True,text=True,timeout=3)
    assert run.returncode==3 and "OPENFAIL" in run.stdout,(run.stdout,run.stderr)
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sender:sender.sendto(b"still belongs to QGC",address)
    existing.settimeout(1);assert existing.recv(1024)==b"still belongs to QGC"
print("Occupied reusable listener is preserved; exclusive bind fails: PASS")

port=free_port();started=time.monotonic()
run=subprocess.run([str(EXE),"udp-idle",f"127.0.0.1:{port}"],capture_output=True,text=True,timeout=3)
assert run.returncode==4 and time.monotonic()-started<2,(run.stdout,run.stderr)
print("Idle UDP reads remain bounded and close cleanly: PASS")

for number in range(2):
    port=free_port();address=("127.0.0.1",port)
    proc=subprocess.Popen([str(EXE),"udp",f"127.0.0.1:{port}"],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    assert proc.stdout.readline().strip()=="READY"
    with reserve() as accepted,reserve() as foreign:
        # Zero-length packets cannot pin the source to an unrelated sender.
        foreign.sendto(b"",address);time.sleep(.02)
        payload=bytes(i%251 for i in range(65507)) if number==0 else b"new peer after close"
        accepted.sendto(payload,address)
        accepted.sendto(b"TAIL",address)
        for _ in range(1000):foreign.sendto(b"FOREIGN",address)
        stdout,stderr=proc.communicate(timeout=5)
        assert proc.returncode==0,(stdout[-200:],stderr)
        actual=stdout.split(" TX=")[0].strip()
        expected=payload+b"TAIL"
        assert actual==expected.hex(),(len(actual),len(expected),stdout[-200:],stderr)
        assert f"TOTAL={len(expected)}" in stdout and "TX=0" in stdout
        accepted.settimeout(.1)
        try:
            reply=accepted.recv(64);raise AssertionError(("receive-only UDP sent data",reply))
        except socket.timeout:pass
print("65507-byte datagram survives 64-byte reads, zero packet/foreign flood ignored, no outbound: PASS")
print("Close/reopen accepts a fresh sender: PASS")
