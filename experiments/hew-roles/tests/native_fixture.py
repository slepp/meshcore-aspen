"""Offline fixture provisioning through the production extension's local IPC."""
import hashlib
import os
import select
import struct
import subprocess
import time


def stage_wasm(worker, root, profile, module):
    root.mkdir(mode=0o700)
    for name in ("nvs", "spiffs"):
        (root/name).mkdir(mode=0o700)
    process = subprocess.Popen([str(worker), str(root)], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    def send(tag, body):
        process.stdin.write(struct.pack("<I", len(body)+1)+bytes([tag])+body)
        process.stdin.flush()
    def exact(size):
        out = b""
        deadline = time.monotonic()+10
        while len(out) < size:
            left = deadline-time.monotonic()
            if left <= 0 or not select.select([process.stdout], [], [], left)[0]:
                raise AssertionError("native fixture IPC timed out")
            data = os.read(process.stdout.fileno(), size-len(out))
            if not data:
                raise AssertionError("native fixture IPC closed")
            out += data
        return out
    def receive():
        while True:
            size, = struct.unpack("<I", exact(4))
            assert 1 <= size <= 4096
            frame = exact(size)
            if frame[0] == 129:
                # No modem is attached to the provisioning fixture.
                send(3, frame[1:5]+struct.pack("<BBIIIB", 0, 2, 0, 0, 0, 0))
            elif frame[0] == 132:
                raise AssertionError(frame)
            else:
                return frame
    def admin(command):
        send(4, struct.pack("<I", 7)+command.encode())
        while True:
            reply = receive()
            if reply[0] == 133:
                assert reply[1:5] == struct.pack("<I", 7)
                return reply[5:].decode()
    try:
        expanded = bytearray(hashlib.sha512(bytes([5])*32).digest())
        expanded[0] &= 248
        expanded[31] = expanded[31]&63 | 64
        send(1, struct.pack("<H", 1)+expanded+profile[:11]+b"\0\0"+
             struct.pack("<256I", 0, *range(11, 266)))
        while receive()[0] != 134:
            pass
        assert "wamr-2.4.1" in admin("source wasm api")
        package = (b"--@meshcore-bot/1;name=hew-arithmetic;version=1.0.0;runtime=wamr-2.4.1;"
                   b"api=meshcore-v1;caps=cmdmeta;schema=none;rollback=none\n")+module
        digest = hashlib.sha256(package).hexdigest()
        ident = digest[:16]
        assert admin(f"source wasm begin {ident} {len(package)} {digest}") == f"ACK {ident} next=0"
        for index, offset in enumerate(range(0, len(package), 48)):
            assert admin(f"source wasm chunk {ident} {index} {package[offset:offset+48].hex()}") == f"ACK {ident} next={index+1}"
        assert "Accepted verification" in admin("source wasm commit "+ident)
        for _ in range(150):
            status = admin("source wasm status")
            if "durably saved and active" in status:
                break
            assert "Error:" not in status, status
            time.sleep(.02)
        else:
            raise AssertionError(status)
        assert digest in admin("source wasm hash")
    finally:
        process.stdin.close()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        process.stdout.close()
    assert process.returncode == 0
