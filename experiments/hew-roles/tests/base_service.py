#!/usr/bin/env python3
"""One bounded local companion/TCP/source/state scenario; no physical radio."""
import base64
import hashlib
import fcntl
import urllib.request
import urllib.error
import json
import os
from pathlib import Path
import queue
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest

from parity import ROOT, public, secret, seal, unseal, packet, parse, kiss
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
sys.path.insert(0, str(ROOT))
from migrate_base import transfer, inventory, hashes

PROFILE = bytes.fromhex("c806643690d003000705020000803f010000")
BIN = Path(os.environ.get("MESHCORE_HEW_BASE_SERVICE", ROOT / "build/hew-base"))


def advert(seed, name, stamp, width=3):
    prefix = public(seed) + struct.pack("<I", stamp)
    app = b"\x81" + name.encode()
    signature = Ed25519PrivateKey.from_private_bytes(bytes([seed])*32).sign(prefix + app)
    return packet(4, prefix + signature + app, route=1, width=width)


def addressed(seed, kind, plain, recipient=1):
    return packet(kind, public(recipient)[:1] + public(seed)[:1] +
                  seal(secret(seed, public(recipient)), plain), route=2)


class Source:
    def __init__(self, port=0, negotiation_packet=None, telemetry=None, connection=None, profile=PROFILE):
        self.listener = None
        self.port = port
        if connection is None:
            self.listener = socket.socket()
            self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.listener.bind(("127.0.0.1", port))
            self.listener.listen(1)
            self.listener.settimeout(.1)
            self.port = self.listener.getsockname()[1]
        self.connection = connection
        self.stop = False
        self.error = None
        self.lock = threading.Lock()
        self.jobs = queue.Queue()
        self.controls = []
        self.completion_phase = 2
        self.negotiation_packet = negotiation_packet
        self.telemetry = telemetry or {}
        self.profile = profile
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def send(self, command, body):
        with self.lock:
            self.connection.sendall(kiss(body, 0, command))

    def receive(self, raw, snr=12, rssi=-73):
        self.send(0, raw)
        self.send(6, bytes([249, snr & 255, rssi & 255]))

    def handle(self, value):
        assert value[0] == 6
        op, data = value[1], value[2:]
        self.controls.append((op, data))
        if op == 32:
            assert data == b"\1\0", "Base attempted physical-owner HELLO"
            reply = b"\1\0" + struct.pack("<II", 1, 1) + b"\0\0"
        elif op == 37:
            assert data == b"\1", "Base attempted aggregate session reservation"
            reply = b"\1\4\5"
        elif op == 34:
            assert data == b"\1\0", "Base attempted physical configuration change"
            reply = b"\1\0" + struct.pack("<I", 17) + self.profile
        elif op == 35:
            assert data[:5] == b"\1" + struct.pack("<I", 1)
            reply = b"\1\0" + data[5:]
        elif op == 25:
            assert data == b"\1"
            reply = b"\1"
        elif op == 15:
            assert len(data) == 1
            reply = struct.pack("<I", 10 + data[0])
        elif op in (19, 16, 13, 20, 18):
            assert data == b"", "physical measurement query must be read-only"
            if op not in self.telemetry:
                self.send(6, b"\xf1\3")
                return
            reply = self.telemetry[op]
        elif op == 33:
            assert len(data) >= 19 and data[0] == 1
            generation, job = struct.unpack_from("<II", data, 1)
            assert generation == 1
            priority = data[9]
            delay, expiry = struct.unpack_from("<II", data, 10)
            assert expiry == 0
            item = {"job": job, "priority": priority, "delay": delay, "raw": data[18:]}
            self.jobs.put(item)
            for phase in ((1,) if self.completion_phase is None else (1, self.completion_phase)):
                self.send(6, b"\xfa\1" + struct.pack("<II", 1, job) +
                          bytes([phase, 0]) + bytes(12))
            return
        else:
            raise AssertionError(f"unexpected Base modem operation {op}")
        response = 154 if op == 25 else op | 128
        if op == 15 and data == b"\xff" and self.negotiation_packet is not None:
            raw = kiss(self.negotiation_packet, 0, 0)
            # One read completes negotiation but ends inside the following RF frame.
            with self.lock:
                self.connection.sendall(kiss(bytes([response])+reply, 0, 6)+raw[:12])
            time.sleep(.15)
            with self.lock:
                self.connection.sendall(raw[12:]+kiss(bytes([249, 12, 183]), 0, 6))
            return
        self.send(6, bytes([response]) + reply)

    def run(self):
        try:
            if self.listener is None:
                self.read_connection()
                return
            while not self.stop:
                try:
                    self.connection, _ = self.listener.accept()
                except socket.timeout:
                    continue
                self.read_connection()
        except BaseException as error:
            if not self.stop:
                self.error = error

    def read_connection(self):
        self.connection.settimeout(.1)
        buffer = bytearray()
        escaped = False
        while not self.stop:
            try:
                data = self.connection.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data: break
            for byte in data:
                if byte == 192:
                    if buffer: self.handle(bytes(buffer))
                    buffer.clear()
                    escaped = False
                elif escaped:
                    assert byte in (220, 221)
                    buffer.append(192 if byte == 220 else 219)
                    escaped = False
                elif byte == 219:
                    escaped = True
                else:
                    buffer.append(byte)
        self.connection.close()

    def next(self, kind, timeout=5):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            if self.error:
                raise self.error
            try:
                job = self.jobs.get(timeout=.1)
            except queue.Empty:
                continue
            if parse(job["raw"])[0] == kind:
                return job
        raise AssertionError(f"no queued packet kind={kind}")

    def close(self):
        self.stop = True
        if self.listener: self.listener.close()
        if self.connection:
            self.connection.close()
        self.thread.join(3)
        if self.error:
            raise self.error


class SharedModem:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.listener.settimeout(.1)
        self.port = self.listener.getsockname()[1]
        self.sources = []
        self.stop = False
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.stop:
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            if len(self.sources) == 4:
                connection.close()
            else:
                self.sources.append(Source(connection=connection, port=self.port))

    def receive(self, raw):
        for source in self.sources: source.receive(raw)

    def close(self):
        self.stop = True
        self.listener.close()
        self.thread.join(3)
        for source in self.sources: source.close()


class Client:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=8)
        self.socket.settimeout(8)
        self.pushes = []

    def read(self):
        def exact(size):
            data = b""
            while len(data) < size:
                chunk = self.socket.recv(size-len(data))
                if not chunk:
                    raise EOFError("companion closed")
                data += chunk
            return data
        header = exact(3)
        assert header[0] == 62
        size = int.from_bytes(header[1:], "little")
        assert 1 <= size <= 176
        return exact(size)

    def command(self, data, code=None):
        self.socket.sendall(b"<" + struct.pack("<H", len(data)) + data)
        while True:
            frame = self.read()
            if frame[0] >= 128:
                self.pushes.append(frame)
                if code == frame[0]:
                    return frame
            else:
                if code is not None:
                    assert frame[0] == code, frame.hex()
                return frame

    def push(self, code):
        for i, frame in enumerate(self.pushes):
            if frame[0] == code:
                return self.pushes.pop(i)
        while True:
            frame = self.read()
            if frame[0] == code:
                return frame
            self.pushes.append(frame)

    def close(self):
        self.socket.close()

    def wait_rx(self, raw):
        while self.push(136)[3:] != raw:
            pass


def saved_state():
    contacts = []
    for i in range(167):
        contacts.append({"PublicKey": list(public(i+2)), "Type": 3 if i == 0 else 1,
                         "Flags": 0, "OutPathLen": 128, "OutPath": [0]*64,
                         "AdvertName": f"Contact-{i}", "LastAdvert": 100+i,
                         "AdvertLatitude": 0, "AdvertLongitude": 0,
                         "LastModified": 100+i, "Advert": None, "SyncSince": 321 if i == 0 else 0,
                         "HeardPath": None, "HeardPathLen": 0, "HeardAt": 0})
    psk = bytes(range(16))
    channel = {"Name": "Synthetic", "PSK": list(psk), "Hash": hashlib.sha256(psk).digest()[0]}
    messages = []
    for i in range(256):
        frame = bytes([16, 0, 0, 0]) + public(2)[:6] + b"\xff\0" + struct.pack("<I", i+1) + f"saved-{i}".encode()
        messages.append({"Sequence": 39+i, "Frame": base64.b64encode(frame).decode(), "Digest": [0]*32})
    return {"Version": 1, "PublicKey": list(public(1)), "Name": "Birch-Base",
            "Latitude": 0, "Longitude": 0, "ClockOffset": 0, "ManualAdd": 0,
            "AdvertLocationPolicy": 0, "AutoAddConfig": 0, "AutoAddMaxHops": 0,
            "TelemetryModes": 0, "MultiACKs": 0, "Contacts": contacts,
            "Channels": [channel] + [None]*39, "Messages": messages, "Sequence": 294,
            "LastModified": 267, "Retention": "durable-replay",
            "Preferences": {"version": 1, "path_hash_mode": 2, "rxdelay": 0,
                            "txdelay": .5, "direct_txdelay": .2, "airtime_factor": 1,
                            "repeat": False, "local_advert_seconds": 0,
                            "flood_advert_seconds": 3600, "flood_max_hops": 64,
                            "unscoped_max_hops": 64, "advert_max_hops": 8,
                            "loop": 0, "default_scope": {"name": "", "key": [0]*16},
                            "wildcard_flags": 0, "regions": None, "owner_info": "",
                            "future_preference": {"enabled": True, "value": 99}}}

def load_rollback_in_go(directory, expected_public):
    before = hashes(inventory(directory))
    env = os.environ | {"MESHCORE_HEW_BASE_ROLLBACK_DIR": str(directory),
        "MESHCORE_HEW_BASE_ROLLBACK_PUBLIC": expected_public.hex(), "TMPDIR": str(ROOT/"build")}
    check = subprocess.run(["go", "test", "../../internal/companion", "-run", "^TestHewBaseRollbackAuthority$", "-count=1"],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=45)
    if check.returncode:
        raise AssertionError(check.stdout+check.stderr)
    if hashes(inventory(directory)) != before:
        raise AssertionError("Go rollback authority loading mutated the frozen candidate")


class BaseScenario(unittest.TestCase):
    def test_profile_boundaries_boot_without_retuning(self):
        root = ROOT/"build"/f"base-profile-{os.getpid()}"
        root.mkdir(mode=0o700)
        try:
            for sf, power in ((5, 22), (6, 23), (7, 2), (12, 30)):
                with self.subTest(sf=sf, reported_power=power):
                    directory = root/f"sf{sf}"
                    directory.mkdir(mode=0o700)
                    for name, data in (("identity.seed", bytes([1])*32),
                                       ("companion.json", json.dumps(saved_state()).encode())):
                        path = directory/name
                        path.write_bytes(data)
                        path.chmod(0o600)
                    profile = bytearray(PROFILE)
                    profile[8], profile[10] = sf, power
                    modem = Source(profile=bytes(profile))
                    try:
                        config = root/f"sf{sf}.json"
                        config.write_text(json.dumps({"state_dir": str(directory),
                            "radio_address": f"127.0.0.1:{modem.port}",
                            "companion_listen": "127.0.0.1:0",
                            "required_profile": profile.hex(), "run_ms": 50}))
                        config.chmod(0o600)
                        result = subprocess.run([str(BIN), str(config)], capture_output=True,
                                                text=True, timeout=10)
                        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                        self.assertIn("BASE_LISTEN ", result.stdout)
                        self.assertIn("BASE_STOP ", result.stdout)
                        self.assertIsNone(modem.error)
                        self.assertIn((34, b"\1\0"), modem.controls)
                        self.assertTrue(all(data == b"\1\0"
                                            for op, data in modem.controls if op == 34))
                    finally:
                        modem.close()
        finally:
            shutil.rmtree(root)

    def test_invalid_required_profiles_fail_before_modem_open(self):
        cases = [("", "36 hexadecimal"), (PROFILE.hex()[:-1], "36 hexadecimal"),
                 ("g"+PROFILE.hex()[1:], "non-hexadecimal")]
        for offset, fmt, value, error in (
            (0, "<I", 149999999, "frequency"), (0, "<I", 960000001, "frequency"),
            (4, "<I", 123456, "bandwidth"), (8, "<B", 4, "SF=4"),
            (8, "<B", 13, "SF=13"), (9, "<B", 4, "CR=4"),
            (9, "<B", 9, "CR=9"), (10, "<B", 31, "reported TX power=31"),
            (11, "<f", -1, "airtime factor"), (11, "<f", float("nan"), "airtime factor"),
            (11, "<f", float("inf"), "airtime factor"), (15, "<B", 2, "CAD flag=2")):
            profile = bytearray(PROFILE)
            struct.pack_into(fmt, profile, offset, value)
            cases.append((profile.hex(), error))
        root = ROOT/"build"/f"base-invalid-profile-{os.getpid()}"
        root.mkdir(mode=0o700)
        try:
            with socket.socket() as modem:
                modem.bind(("127.0.0.1", 0))
                modem.listen()
                modem.settimeout(.05)
                for profile, error in cases:
                    with self.subTest(profile=profile):
                        config = root/"config.json"
                        config.write_text(json.dumps({"state_dir": str(root/"absent-state"),
                            "radio_address": f"127.0.0.1:{modem.getsockname()[1]}",
                            "required_profile": profile}))
                        config.chmod(0o600)
                        result = subprocess.run([str(BIN), str(config)], capture_output=True,
                                                text=True, timeout=5)
                        self.assertEqual(result.returncode, 1, result.stdout+result.stderr)
                        self.assertIn("BASE_CONFIG_ERROR required_profile:", result.stdout)
                        self.assertIn(error, result.stdout)
                        self.assertTrue(result.stdout.endswith(
                            "; correct the profile in the Base JSON configuration\n"), result.stdout)
                        self.assertNotIn("panic", result.stderr.lower())
                        self.assertFalse((root/"absent-state").exists())
                        with self.assertRaises(socket.timeout):
                            connection, _ = modem.accept()
                            connection.close()
        finally:
            shutil.rmtree(root)

    def test_independent_engines_commit_refusal_and_slow_writer(self):
        root = ROOT/"build"/f"base-independent-{os.getpid()}"
        root.mkdir(mode=0o700)
        modem = SharedModem()
        clients = []
        procs = []
        try:
            directories = []
            for index, seed in enumerate((1, 4)):
                directory = root/("base" if index == 0 else "secondary")
                directory.mkdir(mode=0o700)
                directories.append(directory)
                doc = saved_state()
                doc["PublicKey"] = list(public(seed))
                doc["Messages"] = []
                doc["Sequence"] = 0
                doc["Preferences"]["flood_advert_seconds"] = 0
                if index == 1:
                    doc["Name"] = "YEG_SLP_Observer"
                    doc["Contacts"] = [doc["Contacts"][i] for i in (0, 1, 3)]
                for name, data in (("identity.seed", bytes([seed])*32), ("companion.json", json.dumps(doc).encode())):
                    path = directory/name
                    path.write_bytes(data)
                    path.chmod(0o600)
                with socket.socket() as available:
                    available.bind(("127.0.0.1", 0))
                    health_port = available.getsockname()[1]
                config = root/f"engine-{index}.json"
                config.write_text(json.dumps({"state_dir": str(directory),
                    "radio_address": f"127.0.0.1:{modem.port}", "companion_listen": "127.0.0.1:0",
                    "required_profile": PROFILE.hex(), "status_listen": f"127.0.0.1:{health_port}",
                    "status_role": "companion" if index == 0 else "bot_companion",
                    "enable_factory_reset": True}))
                config.chmod(0o600)
                proc = subprocess.Popen([str(BIN), str(config)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="backslashreplace")
                procs.append(proc)
                line = proc.stdout.readline().strip()
                self.assertTrue(line.startswith("BASE_LISTEN "), line)
                self.assertIn("public_key="+public(seed).hex(), line)
                client = Client(int(line.split("port=")[1].split()[0]))
                clients.append(client)
                client.command(b"\x16\3", 13)
                self.assertEqual(client.command(bytes([1])+bytes(7), 5)[4:36], public(seed))
                count = 167 if index == 0 else 3
                self.assertEqual(client.command(b"\4", 2), b"\2"+struct.pack("<I", count))
                self.assertEqual(sum(client.read()[0] == 3 for _ in range(count)), count)
                self.assertEqual(client.read()[0], 4)
                role = "companion" if index == 0 else "bot_companion"
                health = json.load(urllib.request.urlopen(f"http://127.0.0.1:{health_port}/status", timeout=4))
                self.assertEqual(set(health), {role})
            self.assertEqual(len(modem.sources), 2)
            self.assertEqual(clients[1].command(b"\x33reset"), b"\1\1")
            self.assertTrue(all(source.jobs.empty() for source in modem.sources))
            # A blocked atomic commit changes neither history nor its RF ACK.
            document = directories[0]/"companion.json"
            original = document.read_bytes()
            pending = directories[0]/"companion.json.pending"
            pending.write_bytes(b"synthetic blocked commit")
            pending.chmod(0o600)
            stamp = int(time.time())
            first = addressed(2, 2, struct.pack("<IB", stamp, 0)+b"after commit failure")
            modem.receive(first)
            clients[0].wait_rx(first)
            time.sleep(.1)
            self.assertTrue(modem.sources[0].jobs.empty())
            self.assertEqual(document.read_bytes(), original)
            self.assertEqual(clients[0].command(b"\x0a"), b"\x0a")
            self.assertEqual(clients[1].command(b"\x0a"), b"\x0a")
            pending.unlink()
            retried = addressed(2, 2, struct.pack("<IB", stamp, 1)+b"after commit failure")
            modem.receive(retried)
            receipt = modem.sources[0].next(3)
            self.assertEqual(receipt["delay"], 200)
            self.assertTrue(clients[0].command(b"\x0a", 16).endswith(b"after commit failure"))
            self.assertEqual(clients[1].command(b"\x0a"), b"\x0a")
            bot_message = addressed(2, 2, struct.pack("<IB", stamp+1, 0)+b"for secondary only", recipient=4)
            modem.receive(bot_message)
            modem.sources[1].next(3)
            self.assertTrue(clients[1].command(b"\x0a", 16).endswith(b"for secondary only"))
            self.assertEqual(clients[0].command(b"\x0a"), b"\x0a")
            # A slow contact-list reader is retired without starving either engine.
            slow = Client(clients[0].socket.getpeername()[1])
            clients.append(slow)
            slow.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            slow.socket.sendall((b"<\1\0\4")*40)
            time.sleep(1)
            self.assertEqual(clients[0].command(bytes([1])+bytes(7), 5)[4:36], public(1))
            self.assertEqual(clients[1].command(bytes([1])+bytes(7), 5)[4:36], public(4))
            slow.socket.settimeout(4)
            try:
                while slow.socket.recv(65536): pass
            except ConnectionResetError:
                pass
            for proc in procs: proc.terminate()
            for proc in procs:
                out, err = proc.communicate(timeout=15)
                self.assertEqual(proc.returncode, 0, out+err)
                self.assertIn("state_saved=true", out)
            load_rollback_in_go(directories[0], public(1))
            load_rollback_in_go(directories[1], public(4))
        finally:
            for c in clients: c.close()
            for proc in procs:
                if proc.poll() is None:
                    proc.terminate()
                    proc.communicate(timeout=5)
            modem.close()
            shutil.rmtree(root)

    def test_logical_restart_and_optin_factory_reset(self):
        root = ROOT/"build"/f"base-lifecycle-{os.getpid()}"
        root.mkdir(mode=0o700)
        source = Source()
        clients = []
        proc = None
        try:
            candidate = root/"candidate"
            candidate.mkdir(mode=0o700)
            doc = saved_state()
            doc["Preferences"]["flood_advert_seconds"] = 0
            for name, data in (("identity.seed", bytes([1])*32), ("companion.json", json.dumps(doc).encode())):
                path = candidate/name
                path.write_bytes(data)
                path.chmod(0o600)
            config = root/"base.json"
            cfg = {"state_dir": str(candidate), "radio_address": f"127.0.0.1:{source.port}",
                "companion_listen": "127.0.0.1:0", "required_profile": PROFILE.hex(),
                "companion_name": "Fresh-Base", "companion_policy": {"path_hash_mode": 2}}
            config.write_text(json.dumps(cfg))
            config.chmod(0o600)
            proc = subprocess.Popen([str(BIN), str(config)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="backslashreplace")
            def next_start():
                while True:
                    line = proc.stdout.readline().strip()
                    self.assertTrue(line, "Base stopped before completing its logical lifecycle")
                    if line.startswith("BASE_LISTEN "): return line
            first = next_start()
            port = int(first.split("port=")[1].split()[0])
            clients = [Client(port), Client(port)]
            for c in clients: c.command(b"\x16\3", 13)
            self.assertEqual(clients[0].command(b"\x33reset"), b"\1\1")
            self.assertEqual(clients[0].command(b"\x13invalid"), b"\1\6")
            self.assertEqual(clients[0].command(b"\x0a", 16)[16:], b"saved-0")
            cfg["enable_factory_reset"] = True
            config.write_text(json.dumps(cfg))
            with self.assertRaises(EOFError):
                clients[0].command(b"\x13reboot")
            with self.assertRaises(EOFError): clients[1].read()
            for c in clients: c.close()
            restarted = next_start()
            self.assertEqual(int(restarted.split("port=")[1].split()[0]), port)
            self.assertIn("public_key="+public(1).hex(), restarted)
            clients = [Client(port)]
            self.assertEqual(clients[0].command(bytes([1])+bytes(7), 5)[58:], b"Birch-Base")
            clients[0].command(b"\x16\3", 13)
            self.assertEqual(clients[0].command(b"\x0a", 16)[16:], b"saved-0")
            with self.assertRaises(EOFError):
                clients[0].command(b"\x33reset")
            clients[0].close()
            fresh = next_start()
            self.assertEqual(int(fresh.split("port=")[1].split()[0]), port)
            new_public = bytes.fromhex(fresh.split("public_key=")[1].split()[0])
            self.assertNotEqual(new_public, public(1))
            clients = [Client(port)]
            info = clients[0].command(bytes([1])+bytes(7), 5)
            self.assertEqual(info[4:36], new_public)
            self.assertEqual(info[58:], b"Fresh-Base")
            clients[0].command(b"\x16\3", 13)
            self.assertEqual(clients[0].command(b"\x0a"), b"\x0a")
            self.assertEqual(clients[0].command(b"\4", 2), b"\2"+bytes(4))
            self.assertEqual(clients[0].read()[0], 4)
            self.assertEqual(clients[0].command(b"\x1f\0", 18)[2:34].rstrip(b"\0"), b"Public")
            self.assertEqual((candidate/"identity.seed").read_bytes(), bytes([1])*32)
            self.assertTrue(source.jobs.empty(), "logical lifecycle sent an unsolicited RF packet")
            proc.terminate()
            out, err = proc.communicate(timeout=15)
            self.assertEqual(proc.returncode, 0, out+err)
            envelope = json.loads((candidate/"identity-state.json").read_bytes())
            self.assertEqual(envelope["state"]["Sequence"], 0)
            self.assertEqual(envelope["state"]["Contacts"], [])
            self.assertEqual(envelope["state"]["Preferences"]["path_hash_mode"], 2)
            load_rollback_in_go(candidate, new_public)
        finally:
            for c in clients: c.close()
            if proc is not None and proc.poll() is None:
                proc.terminate()
                proc.communicate(timeout=5)
            source.close()
            shutil.rmtree(root)

    def test_cached_physical_measurements_and_readonly_profile_refresh(self):
        root = ROOT/"build"/f"base-measurements-{os.getpid()}"
        root.mkdir(mode=0o700)
        source = Source(telemetry={19: struct.pack("<H", 3856), 16: struct.pack("<h", -119),
            13: struct.pack("<b", -74), 20: struct.pack("<h", -125),
            18: struct.pack("<III", 321, 123, 7)})
        clients = []
        proc = None
        try:
            candidate = root/"candidate"
            candidate.mkdir(mode=0o700)
            doc = saved_state()
            doc["Preferences"]["flood_advert_seconds"] = 0
            doc["TelemetryModes"] = 2
            with socket.socket() as available:
                available.bind(("127.0.0.1", 0))
                status_port = available.getsockname()[1]
            for name, data in (("identity.seed", bytes([1])*32), ("companion.json", json.dumps(doc).encode())):
                path = candidate/name
                path.write_bytes(data)
                path.chmod(0o600)
            config = root/"base.json"
            config.write_text(json.dumps({"state_dir": str(candidate),
                "radio_address": f"127.0.0.1:{source.port}", "companion_listen": "127.0.0.1:0",
                "status_listen": f"127.0.0.1:{status_port}", "required_profile": PROFILE.hex()}))
            config.chmod(0o600)
            proc = subprocess.Popen([str(BIN), str(config)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="backslashreplace")
            line = proc.stdout.readline().strip()
            self.assertTrue(line.startswith("BASE_LISTEN "), line)
            clients = [Client(int(line.split("port=")[1].split()[0]))]
            client = clients[0]
            client.command(b"\x16\3", 13)
            until = time.monotonic()+5
            while True:
                battery = client.command(b"\x14")
                if battery[0] == 12: break
                self.assertEqual(battery, b"\1\4")
                self.assertLess(time.monotonic(), until)
                time.sleep(.01)
            used, total = struct.unpack("<II", battery[3:])
            filesystem = os.statvfs(candidate)
            self.assertEqual(battery[1:3], struct.pack("<H", 3856))
            self.assertEqual(total, filesystem.f_blocks*filesystem.f_frsize//1024)
            self.assertLessEqual(used, total)
            status = json.load(urllib.request.urlopen(f"http://127.0.0.1:{status_port}/status", timeout=4))["companion"]
            self.assertEqual(status["telemetry_error"], "")
            self.assertEqual(status["telemetry_counter_scope"], "shared_physical_modem")
            self.assertEqual(status["telemetry"]["battery_mv"], 3856)
            self.assertEqual(status["telemetry"]["noise_floor_dbm"], -119)
            self.assertEqual(status["telemetry"]["current_rssi_dbm"], -74)
            self.assertEqual(status["telemetry"]["mcu_temperature_c"], -12.5)
            self.assertEqual(status["telemetry"]["counters"], {"PacketsRecv": 321, "PacketsSent": 123, "PacketsErrors": 7})
            self.assertEqual(client.command(b"\x38\2", 24),
                bytes([24, 2])+struct.pack("<7I", 321, 123, 0, 0, 0, 0, 7))
            self.assertEqual(client.command(b"\x38\0"), b"\1\1")
            self.assertEqual(client.command(b"\x38\1"), b"\1\1")
            self.assertEqual(client.command(b"\x27\0\0\0", 139),
                b"\x8b\0"+public(1)[:6]+bytes([1, 116, 1, 129]))
            request = addressed(2, 0, struct.pack("<IBB", 99123, 3, 0))
            source.receive(request)
            job = source.next(1)
            self.assertEqual(job["delay"], 300)
            self.assertEqual(unseal(secret(2, public(1)), parse(job["raw"])[4][2:]),
                struct.pack("<I", 99123)+bytes([1, 116, 1, 129])+bytes(8))
            # A changed physical profile is never repaired with a tuning command.
            prior_readbacks = sum(op == 34 for op, _ in source.controls)
            source.profile = PROFILE[:8]+b"\x08"+PROFILE[9:]
            until = time.monotonic()+20
            while sum(op == 34 for op, _ in source.controls) <= prior_readbacks:
                self.assertLess(time.monotonic(), until)
                time.sleep(.05)
            time.sleep(.1)
            self.assertEqual(client.command(b"\x14"), b"\1\4")
            self.assertEqual(client.command(b"\x0a", 16)[16:], b"saved-0")
            self.assertTrue(all(data == b"\1\0" for op, data in source.controls if op == 34))
            proc.terminate()
            out, err = proc.communicate(timeout=15)
            self.assertEqual(proc.returncode, 0, out+err)
            self.assertIn("physical-readback-or-telemetry-query-failed", out)
        finally:
            for c in clients: c.close()
            if proc is not None and proc.poll() is None:
                proc.terminate()
                try:
                    out, err = proc.communicate(timeout=5)
                    if proc.returncode: print(out, err, file=sys.stderr)
                finally:
                    source.close()
                    shutil.rmtree(root)
            else:
                source.close()
                shutil.rmtree(root)

    def test_source_reconnect_retains_tcp_cursors_and_health_role(self):
        root = ROOT/"build"/f"base-recovery-{os.getpid()}"
        root.mkdir(mode=0o700)
        source = Source()
        clients = []
        proc = None
        try:
            candidate = root/"candidate"
            candidate.mkdir(mode=0o700)
            doc = saved_state()
            doc["Preferences"]["flood_advert_seconds"] = 0
            for name, data in (("identity.seed", bytes([1])*32), ("companion.json", json.dumps(doc).encode())):
                path = candidate/name
                path.write_bytes(data)
                path.chmod(0o600)
            with socket.socket() as available:
                available.bind(("127.0.0.1", 0))
                status_port = available.getsockname()[1]
            config = root/"base.json"
            config.write_text(json.dumps({"state_dir": str(candidate), "radio_address": f"127.0.0.1:{source.port}",
                "companion_listen": "127.0.0.1:0", "status_listen": f"127.0.0.1:{status_port}",
                "status_role": "bot_companion", "required_profile": PROFILE.hex()}))
            config.chmod(0o600)
            proc = subprocess.Popen([str(BIN), str(config)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            line = proc.stdout.readline().strip()
            self.assertTrue(line.startswith("BASE_LISTEN "), line)
            port = int(line.split("port=")[1].split()[0])
            clients = [Client(port), Client(port)]
            for c in clients:
                c.command(b"\x16\3", 13)
                c.command(b"\x0a", 16)
            def fetch(route, failed=False):
                try:
                    return json.load(urllib.request.urlopen(f"http://127.0.0.1:{status_port}/{route}", timeout=4))
                except urllib.error.HTTPError as error:
                    if failed and error.code == 503:
                        return json.load(error)
                    raise
            before = fetch("status")["bot_companion"]
            self.assertEqual(fetch("readyz")["not_ready"], {})
            source.completion_phase = None
            clients[0].command(b"\2\0\0"+struct.pack("<I", int(time.time()))+public(2)[:6]+b"uncertain on disconnect", 6)
            source.next(2)
            radio_port = source.port
            source.close()
            offline = fetch("readyz", True)
            self.assertFalse(offline["ready"])
            self.assertIn("disconnected", offline["not_ready"]["bot_companion"])
            # Local commands continue while the detached connector retries.
            self.assertEqual(clients[0].command(b"\x0a", 16)[16:], b"saved-1")
            self.assertEqual(clients[1].command(b"\x0a", 16)[16:], b"saved-1")
            self.assertEqual(clients[0].command(b"\2\0\0"+struct.pack("<I", int(time.time()))+public(2)[:6]+b"offline"), b"\1\4")
            recovering_advert = advert(200, "Recovered contact", int(time.time()))
            source = Source(radio_port, negotiation_packet=recovering_advert)
            deadline = time.monotonic()+12
            while time.monotonic() < deadline:
                if fetch("readyz", True)["ready"]:
                    break
            else:
                self.fail("source did not reconnect")
            after = fetch("status")["bot_companion"]
            self.assertEqual(after["public_key"], before["public_key"])
            self.assertEqual(after["application_started_at"], before["application_started_at"])
            self.assertEqual(after["role_tx"]["unknown"], 1)
            self.assertFalse(after["role_tx"]["airtime_complete"])
            self.assertTrue(source.jobs.empty(), "an uncertain old-source packet was replayed")
            for c in clients:
                self.assertEqual(c.push(138)[1:33], public(200))
            self.assertEqual(clients[0].command(b"\x0a", 16)[16:], b"saved-2")
            self.assertEqual(clients[1].command(b"\x0a", 16)[16:], b"saved-2")
            clients[0].command(b"\2\0\0"+struct.pack("<I", int(time.time()))+public(2)[:6]+b"recovered", 6)
            job = source.next(2)
            self.assertEqual(job["job"], 1)
            self.assertEqual(unseal(secret(2, public(1)), parse(job["raw"])[4][2:])[5:14], b"recovered")
            # HTTP/1.0 and split request headers are accepted with close framing.
            with socket.create_connection(("127.0.0.1", status_port), timeout=4) as connection:
                connection.sendall(b"GET /sta")
                time.sleep(.03)
                connection.sendall(b"tus HTTP/1.0\r\nConnection: close\r\n\r\n")
                response = b""
                while data := connection.recv(4096):
                    response += data
            self.assertIn(b"200 OK", response)
            self.assertIn("bot_companion", json.loads(response.split(b"\r\n\r\n", 1)[1]))
            proc.terminate()
            out, err = proc.communicate(timeout=15)
            self.assertEqual(proc.returncode, 0, out+err)
            self.assertIn("BASE_SOURCE state=connected", out)
        finally:
            for c in clients:
                c.close()
            if proc is not None and proc.poll() is None:
                proc.terminate()
                try:
                    proc.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.communicate()
            source.close()
            shutil.rmtree(root)

    def test_routing_delay_trace_limits_and_actual_outcomes(self):
        root = ROOT / "build" / f"base-routing-{os.getpid()}"
        root.mkdir(mode=0o700)
        source = Source()
        clients = []
        proc = None
        try:
            candidate = root / "candidate"
            candidate.mkdir(mode=0o700)
            doc = saved_state()
            doc["Messages"], doc["Sequence"], doc["MultiACKs"] = [], 0, 1
            doc["Preferences"].update(repeat=True, rxdelay=10, flood_advert_seconds=0)
            for name, data in (("identity.seed", bytes([1])*32), ("companion.json", json.dumps(doc).encode())):
                path = candidate / name
                path.write_bytes(data)
                path.chmod(0o600)
            with socket.socket() as available:
                available.bind(("127.0.0.1", 0))
                status_port = available.getsockname()[1]
            config = root / "base.json"
            config.write_text(json.dumps({"state_dir": str(candidate), "radio_address": f"127.0.0.1:{source.port}",
                "companion_listen": "0.0.0.0:0", "companion_allow_remote": True,
                "status_listen": f"127.0.0.1:{status_port}", "required_profile": PROFILE.hex()}))
            config.chmod(0o600)
            proc = subprocess.Popen([str(BIN), str(config)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            line = proc.stdout.readline().strip()
            self.assertTrue(line.startswith("BASE_LISTEN "), line)
            port = int(line.split("port=")[1].split()[0])
            clients = [Client(port), Client(port)]
            with socket.create_connection(("::1", port), timeout=3) as ipv6:
                ipv6.sendall(b"<\2\0\x16\3")
                self.assertEqual(ipv6.recv(3), b">"+struct.pack("<H", 82))
            # Poor-signal flood holds reception before appending this radio's
            # three-byte hash. A duplicate is logged but never forwarded twice.
            raw = bytes([9, 129]) + b"abc" + b"no-target"
            began = time.monotonic()
            source.receive(raw, snr=-24)
            forwarded = source.next(2)
            self.assertGreater(time.monotonic() - began, .05)
            self.assertEqual(forwarded["raw"], bytes([9, 130])+b"abc"+public(1)[:3]+b"no-target")
            self.assertEqual(forwarded["priority"], 2)
            self.assertLess(forwarded["delay"], 5*int((10+len(forwarded["raw"]))*.5)+1)
            clients[0].wait_rx(raw)
            source.receive(raw, snr=-24)
            clients[0].wait_rx(raw)
            with self.assertRaises(queue.Empty):
                source.jobs.get(timeout=.15)
            # Direct multipart ACK relay preserves native 300 ms copy spacing.
            ack_raw = bytes([14, 130])+public(1)[:3]+public(2)[:3]+b"\x12\x34\x56\x78"
            source.receive(ack_raw)
            multipart, ack = source.next(10), source.next(3)
            self.assertEqual(multipart["raw"], bytes([42, 129])+public(2)[:3]+b"\x13\x12\x34\x56\x78")
            self.assertEqual(ack["raw"], bytes([14, 129])+public(2)[:3]+b"\x12\x34\x56\x78")
            self.assertEqual(multipart["delay"], ack["delay"])
            self.assertGreaterEqual(ack["delay"], 300)
            # Eight-byte trace hashes use an SNR path, not ordinary route hashes.
            tag = b"trace123"
            trace_body = tag+b"\3"+public(2)[:8]+public(1)[:8]
            self.assertEqual(clients[0].command(b"\x24"+trace_body, 6)[2:6], tag[:4])
            source.next(9)
            source.receive(bytes([38, 2, 4, 8])+trace_body)
            traced = clients[0].push(137)
            self.assertEqual(traced, b"\x89\0\x10\3"+trace_body[:8]+trace_body[9:]+bytes([4, 8, 12]))
            relay_body = b"relay123"+b"\3"+public(1)[:8]+public(2)[:8]
            source.receive(bytes([38, 0])+relay_body)
            relayed = source.next(9)
            self.assertEqual(relayed["raw"], bytes([38, 1, 12])+relay_body)
            self.assertEqual(relayed["priority"], 5)
            # The source's uncertain outcome must not become a success or a
            # fabricated RF ACK. The status uses actual terminal airtime only.
            source.completion_phase = 4
            clients[0].command(b"\2\0\0"+struct.pack("<I", int(time.time()))+public(2)[:6]+b"unknown", 6)
            source.next(2)
            until = time.monotonic()+4
            while True:
                with urllib.request.urlopen(f"http://127.0.0.1:{status_port}/status", timeout=4) as response:
                    status = json.load(response)
                role = status["companion"]
                if role["role_tx"]["unknown"] != 0 or time.monotonic() >= until:
                    break
                time.sleep(.01)
            self.assertTrue(role["application_started_at"].endswith("Z"))
            self.assertEqual(role["role_tx"]["unknown"], 1)
            self.assertFalse(role["role_tx"]["airtime_complete"])
            self.assertEqual(role["role_tx"]["reported_rf_airtime_ns"], 0)
            self.assertEqual(role["role_tx"]["last_problem"]["state"], "unknown")
            with urllib.request.urlopen(f"http://127.0.0.1:{status_port}/readyz", timeout=4) as response:
                self.assertEqual(response.status, 200)
                readiness = json.load(response)
                self.assertTrue(readiness["ready"])
                self.assertEqual(readiness["not_ready"], {})
                self.assertFalse(readiness["mqtt_connection_checked"])
            # Source queue and TCP client limits match production-sized limits.
            source.completion_phase = None
            for index in range(32):
                sent = clients[0].command(b"\2\0\0"+struct.pack("<I", int(time.time()))+public(2)[:6]+f"queued-{index}".encode(), 6)
                self.assertEqual(sent[0], 6)
                source.next(2)
            self.assertEqual(clients[0].command(b"\2\0\0"+struct.pack("<I", int(time.time()))+public(2)[:6]+b"overflow"), b"\1\4")
            for _ in range(30):
                c = Client(port)
                c.command(b"\x16\3", 13)
                clients.append(c)
            extra = Client(port)
            with self.assertRaises((EOFError, ConnectionResetError)):
                extra.command(b"\x16\3", 13)
            extra.close()
            clients[-1].socket.sendall(b"<"+struct.pack("<H", 177))
            with self.assertRaises((EOFError, ConnectionResetError)):
                clients[-1].read()
            proc.terminate()
            out, err = proc.communicate(timeout=15)
            self.assertEqual(proc.returncode, 0, out+err)
            self.assertIn("unknown=33", out)
            saved = json.loads((candidate/"companion.json").read_bytes())
            self.assertTrue(saved["Preferences"]["repeat"])
            self.assertEqual(saved["Preferences"]["rxdelay"], 10)
        finally:
            for c in clients:
                c.close()
            if proc is not None and proc.poll() is None:
                proc.terminate()
                try:
                    proc.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.communicate()
            source.close()
            shutil.rmtree(root)

    def test_frozen_input_admission(self):
        root = ROOT / "build" / f"base-frozen-{os.getpid()}"
        root.mkdir(mode=0o700)
        try:
            frozen = root / "frozen"
            frozen.mkdir(mode=0o700)
            for name, data in (("identity.seed", bytes([1])*32), ("companion.json", json.dumps(saved_state()).encode())):
                path = frozen / name
                path.write_bytes(data)
                path.chmod(0o600)
            before = hashes(inventory(frozen))
            with self.assertRaisesRegex(ValueError, "inside"):
                transfer(frozen, frozen/"nested", "stage")
            staging = root/"candidate.staging"
            staging.mkdir(mode=0o700)
            marker = staging/"keep"
            marker.write_bytes(b"unchanged")
            with self.assertRaisesRegex(ValueError, "staging"):
                transfer(frozen, root/"candidate", "stage")
            self.assertEqual(marker.read_bytes(), b"unchanged")
            with open(frozen/".lock", "wb") as locked:
                (frozen/".lock").chmod(0o600)
                fcntl.flock(locked, fcntl.LOCK_EX | fcntl.LOCK_NB)
                with self.assertRaises(BlockingIOError):
                    transfer(frozen, root/"locked", "stage")
            authority = frozen/"identity-state.json"
            authority.write_bytes(b"broken authority")
            authority.chmod(0o600)
            with self.assertRaises(ValueError):
                transfer(frozen, root/"corrupt", "stage")
            authority.unlink()
            self.assertEqual(hashes(inventory(frozen)), before)
            expanded = bytearray(hashlib.sha512(bytes([202])*32).digest())
            expanded[0] &= 248
            expanded[31] = (expanded[31] & 63) | 64
            authority.write_text(json.dumps({"version": 1, "document": "companion.json",
                "identity": base64.b64encode(expanded).decode(), "state": None}))
            authority.chmod(0o600)
            reset_before = hashes(inventory(frozen))
            staged = root / "reset-candidate"
            manifest = transfer(frozen, staged, "stage")
            self.assertTrue(manifest["summary"]["reset_marker"])
            self.assertEqual(manifest["summary"]["public_key"], public(202).hex())
            self.assertEqual((staged/"identity-state.json").read_bytes(), authority.read_bytes())
            rollback = root / "reset-rollback"
            transfer(staged, rollback, "rollback")
            load_rollback_in_go(rollback, public(202))
            source = Source()
            try:
                config = root / "reset-base.json"
                config.write_text(json.dumps({"state_dir": str(staged),
                    "radio_address": f"127.0.0.1:{source.port}", "companion_listen": "127.0.0.1:0",
                    "required_profile": PROFILE.hex(), "run_ms": 300}))
                config.chmod(0o600)
                proc = subprocess.run([str(BIN), str(config)], capture_output=True, text=True, timeout=15)
                self.assertEqual(proc.returncode, 0, proc.stdout+proc.stderr)
                self.assertIn("public_key="+public(202).hex(), proc.stdout)
                fresh = json.loads((staged/"identity-state.json").read_bytes())["state"]
                self.assertEqual(bytes(fresh["PublicKey"]), public(202))
                self.assertEqual(fresh["Name"], "Shared Radio Base")
                self.assertEqual((len(fresh["Contacts"]), len(fresh["Messages"]), len(fresh["Channels"])), (0, 0, 40))
                self.assertEqual((staged/"identity.seed").read_bytes(), bytes([1])*32)
                initialized = root / "initialized-rollback"
                transfer(staged, initialized, "rollback")
                load_rollback_in_go(initialized, public(202))
            finally:
                source.close()
            self.assertEqual(hashes(inventory(frozen)), reset_before)
        finally:
            shutil.rmtree(root)

    def test_identity_envelope_import_restart_and_rollback(self):
        root = ROOT / "build" / f"base-identity-{os.getpid()}"
        root.mkdir(mode=0o700)
        source = Source()
        proc = None
        clients = []
        try:
            frozen = root / "frozen"
            frozen.mkdir(mode=0o700)
            doc = saved_state()
            doc["Messages"], doc["Sequence"] = [], 0
            doc["Preferences"]["flood_advert_seconds"] = 0
            for name, data in (("identity.seed", bytes([1])*32), ("companion.json", json.dumps(doc).encode())):
                path = frozen / name
                path.write_bytes(data)
                path.chmod(0o600)
            candidate = root / "candidate"
            transfer(frozen, candidate, "stage")
            original = hashes(inventory(frozen))
            config = root / "base.json"
            config.write_text(json.dumps({"state_dir": str(candidate),
                "radio_address": f"127.0.0.1:{source.port}", "companion_listen": "127.0.0.1:0",
                "required_profile": PROFILE.hex(), "enable_key_export": True, "enable_key_import": True}))
            config.chmod(0o600)
            proc = subprocess.Popen([str(BIN), str(config)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            line = proc.stdout.readline().strip()
            self.assertTrue(line.startswith("BASE_LISTEN "), line)
            clients = [Client(int(line.split("port=")[1].split()[0])) for _ in range(2)]
            for c in clients:
                self.assertEqual(c.command(b"\x21", 19), b"\x13\0\0\x20\0\0")
                self.assertEqual(c.command(b"\x22signature"), b"\0")
            expanded = bytearray(hashlib.sha512(bytes([201])*32).digest())
            expanded[0] &= 248
            expanded[31] = (expanded[31] & 63) | 64
            self.assertEqual(clients[0].command(b"\x18"+expanded), b"\0")
            self.assertEqual(clients[1].command(b"\x23"), b"\1\4")
            self.assertEqual(clients[0].command(b"\x17", 14), b"\x0e"+expanded)
            self_info = clients[1].command(b"\1"+bytes(7), 5)
            self.assertEqual(self_info[4:36], public(201))
            for c in clients:
                c.close()
            clients = []
            proc.terminate()
            out, err = proc.communicate(timeout=15)
            self.assertEqual(proc.returncode, 0, out+err)
            envelope = json.loads((candidate / "identity-state.json").read_bytes())
            self.assertEqual(base64.b64decode(envelope["identity"]), expanded)
            self.assertEqual(bytes(envelope["state"]["PublicKey"]), public(201))
            self.assertEqual(envelope["state"]["Preferences"], doc["Preferences"])
            self.assertEqual((candidate / "identity.seed").read_bytes(), bytes([1])*32)
            rollback = root / "rollback"
            transfer(candidate, rollback, "rollback")
            load_rollback_in_go(rollback, public(201))
            self.assertEqual((rollback / "identity-state.json").read_bytes(), (candidate / "identity-state.json").read_bytes())
            self.assertEqual(hashes(inventory(frozen)), original)
            # Envelope authority wins over retained seed and expanded files.
            stale = candidate / "identity.expanded"
            stale.write_bytes(b"invalid retained key")
            stale.chmod(0o600)
            source.close()
            source = Source()
            cfg = json.loads(config.read_bytes())
            cfg["radio_address"] = f"127.0.0.1:{source.port}"
            config.write_text(json.dumps(cfg))
            proc = subprocess.Popen([str(BIN), str(config)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            line = proc.stdout.readline().strip()
            self.assertIn("public_key="+public(201).hex(), line)
            proc.terminate()
            out, err = proc.communicate(timeout=15)
            self.assertEqual(proc.returncode, 0, out+err)
            # Corrupt authority never falls back to the retained seed.
            authority = candidate / "identity-state.json"
            authority.write_bytes(b"{broken")
            with self.assertRaises(ValueError):
                transfer(candidate, root/"rejected", "rollback")
            self.assertFalse((root/"rejected").exists())
        finally:
            for c in clients:
                c.close()
            if proc is not None and proc.poll() is None:
                proc.terminate()
                proc.communicate(timeout=5)
            source.close()
            shutil.rmtree(root)

    def test_coherent_pipeline_and_frozen_roundtrip(self):
        root = ROOT / "build" / f"base-scenario-{os.getpid()}"
        root.mkdir(mode=0o700)
        source = Source()
        clients = []
        proc = None
        try:
            frozen = root / "frozen"
            frozen.mkdir(mode=0o700)
            for name, data in (("identity.seed", bytes([1])*32),
                               ("companion.json", json.dumps(saved_state()).encode())):
                path = frozen / name
                path.write_bytes(data)
                path.chmod(0o600)
            original = hashes(inventory(frozen))
            candidate = root / "candidate"
            manifest = transfer(frozen, candidate, "stage")
            self.assertEqual(manifest["summary"]["contacts"], 167)
            self.assertEqual(hashes(inventory(frozen)), original)
            config = root / "base.json"
            config.write_text(json.dumps({"state_dir": str(candidate),
                "radio_address": f"127.0.0.1:{source.port}", "companion_listen": "127.0.0.1:0",
                "required_profile": PROFILE.hex(), "enable_key_export": False, "enable_key_import": False}))
            config.chmod(0o600)
            proc = subprocess.Popen([str(BIN), str(config)], stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True, bufsize=1)
            line = proc.stdout.readline().strip()
            self.assertTrue(line.startswith("BASE_LISTEN "), line + proc.stderr.read() if proc.poll() is not None else line)
            port = int(line.split("port=")[1].split()[0])
            clients = [Client(port), Client(port)]
            for c in clients:
                device = c.command(b"\x16\3", 13)
                self.assertEqual((device[1], device[2], device[3], device[81]), (13, 175, 40, 2))
                self_info = c.command(bytes([1])+bytes(7), 5)
                self.assertEqual(self_info[4:36], public(1))
                self.assertEqual(self_info[58:], b"Birch-Base")
                first = c.command(b"\4", 2)
                self.assertEqual(int.from_bytes(first[1:], "little"), 167)
                frames = [c.read() for _ in range(168)]
                self.assertEqual(sum(frame[0] == 3 for frame in frames), 167)
                self.assertEqual(frames[-1][0], 4)
            # The upstream MeshCore client uses its own framing/response parsers.
            env = os.environ | {"MESHCORE_HEW_BASE_SDK_ADDRESS": f"127.0.0.1:{port}", "TMPDIR": str(ROOT / "build")}
            sdk = subprocess.run(["go", "test", "../../internal/companion", "-run", "^TestHewBaseSDKTCP$", "-count=1"],
                                 cwd=ROOT, env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(sdk.returncode, 0, sdk.stdout + sdk.stderr)
            histories = []
            for c in clients:
                histories.append([c.command(b"\x0a", 16) for _ in range(256)])
                self.assertEqual(c.command(b"\x0a"), b"\x0a")
            self.assertEqual(histories[0], histories[1])
            self.assertEqual(clients[0].command(b"\x17"), b"\x0f")
            # Single scheduled private send, native ACK broadcast to both clients.
            stamp = int(time.time())
            command = b"\2\0\0" + struct.pack("<I", stamp) + public(2)[:6] + b"hello"
            sent = clients[0].command(command, 6)
            job = source.next(2)
            self.assertEqual((job["priority"], job["delay"]), (0, 0))
            plain = unseal(secret(2, public(1)), parse(job["raw"])[4][2:])
            self.assertEqual(plain[:10], struct.pack("<IB", stamp, 0) + b"hello")
            ack = hashlib.sha256(plain[:10] + public(1)).digest()[:4]
            self.assertEqual(sent[2:6], ack)
            source.receive(packet(3, ack))
            for c in clients:
                self.assertEqual(c.push(130)[1:5], ack)
            # Actual encrypted ingress commits before its native ACK enters the source.
            incoming = struct.pack("<IB", stamp+1, 0) + b"reply"
            source.receive(addressed(2, 2, incoming))
            receipt = source.next(3)
            self.assertEqual((receipt["priority"], receipt["delay"]), (0, 200))
            self.assertEqual(parse(receipt["raw"])[4][:4], hashlib.sha256(incoming + public(2)).digest()[:4])
            replies = [c.command(b"\x0a", 16) for c in clients]
            self.assertEqual(replies[0], replies[1])
            self.assertEqual(replies[0][1], 12)
            self.assertEqual(replies[0][16:], b"reply")
            # CLI and room login/history share the same identity and durable cursor.
            cli = b"\2\1\0" + struct.pack("<I", stamp) + public(2)[:6] + b"ver"
            self.assertEqual(clients[0].command(cli, 6)[2:6], bytes(4))
            cli_tx = source.next(2)
            cli_plain = unseal(secret(2, public(1)), parse(cli_tx["raw"])[4][2:])
            cli_reply = addressed(2, 2, cli_plain[:4]+b"\4Hew-compatible CLI")
            source.receive(cli_reply)
            for c in clients:
                c.wait_rx(cli_reply)
                self.assertTrue(c.command(b"\x0a", 16).endswith(b"Hew-compatible CLI"))
            login = b"\x1a" + public(2) + b"1234567890123456789"
            clients[0].command(login, 6)
            login_job = source.next(7)
            login_plain = unseal(secret(2, public(1)), parse(login_job["raw"])[4][33:])
            self.assertEqual(login_plain[4:8], struct.pack("<I", 321))
            self.assertEqual(login_plain[8:23], b"123456789012345")
            response = struct.pack("<I", stamp+5) + bytes([0, 1, 2, 0, 0, 0, 0, 0, 1])
            source.receive(addressed(2, 1, response))
            for c in clients:
                self.assertEqual(c.push(133)[2:8], public(2)[:6])
            self.assertEqual(clients[1].command(b"\x1c"+public(2)), b"\0")
            room_plain = struct.pack("<IB", stamp+6, 8) + public(3)[:4] + b"room history"
            source.receive(addressed(2, 2, room_plain))
            room_ack = source.next(3)
            self.assertEqual(parse(room_ack["raw"])[4][:4], hashlib.sha256(room_plain+public(1)).digest()[:4])
            for c in clients:
                self.assertTrue(c.command(b"\x0a", 16).endswith(b"room history"))
            self.assertEqual(clients[0].command(b"\x1d"+public(2)), b"\0")
            self.assertEqual(clients[1].command(b"\x1c"+public(2)), b"\1\2")
            # Channel 39, signed advert import and non-owner tuning contracts.
            channel = b"\x20\x27" + b"Channel-39".ljust(32, b"\0") + bytes([9])*16
            self.assertEqual(clients[0].command(channel), b"\0")
            self.assertEqual(clients[1].command(b"\x1f\x27", 18), b"\x12\x27"+channel[2:])
            self.assertEqual(clients[0].command(b"\x12"+advert(200, "New contact", stamp+7)), b"\0")
            self.assertEqual(clients[1].push(138)[1:33], public(200))
            self.assertEqual(clients[0].command(b"\x0c\3"), b"\1\1")
            self.assertEqual(clients[0].command(b"\x14"), b"\1\4")
            self.assertEqual(clients[0].command(b"\x15"+struct.pack("<II", 0, 99000)), b"\0")
            # Stop before reverse handover; the old frozen role remains unchanged.
            for c in clients:
                c.close()
            clients = []
            proc.terminate()
            out, err = proc.communicate(timeout=15)
            self.assertEqual(proc.returncode, 0, out+err)
            self.assertIn("state_saved=true", out)
            self.assertEqual(hashes(inventory(frozen)), original)
            rollback = root / "rollback"
            transfer(candidate, rollback, "rollback")
            saved = json.loads((rollback / "companion.json").read_bytes())
            load_rollback_in_go(rollback, public(1))
            self.assertEqual(saved["Name"], "Birch-Base")
            self.assertEqual(len(saved["Contacts"]), 168)
            self.assertEqual(len(saved["Messages"]), 256)
            self.assertEqual(saved["Sequence"], 297)
            self.assertEqual(saved["Contacts"][0]["SyncSince"], stamp+6)
            self.assertEqual(saved["Preferences"]["airtime_factor"], 99)
            self.assertEqual(saved["Preferences"]["future_preference"], {"enabled": True, "value": 99})
            self.assertEqual((rollback / "identity.seed").read_bytes(), bytes([1])*32)
        finally:
            for c in clients:
                c.close()
            if proc is not None and proc.poll() is None:
                proc.terminate()
                try:
                    proc.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.communicate()
            source.close()
            shutil.rmtree(root)


if __name__ == "__main__":
    unittest.main()
