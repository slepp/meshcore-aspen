"""Production modem -> Hew observer -> localhost MQTT, with broker/modem reconnect."""
import json
import base64
import hashlib
import os
import struct
import time
import unittest
from nacl.signing import VerifyKey

import service_demo
from observer_client import Broker, field, packet as mqtt_packet
from parity import ROOT, public, packet, parse
from service_demo import RunningService, advert, login


class CaptureBroker(Broker):
    def connect(self, connection):
        first = mqtt_packet(connection)
        assert first and first[0] == 16
        body = first[1]
        protocol, at = field(body, 0)
        assert protocol == b"MQTT" and body[at:at + 4] == b"\x04\xee\0\x0a"
        client, at = field(body, at + 4)
        topic, at = field(body, at)
        will, at = field(body, at)
        username, at = field(body, at)
        password, at = field(body, at)
        assert at == len(body) and client == b"meshcore-observer-" + public(3).hex()[:12].encode()
        assert username == b"v1_" + public(3).hex().upper().encode()
        header, claims, signature = password.split(b".")
        unpad = lambda value: base64.urlsafe_b64decode(value+b"="*((-len(value)) % 4))
        VerifyKey(public(3)).verify(header+b"."+claims, bytes.fromhex(signature.decode()))
        identity = json.loads(unpad(claims))
        assert identity["publicKey"] == public(3).hex().upper() and identity["aud"] == "willow-local"
        assert identity["iat"] <= time.time()+5 < identity["exp"]
        assert topic == b"willow-local/YYC/" + public(3).hex().upper().encode() + b"/status"
        assert json.loads(will)["status"] == "offline"
        connection.settimeout(15)
        return topic


class ObserverService(RunningService):
    def start_process(self, native, runner=False):
        seed = self.root / "observer.seed"
        if not seed.exists():
            seed.write_bytes(bytes([6]) * 32)
            seed.chmod(0o600)
            expanded = bytearray(hashlib.sha512(bytes([3])*32).digest())
            expanded[0] &= 248
            expanded[31] = (expanded[31]&63)|64
            expanded[32:] = bytes(value ^ 0x5a for value in expanded[32:])
            path = self.root/"observer.expanded"
            path.write_bytes(expanded)
            path.chmod(0o600)
        super().start_process(native, runner)
        self.wait("OBSERVER_READY")
        self.wait("OBSERVER_MQTT state=ready connected=true")


class IntegratedObserver(unittest.TestCase):
    def test_receive_publish_reconnect(self):
        service_demo.PROFILE = struct.pack("<IIBBBfBh", 912525000, 250000, 7, 5, 2, 1.0, 1, 0)
        broker = CaptureBroker("retry")
        service = None
        try:
            service = ObserverService(extra_config=(
                "observer.enabled=1\n"
                f"observer.url=tcp://127.0.0.1:{broker.port}\n"
                "observer.format=capture-v1\nobserver.iata=YYC\n"
                "observer.topic_prefix=willow-local\nobserver.origin=Willow local integration\n"
                "observer.audience=willow-local\n"
                "observer.packet_filter=16\nobserver.queue_size=16\n"))
            modem = service.emulator
            raw = advert(2)
            # Logical-port copies must not become four observer events.
            for port in range(4):
                modem.send(port, 0, raw)
            self.until(lambda: len(broker.messages) >= 2, service, broker)
            self.assertEqual(broker.messages[0][2], broker.messages[1][2])
            payload = json.loads(broker.messages[1][2])
            self.assertEqual(payload["raw"], raw.hex().upper())
            self.assertEqual((payload["SNR"], payload["RSSI"]), ("4.5", "-73"))
            self.assertEqual(payload["origin_id"], public(3).hex().upper())
            self.assertEqual((payload["packet_type"], payload["direction"]), ("4", "rx"))
            self.assertGreaterEqual(broker.connections, 2)
            # RF-only capture excludes reflection, absent metadata and masked types.
            modem.send(3, 0, raw, metadata=b"\xf9\x80\x7f")
            modem.send(3, 0, raw, metadata=None)
            modem.send(3, 0, packet(3, b"\1\2\3\4"))
            # A real room request must still traverse the same receiving tree.
            modem.send(1, 0, login(stamp=int(time.time())))
            modem.receive(lambda job: job["port"] == 1 and parse(job["raw"])[0] in (1, 8))
            time.sleep(.2)
            self.assertEqual(len(broker.messages), 2)
            before = len(service.logs)
            modem.disconnect()
            service.wait("ONLINE epoch=2", 15, after=before)
            modem.send(3, 0, raw)
            self.until(lambda: len(broker.messages) >= 3, service, broker)
            self.assertEqual(json.loads(broker.messages[2][2])["raw"], raw.hex().upper())
            self.assertFalse(any(job["port"] == 3 for job in modem.all_submissions))
            # PINGREQ is emitted only after the pending packet is acknowledged.
            self.until(lambda: broker.pings > 0, service, broker)
            service.stop_process()
            self.assertEqual(json.loads(broker.statuses[-1][2])["status"], "offline")
            self.assertEqual(broker.disconnects, 1)
            stats = next(line for line in reversed(service.logs) if line.startswith("OBSERVER_STATS"))
            self.assertIn("observed=3 published=2 dropped=1 queued=0 pending=false", stats)
            evidence = {"profile": service_demo.PROFILE.hex(), "mqtt_connections": broker.connections,
                        "expanded_identity_jwt_verified": True, "stale_seed_ignored": True,
                        "packet_deliveries": len(broker.messages), "retry_payload_unchanged": True,
                        "modem_epochs": modem.epoch, "room_login_reply": True,
                        "statistics": stats, "markers": [line for line in service.logs
                            if line.startswith(("OBSERVER_", "ONLINE", "OFFLINE"))]}
            (ROOT / "build/observer-service-results.json").write_text(json.dumps(evidence, indent=2) + "\n")
        finally:
            if service is not None:
                service.close()
            broker.close()
            if broker.errors:
                raise broker.errors[0]
        self.assertEqual(broker.errors, [])

    def until(self, predicate, service, broker):
        end = time.monotonic() + 15
        while time.monotonic() < end:
            if broker.errors:
                raise broker.errors[0]
            if predicate():
                return
            if service.proc.poll() is not None:
                self.fail(f"service exited: {service.logs} {service.errors}")
            time.sleep(.02)
        self.fail(f"observer pipeline timed out: {service.logs} {service.errors}")


if __name__ == "__main__":
    os.environ.setdefault("MESHCORE_HEW_HOST", str(ROOT / "build/hew-host-release"))
    unittest.main()
