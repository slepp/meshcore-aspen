"""Existing RF administration client against supervised roles over loopback."""
import heapq
from pathlib import Path
import queue
import select
import socket
import sys
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parents[1]))
from tools.hardware.admin import NativeClient, decode
from service_demo import RunningService, public, parse, kiss, packet
from firmware_negotiation import Decoder


class Gateway:
    """Model consumed direct hops and added flood hops without changing ciphertext."""
    def __init__(self, modem, role_port, hops=0):
        self.modem = modem
        self.role_port = role_port
        self.path = b"".join(index.to_bytes(3, "little") for index in range(1, hops + 1))
        self.error = None
        self.stop = False
        self.requests = []
        self.responses = []
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(.1)
        self.port = self.listener.getsockname()[1]
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            while not self.stop:
                try:
                    connection, _ = self.listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    return
                with connection:
                    decoder = Decoder()
                    pending = []
                    sequence = 0
                    while not self.stop:
                        if select.select([connection], [], [], .01)[0]:
                            data = connection.recv(4096)
                            if not data:
                                break
                            for frame in decoder.feed(data):
                                assert frame[0] == 0
                                self.requests.append(frame[1:])
                                raw = frame[1:]
                                kind, route, encoded, _, payload = parse(raw)
                                if kind == 7:
                                    raw = packet(kind, payload, route=route, width=3, path=self.path)
                                elif route == 2:
                                    raw = packet(kind, payload, route=route, width=(encoded >> 6) + 1)
                                self.modem.send(self.role_port, 0, raw)
                        while True:
                            try:
                                job = self.modem.submissions.get_nowait()
                            except queue.Empty:
                                break
                            if job["port"] == self.role_port:
                                raw = job["raw"]
                                kind, route, encoded, _, payload = parse(raw)
                                if kind == 8 and route == 1 and self.path:
                                    raw = packet(kind, payload, route=route, width=3,
                                                 path=b"\xaa\xbb\xcc")
                                elif route == 2:
                                    raw = packet(kind, payload, route=route, width=(encoded >> 6) + 1)
                                sequence += 1
                                heapq.heappush(pending, (time.monotonic() + job["delay"] / 1000,
                                                       sequence, raw))
                        while pending and pending[0][0] <= time.monotonic():
                            _, _, raw = heapq.heappop(pending)
                            self.responses.append(raw)
                            connection.sendall(kiss(raw))
        except Exception as error:
            if not self.stop:
                self.error = error

    def close(self):
        self.stop = True
        self.listener.close()
        self.thread.join(3)
        assert not self.thread.is_alive(), "loopback gateway did not stop"
        assert self.error is None, self.error


class NativeLogin(unittest.TestCase):
    def client(self, service, gateway, room, password="admin", timeout=5):
        seed = service.root / "native-client.seed"
        if not seed.exists():
            seed.write_bytes(bytes([2]) * 32)
            seed.chmod(0o600)
        return NativeClient("127.0.0.1", gateway.port, seed, public(4 if room else 1).hex(),
                            password, timeout=timeout, room=room, path=None,
                            tagged=False, retry_commands=False)

    def exercise(self, room, hops):
        with RunningService() as service:
            gateway = Gateway(service.emulator, 1 if room else 0, hops)
            try:
                client = self.client(service, gateway, room)
                try:
                    replies = []
                    for raw in gateway.responses:
                        decoded = decode(client.secret, client.public, client.target, raw)
                        if decoded is not None:
                            kind, plain = decoded
                            self.assertEqual(kind, 8)
                            size = (plain[0] & 63) * ((plain[0] >> 6) + 1)
                            self.assertEqual(plain[size + 1], 1)
                            replies.append(plain[size + 2:])
                    self.assertEqual(len(replies), 1)
                    self.assertEqual(replies[0][6:8], b"\x01\x03")
                    self.assertEqual(replies[0][12], 1 if room else 2)
                    self.assertEqual(gateway.requests[0][:2], b"\x1d\x80")
                    self.assertEqual(client.path, bytes([0x80 | hops]) + gateway.path)
                    port = 1 if room else 0
                    self.assertTrue(any(line.startswith(f"ROLE_RX port={port} kind=7 ")
                                        for line in service.logs), service.logs)
                    self.assertTrue(any(line.startswith(f"SUBMITTED port={port} ") and
                                        " kind=8 " in line for line in service.logs), service.logs)
                    self.assertEqual(client.command("get public.key"),
                                     "> " + public(4 if room else 1).hex().upper())
                    self.assertEqual(client.command("get role"),
                                     "> room_server" if room else "> repeater")
                    self.assertFalse(any("OFFLINE" in line for line in service.logs), service.logs)
                finally:
                    client.close()
            finally:
                gateway.close()

    def durability(self, room):
        with RunningService() as service:
            gateway = Gateway(service.emulator, 1 if room else 0)
            try:
                name = "Native client room" if room else "Native client relay"
                client = self.client(service, gateway, room)
                try:
                    self.assertEqual(client.command("set name " + name), "OK")
                    self.assertEqual(client.command("get name"), "> " + name)
                finally:
                    client.close()
                snapshot = service.root / ("room.state" if room else "relay.state")
                self.assertEqual(snapshot.read_bytes()[:4], b"HEW5")
                service.stop_process()
                service.start_process(True)
                client = self.client(service, gateway, room)
                try:
                    self.assertEqual(client.command("get name"), "> " + name)
                finally:
                    client.close()
            finally:
                gateway.close()

    def test_relay_zero_hop_login_and_cli(self): self.exercise(False, 0)
    def test_relay_one_hop_login_and_cli(self): self.exercise(False, 1)
    def test_relay_max_width3_path_login_and_cli(self): self.exercise(False, 21)
    def test_room_zero_hop_login_and_cli(self): self.exercise(True, 0)
    def test_room_one_hop_login_and_cli(self): self.exercise(True, 1)
    def test_room_max_width3_path_login_and_cli(self): self.exercise(True, 21)
    def test_relay_rf_preference_survives_process_restart(self): self.durability(False)
    def test_room_rf_preference_survives_process_restart(self): self.durability(True)
    def test_wrong_password_ingress_does_not_claim_authentication(self):
        with RunningService() as service:
            gateway = Gateway(service.emulator, 0)
            try:
                with self.assertRaises(TimeoutError):
                    self.client(service, gateway, False, password="wrong", timeout=1)
                self.assertTrue(any(line.startswith("ROLE_RX port=0 kind=7 ")
                                    for line in service.logs), service.logs)
                self.assertFalse(any(line.startswith("SUBMITTED port=0 ") and
                                     " kind=8 " in line for line in service.logs), service.logs)
            finally:
                gateway.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
