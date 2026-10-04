"""Native observer actors against a private, output-only loopback MQTT fixture."""
import base64
import json
import hashlib
import os
from pathlib import Path
import socket
import subprocess
import threading
import time
import unittest

from nacl.signing import VerifyKey
from outbound import TLSFixture

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("MESHCORE_HEW_OBSERVER_CLIENT_BIN", ROOT / "build/observer-client-checks"))


def take(connection, size):
    data = b""
    while len(data) < size:
        part = connection.recv(size - len(data))
        if not part:
            if not data:
                return None
            raise AssertionError("incomplete MQTT fixture frame")
        data += part
    return data


def packet(connection):
    header = take(connection, 1)
    if header is None:
        return None
    size = 0
    for index in range(4):
        digit = take(connection, 1)
        if digit is None:
            raise AssertionError("missing MQTT remaining length")
        size |= (digit[0] & 127) << (index * 7)
        if not digit[0] & 128:
            break
    else:
        raise AssertionError("invalid MQTT remaining length")
    if size > 1048576:
        raise AssertionError("oversized MQTT fixture packet")
    return header[0], take(connection, size) if size else b""


def field(data, at):
    size = int.from_bytes(data[at:at + 2], "big")
    end = at + 2 + size
    if end > len(data):
        raise AssertionError("invalid MQTT string length")
    return data[at + 2:end], end


def publication(frame):
    header, body = frame
    if header >> 4 != 3 or (header >> 1) & 3 != 1:
        raise AssertionError("expected QoS1 publication")
    topic, at = field(body, 0)
    identifier = body[at:at + 2]
    if len(identifier) != 2 or identifier == b"\0\0":
        raise AssertionError("invalid MQTT packet identifier")
    return topic, identifier, body[at + 2:], bool(header & 1)


class WebSocketStream:
    def __init__(self, connection, mode):
        self.connection, self.mode = connection, mode
        self.buffer = b""
        self.pongs = 0
        request = b""
        while not request.endswith(b"\r\n\r\n"):
            part = take(connection, 1)
            if part is None or len(request) >= 16384:
                raise AssertionError("missing or oversized WebSocket upgrade")
            request += part
        lines = request.decode("ascii").split("\r\n")
        headers = dict(line.lower().split(": ", 1) for line in lines[1:] if line)
        original = dict(line.split(": ", 1) for line in lines[1:] if line)
        if lines[0] != "GET /mqtt HTTP/1.1" or headers.get("sec-websocket-protocol") != "mqtt":
            raise AssertionError("MQTT WebSocket path/subprotocol changed")
        if headers.get("upgrade") != "websocket" or headers.get("sec-websocket-version") != "13":
            raise AssertionError("invalid WebSocket upgrade")
        key = original["Sec-WebSocket-Key"]
        if len(base64.b64decode(key, validate=True)) != 16:
            raise AssertionError("invalid WebSocket key")
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        protocol = b"other" if mode == "bad-upgrade" else b"mqtt"
        connection.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           b"Sec-WebSocket-Accept: " + accept + b"\r\nSec-WebSocket-Protocol: " +
                           protocol + b"\r\n\r\n")

    def frame(self, header, payload):
        size = len(payload)
        length = bytes([size]) if size < 126 else b"\x7e" + size.to_bytes(2, "big")
        self.connection.sendall(bytes([header]) + length + payload)

    def sendall(self, data):
        if self.mode == "ws-fragments" and len(data) > 1:
            self.frame(2, data[:1])
            self.frame(137, b"fixture-ping")
            self.frame(128, data[1:])
        else:
            self.frame(130, data)

    def recv(self, size):
        while not self.buffer:
            head = take(self.connection, 2)
            if head is None:
                return b""
            if head[0] & 112 or not head[1] & 128:
                raise AssertionError("client WebSocket frames must be masked without extensions")
            length = head[1] & 127
            if length in (126, 127):
                length = int.from_bytes(take(self.connection, 2 if length == 126 else 8), "big")
            if length > 1048576:
                raise AssertionError("oversized WebSocket fixture frame")
            mask = take(self.connection, 4)
            body = take(self.connection, length) if length else b""
            body = bytes(value ^ mask[index % 4] for index, value in enumerate(body))
            kind = head[0] & 15
            if kind == 10:
                if body != b"fixture-ping":
                    raise AssertionError("incorrect automatic WebSocket PONG")
                self.pongs += 1
                continue
            if kind == 8:
                return b""
            if kind not in (0, 2):
                raise AssertionError("MQTT must use binary WebSocket frames")
            self.buffer = body
        result, self.buffer = self.buffer[:size], self.buffer[size:]
        return result

    def settimeout(self, seconds):
        self.connection.settimeout(seconds)

    def close(self):
        self.connection.close()


class Broker:
    def __init__(self, mode, tls=None, websocket=False):
        self.mode, self.tls, self.websocket = mode, tls, websocket
        self.websocket_pongs = 0
        self.errors, self.messages, self.statuses = [], [], []
        self.tokens = []
        self.connections = self.disconnects = self.pings = 0
        self.done = threading.Event()
        self.listener = socket.socket()
        if mode == "ws-backpressure":
            self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.thread = threading.Thread(target=self.serve)
        self.thread.start()

    def connect(self, connection):
        first = packet(connection)
        if not first or first[0] != 16:
            raise AssertionError("first bytes must be native MQTT CONNECT, not HTTP")
        body = first[1]
        protocol, at = field(body, 0)
        version, flags = body[at], body[at + 1]
        keepalive = int.from_bytes(body[at + 2:at + 4], "big")
        if protocol != b"MQTT" or version != 4 or flags & 63 != 46 or keepalive != 10:
            raise AssertionError("CONNECT session/will contract changed")
        client, at = field(body, at + 4)
        topic, at = field(body, at)
        will, at = field(body, at)
        username, at = field(body, at)
        password, at = field(body, at)
        if client != b"willow-fixture" or flags & 192 != 192 or at != len(body):
            raise AssertionError("CONNECT identity/credential layout changed")
        if self.mode.startswith("jwt"):
            head, claims, signature = password.decode().split(".")
            decode = lambda value: base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
            if json.loads(decode(head)) != {"alg": "Ed25519", "typ": "JWT"}:
                raise AssertionError("wrong JWT header")
            payload = json.loads(decode(claims))
            self.tokens.append(payload)
            if username.decode() != "v1_" + payload["publicKey"] or payload["aud"] != "willow-fixture":
                raise AssertionError("wrong JWT identity/audience")
            VerifyKey(bytes.fromhex(payload["publicKey"])).verify(
                (head + "." + claims).encode(), bytes.fromhex(signature))
            if payload["exp"] - payload["iat"] != 86400:
                raise AssertionError("wrong JWT validity")
            if json.loads(will)["status"] != "offline":
                raise AssertionError("wrong retained will")
        elif username != b"fixture-user" or password != b"fixture-password" or will != b"offline":
            raise AssertionError("static credential/will contract changed")
        return topic

    def serve_connection(self, connection):
        if self.mode == "ws-backpressure":
            message = take(connection, 12)
            if message != b"HEW_OUTBOUND":
                raise AssertionError("WebSocket transport payload changed")
            connection.sendall(message)
            self.done.wait(6)
            return
        topic = self.connect(connection)
        for part in (b"\x20", b"\x02\0", b"\0"):
            connection.sendall(part)
            time.sleep(0.002)
        online = publication(packet(connection))
        if online[0] != topic or not online[3]:
            raise AssertionError("online status must be retained on the status topic")
        self.statuses.append(online)
        if self.mode in ("gate", "burst"):
            connection.settimeout(0.15)
            try:
                premature = packet(connection)
            except socket.timeout:
                premature = None
            if premature is not None:
                raise AssertionError("publication sent before online status PUBACK")
            connection.settimeout(6)
        connection.sendall(b"\x40\x02" + online[1])
        while not self.done.is_set():
            frame = packet(connection)
            if frame is None:
                return
            if frame[0] == 224 and frame[1] == b"":
                self.disconnects += 1
                return
            if frame[0] == 192 and frame[1] == b"":
                self.pings += 1
                if self.mode == "ping-timeout" and self.connections == 1:
                    if packet(connection) is not None:
                        raise AssertionError("PINGRESP deadline did not retire the connection")
                    return
                connection.sendall(b"\xd0\0")
                continue
            record = publication(frame)
            if record[0] == topic:
                if not record[3]:
                    raise AssertionError("offline status is not retained")
                self.statuses.append(record)
                if self.mode == "wrong-ack":
                    connection.sendall(b"\x40\x02\x03\xe7")
                    time.sleep(0.06)
                connection.sendall(b"\x40\x02" + record[1] + (b"\0" if self.mode == "ack-bad" else b""))
                if self.mode in ("ack-bad", "ack-eof"):
                    return
                if self.mode == "wrong-ack":
                    connection.sendall(b"\x40\x02" + record[1])
                continue
            if record[0] != topic[:-6] + b"packets" or record[3]:
                raise AssertionError("packet topic or retain policy changed")
            self.messages.append(record)
            if self.connections == 1 and self.mode in ("ws-text", "ws-oversize"):
                if self.mode == "ws-text":
                    connection.frame(129, b"not MQTT binary data")
                else:
                    connection.connection.sendall(b"\x82\x7f" + (1048577).to_bytes(8, "big") + b"\0")
                if take(connection.connection, 1) is not None:
                    raise AssertionError("invalid WebSocket data did not retire transport")
                return
            if self.connections == 1 and self.mode in ("retry", "timeout", "panic"):
                if self.mode == "retry":
                    return
                if packet(connection) is not None:
                    raise AssertionError("unacknowledged packet replayed on the same connection")
                return
            connection.sendall(b"\x40\x02" + record[1])

    def serve(self):
        try:
            while not self.done.is_set():
                try:
                    connection, _ = self.listener.accept()
                except socket.timeout:
                    continue
                connection.settimeout(6)
                self.connections += 1
                try:
                    if self.tls:
                        connection = self.tls.wrap_socket(connection, server_side=True)
                    if self.websocket:
                        connection = WebSocketStream(connection, self.mode)
                    if self.mode == "bad-upgrade":
                        if take(connection.connection, 1) is not None:
                            raise AssertionError("MQTT sent despite wrong WebSocket subprotocol")
                    else:
                        self.serve_connection(connection)
                    if self.websocket:
                        self.websocket_pongs += connection.pongs
                finally:
                    connection.close()
        except BaseException as error:
            if not self.done.is_set():
                self.errors.append(error)

    def close(self):
        self.done.set()
        self.thread.join(timeout=7)
        self.listener.close()
        if self.thread.is_alive():
            raise AssertionError("loopback broker did not stop")


class ObserverClient(TLSFixture):
    def exercise(self, mode, tls=False, websocket=False):
        broker = Broker(mode, self.tls if tls else None, websocket)
        try:
            scheme = ("wss" if tls else "ws") if websocket else ("tls" if tls else "tcp")
            url = f"{scheme}://localhost:{broker.port}" + ("/mqtt" if websocket else "")
            result = subprocess.run([str(BINARY), mode, url, str(self.cert) if tls else ""],
                                    capture_output=True, text=True, timeout=18)
        finally:
            broker.close()
        if broker.errors:
            raise broker.errors[0]
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        if mode == "bad-upgrade":
            self.assertIn("OBSERVER_BAD_UPGRADE_OK", result.stdout)
            self.assertEqual(broker.connections, 1)
            self.assertEqual(broker.messages, [])
            return broker, result.stdout
        self.assertIn("OBSERVER_CLIENT_OK", result.stdout)
        self.assertIn("OBSERVER_STATUS_ACK_GATE", result.stdout)
        abrupt = mode in ("ack-bad", "ack-eof", "closed-panic")
        self.assertEqual(broker.disconnects, 0 if abrupt else 1)
        if not abrupt:
            self.assertEqual(json.loads(broker.statuses[-1][2])["status"] if mode.startswith("jwt") else broker.statuses[-1][2],
                             "offline" if mode.startswith("jwt") else b"offline")
        if mode in ("retry", "timeout", "panic", "ws-text", "ws-oversize"):
            self.assertEqual(broker.connections, 2)
            self.assertEqual(len(broker.messages), 2)
            self.assertEqual(broker.messages[0][2], broker.messages[1][2])
        else:
            self.assertEqual(len(broker.messages), 2 if mode == "burst" else 1)
        if mode == "panic":
            self.assertIn("OBSERVER_PUBLISHER_PANIC_INJECTED", result.stdout)
        if mode == "timeout":
            self.assertIn("acknowledgement deadline expired: packet-ack", result.stdout)
        if mode in ("ping", "ping-timeout"):
            self.assertEqual(broker.pings, 1)
        if mode == "ping-timeout":
            self.assertEqual(broker.connections, 2)
            self.assertIn("PINGRESP deadline expired", result.stdout)
        if mode == "closed-panic":
            self.assertEqual(broker.connections, 1)
        if mode == "jwt-clock":
            self.assertIn("requires synchronized UTC", result.stdout)
            self.assertEqual(broker.connections, 1)
        if mode in ("jwt-renew", "jwt-rollback"):
            self.assertEqual(broker.connections, 2)
            self.assertEqual(len(broker.tokens), 2)
            difference = broker.tokens[1]["iat"] - broker.tokens[0]["iat"]
            if mode == "jwt-renew":
                self.assertGreaterEqual(difference, 86100)
            else:
                self.assertLess(difference, 0)
            self.assertIn("identity token requires reconnect", result.stdout)
        if mode == "ws-fragments":
            self.assertGreater(broker.websocket_pongs, 0)
        return broker, result.stdout

    def test_plaintext_qos1_and_graceful_offline(self): self.exercise("basic")
    def test_verified_tls_qos1(self): self.exercise("basic", tls=True)
    def test_online_status_ack_gates_publication(self): self.exercise("gate")
    def test_bounded_queue_during_status_ack_delay(self): self.exercise("burst")
    def test_unacknowledged_event_retries_unchanged(self): self.exercise("retry")
    def test_missing_puback_deadline_retries_unchanged(self): self.exercise("timeout")
    def test_supervised_publisher_replacement_retains_pending_event(self): self.exercise("panic")
    def test_supervised_tls_publisher_replacement_retains_pending_event(self): self.exercise("panic", tls=True)
    def test_identity_authentication_over_verified_tls(self): self.exercise("jwt", tls=True)
    def test_keepalive_ping_and_pong(self): self.exercise("ping")
    def test_missing_ping_response_reconnects(self): self.exercise("ping-timeout")
    def test_established_tls_survives_setup_timeout(self): self.exercise("longalive", tls=True)
    def test_puback_before_malformed_suffix_remains_confirmed(self): self.exercise("ack-bad")
    def test_puback_before_eof_remains_confirmed(self): self.exercise("ack-eof")
    def test_wrong_and_duplicate_pubacks_do_not_settle_other_events(self): self.exercise("wrong-ack")
    def test_closed_outbox_prevents_publisher_restart_reconnection(self): self.exercise("closed-panic")
    def test_identity_auth_waits_for_synchronized_clock(self): self.exercise("jwt-clock", tls=True)
    def test_identity_auth_renews_before_expiration(self): self.exercise("jwt-renew", tls=True)
    def test_identity_auth_reconnects_after_clock_reversal(self): self.exercise("jwt-rollback", tls=True)
    def test_websocket_binary_mqtt(self): self.exercise("basic", websocket=True)
    def test_verified_secure_websocket_mqtt(self): self.exercise("jwt", tls=True, websocket=True)
    def test_websocket_fragmented_binary_with_control_ping(self): self.exercise("ws-fragments", websocket=True)
    def test_websocket_puback_retry_is_unchanged(self): self.exercise("retry", tls=True, websocket=True)
    def test_websocket_publisher_panic_retains_pending_event(self): self.exercise("panic", tls=True, websocket=True)
    def test_wrong_websocket_subprotocol_rejected_before_mqtt(self): self.exercise("bad-upgrade", websocket=True)
    def test_websocket_text_is_rejected_without_settling_pending_packet(self): self.exercise("ws-text", websocket=True)
    def test_websocket_oversize_is_rejected_without_buffering_entire_frame(self): self.exercise("ws-oversize", websocket=True)

    def test_secure_websocket_backpressure_returns_partial_count(self):
        broker = Broker("ws-backpressure", self.tls, websocket=True)
        binary = ROOT / "build" / ("outbound-checks-release" if BINARY.name.endswith("-release") else "outbound-checks")
        try:
            result = subprocess.run([str(binary), "backpressure", f"wss://localhost:{broker.port}/mqtt", str(self.cert)],
                                    capture_output=True, text=True, timeout=12)
        finally:
            broker.close()
        if broker.errors:
            raise broker.errors[0]
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("deadline expired; committed_prefix=", result.stdout)
        self.assertIn("OUTBOUND_CHECKS_OK", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
