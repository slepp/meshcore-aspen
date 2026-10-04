"""One MQTT lifecycle against the Hew binary and the application's actual Go broker.

Only private ephemeral loopback listeners are used; never port 1883 or live config.
"""
import contextlib
import os
from pathlib import Path
import select
import shutil
import socket
import subprocess
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


def field(value):
    value = value.encode() if isinstance(value, str) else value
    return len(value).to_bytes(2, "big") + value


def frame(header, body=b""):
    remaining, length = len(body), b""
    while True:
        digit, remaining = remaining % 128, remaining // 128
        length += bytes([digit | (128 if remaining else 0)])
        if not remaining:
            return bytes([header]) + length + body


def take(conn, size):
    data = b""
    while len(data) < size:
        part = conn.recv(size - len(data))
        if not part:
            raise EOFError("MQTT connection ended")
        data += part
    return data


def packet(conn):
    header, remaining = take(conn, 1)[0], 0
    for index in range(4):
        digit = take(conn, 1)[0]
        remaining |= (digit & 127) << (index * 7)
        if not digit & 128:
            return header, take(conn, remaining)
    raise AssertionError("invalid remaining length")

def varint(value):
    assert 0 <= value <= 268435455
    out = b""
    while True:
        digit, value = value % 128, value // 128
        out += bytes([digit | (128 if value else 0)])
        if not value:
            return out

def variable(data, at):
    value = 0
    for index in range(4):
        digit = data[at]; at += 1
        value |= (digit & 127) << (7 * index)
        if digit < 128:
            return value, at
    raise AssertionError("invalid MQTT property integer")

def property_block(raw):
    size = len(raw)
    out = b""
    while True:
        digit, size = size % 128, size // 128
        out += bytes([digit | (128 if size else 0)])
        if not size:
            return out + raw

def decoded_properties(data, at=0):
    size, at = variable(data, at)
    end, result = at + size, {}
    assert end <= len(data)
    while at < end:
        kind, at = data[at], at + 1
        if kind in (1, 23, 25, 36, 37, 40, 41, 42):
            value, at = data[at], at + 1
        elif kind in (19, 33, 34, 35):
            value, at = int.from_bytes(data[at:at + 2], "big"), at + 2
        elif kind in (2, 17, 24, 39):
            value, at = int.from_bytes(data[at:at + 4], "big"), at + 4
        elif kind == 11:
            value, at = variable(data, at)
        else:
            length = int.from_bytes(data[at:at + 2], "big")
            value, at = data[at + 2:at + 2 + length], at + 2 + length
            if kind == 38:
                length = int.from_bytes(data[at:at + 2], "big")
                value, at = (value, data[at + 2:at + 2 + length]), at + 2 + length
        result.setdefault(kind, []).append(value)
    assert at == end
    return result, end


class Client:
    def __init__(self, port, name, clean=True, qos=0, will=None, keepalive=10,
                 username=None, password=None, version=4, properties=None, will_properties=b""):
        self.version, self.aliases = version, {}
        self.conn = socket.create_connection(("127.0.0.1", port), timeout=2)
        self.conn.settimeout(2)
        flags = 2 if clean else 0
        body = field("MQTT" if version != 3 else "MQIsdp") + bytes([version])
        if will:
            flags |= 4 | (qos << 3) | (32 if will[2] else 0)
        if username is not None:
            flags |= 128
        if password is not None:
            flags |= 64
        body += bytes([flags]) + keepalive.to_bytes(2, "big")
        if version == 5:
            body += property_block(properties if properties is not None else (
                b"\x11" + (30).to_bytes(4, "big") if not clean else b""))
        body += field(name)
        if will:
            if version == 5:
                body += property_block(will_properties)
            body += field(will[0]) + field(will[1])
        if username is not None:
            body += field(username)
        if password is not None:
            body += field(password)
        self.conn.sendall(frame(16, body))
        self.connack_wire = packet(self.conn)
        self.connack = (self.connack_wire[0], self.connack_wire[1][:2]) if version == 5 else self.connack_wire
        self.server_properties = decoded_properties(self.connack_wire[1], 2)[0] if version == 5 else {}

    def close(self, graceful=False):
        if graceful:
            self.conn.sendall(frame(224))
        self.conn.close()

    def expect(self, header, body, reason=0):
        actual = packet(self.conn)
        if self.version == 5 and header >> 4 in (4, 5, 6, 7):
            self.last_ack_properties = decoded_properties(actual[1], 3)[0] if len(actual[1]) > 3 else {}
            actual_reason = actual[1][2] if len(actual[1]) > 2 else 0
            assert actual_reason == reason, (actual_reason, reason, actual)
            actual = actual[0], actual[1][:2]
        if self.version == 5 and header == 224:
            assert (actual[1][0] if actual[1] else 0) == reason, actual
            actual = actual[0], b""
        if actual != (header, body):
            raise AssertionError(f"expected {(header, body)!r}, got {actual!r}")

    def idle(self, delay=0.08):
        self.conn.settimeout(delay)
        try:
            value = packet(self.conn)
        except socket.timeout:
            return
        finally:
            self.conn.settimeout(2)
        raise AssertionError(f"unexpected MQTT packet {value!r}")

    def subscribe(self, filters, identifier=1, properties=b""):
        body = identifier.to_bytes(2, "big")
        if self.version == 5:
            body += property_block(properties)
        for topic, qos in filters:
            body += field(topic) + bytes([qos])
        self.conn.sendall(frame(130, body))
        self.expect(144, identifier.to_bytes(2, "big") + (b"\0" if self.version == 5 else b"") +
                    bytes([q & 3 for _, q in filters]))

    def unsubscribe(self, filters, identifier=2):
        body = identifier.to_bytes(2, "big") + (b"\0" if self.version == 5 else b"") + b"".join(field(f) for f in filters)
        self.conn.sendall(frame(162, body))
        self.expect(176, identifier.to_bytes(2, "big") + (b"\0" + bytes(len(filters)) if self.version == 5 else b""))

    def publish(self, topic, payload, qos=0, retained=False, identifier=10, duplicate=False, properties=b""):
        body = field(topic) + (identifier.to_bytes(2, "big") if qos else b"")
        if self.version == 5:
            body += property_block(properties)
        body += payload
        self.conn.sendall(frame(48 | (qos << 1) | int(retained) | (8 if duplicate else 0), body))
        if qos:
            self.expect(64 if qos == 1 else 80, identifier.to_bytes(2, "big"),
                        reason=145 if self.version == 5 and duplicate and qos == 2 else 0)

    def publication(self, topic, payload, qos, retained=False, duplicate=False, acknowledge=True):
        header, actual_topic, actual_payload, identifier = self.read_publication()
        expected = 48 | (qos << 1) | int(retained) | (8 if duplicate else 0)
        assert (header, actual_topic, actual_payload) == (expected, topic.encode(), payload), (
            header, actual_topic, actual_payload)
        assert not qos or identifier != 0
        if acknowledge and qos:
            self.conn.sendall(frame(64 if qos == 1 else 80, identifier.to_bytes(2, "big")))
            if qos == 2:
                self.expect(98, identifier.to_bytes(2, "big"))
                self.conn.sendall(frame(112, identifier.to_bytes(2, "big")))
        return identifier

    def read_publication(self):
        header, body = packet(self.conn)
        assert header >> 4 == 3, (header, body)
        qos = (header >> 1) & 3
        size = int.from_bytes(body[:2], "big")
        actual_topic, at = body[2:2 + size], 2 + size
        identifier = int.from_bytes(body[at:at + 2], "big") if qos else 0
        at += 2 if qos else 0
        self.last_properties = {}
        if self.version == 5:
            self.last_properties, at = decoded_properties(body, at)
            if 35 in self.last_properties:
                alias = self.last_properties[35][0]
                if actual_topic:
                    self.aliases[alias] = actual_topic
                else:
                    actual_topic = self.aliases[alias]
        return header, actual_topic, body[at:], identifier


@contextlib.contextmanager
def endpoint(binary, credentials=None, host="127.0.0.1"):
    config = ROOT / "build" / f"broker-test-{os.getpid()}.config"
    if binary.name == "broker-oracle":
        command = [str(binary)] + (list(credentials) if credentials else [])
        if host != "127.0.0.1":
            assert credentials
            command.append(host)
    else:
        text = f"host={host}\nport=0\n"
        if credentials:
            text += f"username={credentials[0]}\npassword={credentials[1]}\n"
        fd = os.open(config, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as stream:
            stream.write(text)
        command = [str(binary), str(config)]
    proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert select.select([proc.stdout], [], [], 15)[0], "broker did not become ready"
        ready = proc.stdout.readline().strip()
        if "ORACLE_READY" in ready:
            port = int(ready.rsplit(":", 1)[1])
        else:
            assert ready.startswith("MQTT_BROKER_READY "), (ready, proc.poll())
            port = int(ready.split("port=", 1)[1].split()[0])
        assert port != 1883, "test must not use the live endpoint"
        yield port, proc
    finally:
        if proc.poll() is None:
            proc.terminate()
        try:
            output, error = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            output, error = proc.communicate()
            raise AssertionError("broker shutdown exceeded its deadline")
        if config.exists():
            config.unlink()
        assert proc.returncode == 0, (proc.returncode, output, error)
        assert not error, error
        # Listener owner closes before process exit: address can be rebound immediately.
        if "port" in locals():
            with socket.socket() as probe:
                probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                probe.bind(("127.0.0.1", port))


def lifecycle(port, version=4):
    """Subscriber/publisher/retained/QoS/reconnect/will lifecycle, one namespace."""
    clients = []

    def client(name, **options):
        value = Client(port, name, version=version, **options)
        clients.append(value)
        assert value.connack == (32, b"\0\0"), value.connack
        return value

    try:
        sub = client("subscriber", clean=False)
        sub.subscribe([("meshcore/test/#", 2), ("$fixture/#", 1)])
        pub = client("publisher", clean=False)
        pub.publish("meshcore/test/status", b"online", qos=1, retained=True)
        sub.publication("meshcore/test/status", b"online", 1)
        pub.publish("$fixture/status", b"private", qos=1)
        sub.publication("$fixture/status", b"private", 1)
        wildcard = client("wildcard")
        wildcard.subscribe([("#", 0)])
        wildcard.publication("meshcore/test/status", b"online", 0, retained=True)
        pub.publish("$fixture/status", b"private", qos=1)
        sub.publication("$fixture/status", b"private", 1)
        wildcard.idle()
        wildcard.close(graceful=True)
        sub.subscribe([("meshcore/+/status", 0)], identifier=3)
        sub.publication("meshcore/test/status", b"online", 0, retained=True)
        pub.publish("meshcore/test/status", b"replacement", qos=1, retained=True)
        sub.publication("meshcore/test/status", b"replacement", 1)
        pub.publish("meshcore/test/q0", b"zero")
        sub.publication("meshcore/test/q0", b"zero", 0)
        # QoS2 routes once at PUBLISH, not PUBREL, matching Mochi's handoff boundary.
        pub.publish("meshcore/test/q2", b"exactly once", qos=2, identifier=11)
        sub.publication("meshcore/test/q2", b"exactly once", 2)
        pub.publish("meshcore/test/q2", b"exactly once", qos=2, identifier=11, duplicate=True)
        sub.idle()
        pub.conn.sendall(frame(98, b"\0\x0b"))
        pub.expect(112, b"\0\x0b")
        pub.conn.sendall(frame(98, b"\0\x0b"))
        pub.expect(112, b"\0\x0b", reason=146 if version == 5 else 0)
        # QoS1 retransmission can redeliver; a receiver, not the broker, owns deduplication.
        pub.publish("meshcore/test/q1", b"at least once", qos=1, identifier=12)
        sub.publication("meshcore/test/q1", b"at least once", 1)
        pub.publish("meshcore/test/q1", b"at least once", qos=1, identifier=12, duplicate=True)
        sub.publication("meshcore/test/q1", b"at least once", 1)
        # Reconnect with an outstanding QoS1 delivery: same identifier, DUP bit.
        pub.publish("meshcore/test/resume", b"pending", qos=1, identifier=13)
        pending = sub.publication("meshcore/test/resume", b"pending", 1, acknowledge=False)
        sub.close()
        time.sleep(0.06)
        resumed = Client(port, "subscriber", clean=False, version=version)
        clients.append(resumed)
        assert resumed.connack == (32, b"\1\0"), resumed.connack
        replay = resumed.publication("meshcore/test/resume", b"pending", 1, duplicate=True)
        assert replay == pending
        sub = resumed
        # Offline persistent subscription queues a QoS1 message for the same session.
        sub.close()
        time.sleep(0.06)
        pub.publish("meshcore/test/offline", b"queued", qos=1, identifier=14)
        sub = Client(port, "subscriber", clean=False, version=version)
        clients.append(sub)
        assert sub.connack == (32, b"\1\0")
        # Mochi marks even an unsent queued PUBLISH as duplicate on resume.
        sub.publication("meshcore/test/offline", b"queued", 1, duplicate=True)
        # Incoming QoS2 state survives a publisher reconnect and suppresses replay.
        pub.publish("meshcore/test/restart", b"qos2 resume", qos=2, identifier=15)
        sub.publication("meshcore/test/restart", b"qos2 resume", 2)
        pub.close()
        time.sleep(0.06)
        pub = Client(port, "publisher", clean=False, version=version)
        clients.append(pub)
        assert pub.connack == (32, b"\1\0")
        # The Go broker replays the pending PUBREC before accepting the duplicate.
        pub.expect(80, b"\0\x0f")
        pub.publish("meshcore/test/restart", b"qos2 resume", qos=2, identifier=15, duplicate=True)
        sub.idle()
        pub.conn.sendall(frame(98, b"\0\x0f"))
        pub.expect(112, b"\0\x0f")
        # Outbound QoS2 resumes at PUBREL after receiving PUBREC, never repeats application data.
        pub.publish("meshcore/test/phase", b"phase two", qos=2, identifier=16)
        phase = sub.publication("meshcore/test/phase", b"phase two", 2, acknowledge=False)
        sub.conn.sendall(frame(80, phase.to_bytes(2, "big")))
        sub.expect(98, phase.to_bytes(2, "big"))
        sub.close()
        time.sleep(0.06)
        sub = Client(port, "subscriber", clean=False, version=version)
        clients.append(sub)
        assert sub.connack == (32, b"\1\0")
        sub.expect(98, phase.to_bytes(2, "big"))
        sub.conn.sendall(frame(80, phase.to_bytes(2, "big")))
        sub.expect(98, phase.to_bytes(2, "big"))
        sub.conn.sendall(frame(112, phase.to_bytes(2, "big")))
        pub.conn.sendall(frame(98, b"\0\x10"))
        pub.expect(112, b"\0\x10")
        # Unsubscribe removes only the named filter, then retained deletion reaches live subscribers.
        sub.unsubscribe(["meshcore/+/status"])
        pub.publish("meshcore/test/status", b"", qos=1, retained=True)
        sub.publication("meshcore/test/status", b"", 1)
        late = client("late")
        late.subscribe([("meshcore/test/status", 1)])
        late.idle()
        sub.unsubscribe(["meshcore/test/#", "$fixture/#"])
        pub.publish("meshcore/test/quiet", b"unsubscribed", qos=1)
        sub.idle()
        # A will is delivered over the local broker on EOF and retained for new subscribers.
        late.subscribe([("meshcore/test/will", 1)], identifier=4)
        doomed = client("will-owner", will=("meshcore/test/will", b"offline", True), qos=1)
        doomed.close()
        late.publication("meshcore/test/will", b"offline", 1)
        late.subscribe([("meshcore/test/will", 1)], identifier=5)
        late.publication("meshcore/test/will", b"offline", 1, retained=True)
        # The old live connection receives DISCONNECT and its will on session takeover.
        taken = client("taken", clean=False, will=("meshcore/test/will", b"taken over", False), qos=1)
        replacement = Client(port, "taken", clean=False, version=version)
        clients.append(replacement)
        assert replacement.connack == (32, b"\1\0")
        taken.expect(224, b"", reason=142 if version == 5 else 0)
        assert taken.conn.recv(1) == b""
        late.publication("meshcore/test/will", b"taken over", 1)
        graceful = client("no-will", will=("meshcore/test/will", b"wrong", True), qos=1)
        graceful.close(graceful=True)
        late.idle()
        timeout = client("keepalive", will=("meshcore/test/will", b"timeout", False), qos=1, keepalive=1)
        timeout.conn.sendall(frame(192))
        timeout.expect(208, b"")
        time.sleep(0.8)
        timeout.conn.sendall(frame(192))
        timeout.expect(208, b"")
        late.idle()
        late.conn.settimeout(3)
        late.publication("meshcore/test/will", b"timeout", 1)
        # Clean start drops prior subscriptions and does not set session-present.
        sub.close()
        clean = Client(port, "subscriber", clean=True, version=version)
        clients.append(clean)
        assert clean.connack == (32, b"\0\0")
        pub.publish("meshcore/test/clean", b"no inherited subscription", qos=1)
        clean.idle()
        # Shared groups receive one live copy each, but no subscription-triggered retained copy.
        pub.publish("meshcore/shared/retained", b"stored", qos=1, retained=True)
        shared_a, shared_b, other_group = client("shared-a"), client("shared-b"), client("shared-other")
        for value, group in [(shared_a, "team"), (shared_b, "team"), (other_group, "other")]:
            value.subscribe([(f"$share/{group}/meshcore/shared/#", 1)])
            value.idle()
        pub.publish("meshcore/shared/live", b"one per group", qos=1, retained=True)
        other_group.publication("meshcore/shared/live", b"one per group", 1)
        readers = select.select([shared_a.conn, shared_b.conn], [], [], 2)[0]
        assert len(readers) == 1, readers
        selected, excluded = (shared_a, shared_b) if readers[0] is shared_a.conn else (shared_b, shared_a)
        selected.publication("meshcore/shared/live", b"one per group", 1)
        excluded.idle()
        for value in (shared_a, shared_b, other_group):
            value.close(graceful=True)
        persistent_shared = client("shared-persistent", clean=False)
        persistent_shared.subscribe([("$share/offline/meshcore/shared/offline", 1)])
        persistent_shared.close()
        time.sleep(0.06)
        pub.publish("meshcore/shared/offline", b"shared resume", qos=1)
        persistent_shared = Client(port, "shared-persistent", clean=False, version=version)
        clients.append(persistent_shared)
        assert persistent_shared.connack == (32, b"\1\0")
        persistent_shared.publication("meshcore/shared/offline", b"shared resume", 1, duplicate=True)
        return "subscriber retained QoS0/1/2 duplicate reconnect offline unsubscribe will keepalive clean"
    finally:
        for value in clients:
            with contextlib.suppress(OSError):
                value.close()


class BrokerTest(unittest.TestCase):
    binaries = [ROOT / "build/broker-oracle", ROOT / "build/hew-broker", ROOT / "build/hew-broker-release"]

    def test_generic_listener_dual_stack(self):
        for binary in [ROOT / "build/broker-checks", ROOT / "build/broker-checks-release"]:
            with self.subTest(binary=binary.name):
                proc = subprocess.Popen([str(binary), "dual-stack"], stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True)
                try:
                    self.assertTrue(select.select([proc.stdout], [], [], 5)[0])
                    ready = proc.stdout.readline().strip()
                    self.assertTrue(ready.startswith("LISTENER_READY "), ready)
                    port = int(ready.split("port=")[1])
                    for family, address in [(socket.AF_INET, "127.0.0.1"), (socket.AF_INET6, "::1")]:
                        with socket.socket(family) as conn:
                            conn.settimeout(2)
                            conn.connect((address, port))
                            conn.sendall(b"owned listener")
                            self.assertEqual(take(conn, 14), b"owned listener")
                            self.assertEqual(conn.recv(1), b"")
                    output, error = proc.communicate(timeout=5)
                    self.assertEqual(proc.returncode, 0, error)
                    self.assertIn("LISTENER_DUAL_STACK_OK", output)
                    self.assertEqual(error, "")
                finally:
                    if proc.poll() is None:
                        proc.kill()
                        proc.communicate()

    def test_differential_lifecycle(self):
        summaries = []
        for binary in self.binaries:
            for version in (3, 4, 5):
                with self.subTest(binary=binary.name, version=version), endpoint(binary) as (port, _):
                    summaries.append(lifecycle(port, version))
        self.assertEqual(len(set(summaries)), 1)

    def test_authentication_and_protocol_inventory(self):
        for binary in self.binaries:
            with self.subTest(binary=binary.name), endpoint(binary, ("reader", "fixture-password")) as (port, _):
                for version in (3, 4, 5):
                    for username, password, code in [
                        (None, None, 5), ("reader", "wrong", 5), ("wrong", "fixture-password", 5),
                        ("reader", "fixture-password", 0),
                    ]:
                        value = Client(port, "auth", username=username, password=password, version=version)
                        self.assertEqual(value.connack, (32, bytes([0, 134 if version == 5 and code else code])))
                        value.close()
                for version in (3, 4):
                    value = Client(port, f"protocol-{version}", version=version,
                                   username="reader", password="fixture-password")
                    self.assertEqual(value.connack, (32, b"\0\0"))
                    value.close()
                value = Client(port, "mqtt5", version=5, username="reader", password="fixture-password")
                self.assertEqual(value.connack, (32, b"\0\0"))
                self.assertEqual(value.server_properties.get(33), [1024])
                value.close()
                shared = Client(port, "shared-contract", username="reader", password="fixture-password")
                try:
                    shared.conn.sendall(frame(130, b"\0\1" + field("$share/fixture/meshcore/#") + b"\1"))
                    shared.expect(144, b"\0\1\1")
                finally:
                    shared.close()

    def test_authenticated_hostname_and_dual_stack_bind(self):
        credentials = ("bind-reader", "fixture-bind-password")
        for binary in self.binaries:
            for host in ("localhost", "0.0.0.0"):
                with self.subTest(binary=binary.name, host=host), endpoint(binary, credentials, host) as (port, _):
                    value = Client(port, "bind-ipv4", version=5,
                                   username=credentials[0], password=credentials[1])
                    self.assertEqual(value.connack, (32, b"\0\0"))
                    value.conn.sendall(frame(192))
                    value.expect(208, b"")
                    value.close(graceful=True)
                    if host == "0.0.0.0":
                        with socket.socket(socket.AF_INET6) as conn:
                            conn.settimeout(2)
                            conn.connect(("::1", port))
                            body = (field("MQTT") + b"\x05\xc2\x00\x0a\x00" + field("bind-ipv6") +
                                    field(credentials[0]) + field(credentials[1]))
                            conn.sendall(frame(16, body))
                            self.assertEqual(packet(conn), (32, b"\0\0\x03\x21\x04\0"))
                            conn.sendall(frame(192))
                            self.assertEqual(packet(conn), (208, b""))
                            conn.sendall(frame(224))

    def test_malformed_and_packet_boundary(self):
        for binary in self.binaries:
            with self.subTest(binary=binary.name), endpoint(binary) as (port, _):
                for malformed in [
                    b"\xc1\0", b"\x30\xff\xff\xff\xff\xff", b"\x82\2\0\1", b"\x10\0",
                ]:
                    value = Client(port, "malformed")
                    value.conn.sendall(malformed)
                    value.conn.settimeout(3)
                    self.assertEqual(value.conn.recv(1), b"", malformed.hex())
                    value.close()
                # Match the configured broker's tolerated nonempty PING and zero PUBACK.
                for malformed, baseline in [
                    (b"\xc0\1\0", "pong"), (b"\x40\2\0\0", "idle"),
                    (b"\x30\xff\xff\xff\xff", "idle"),
                ]:
                    value = Client(port, "strict-validation")
                    try:
                        value.conn.sendall(malformed)
                        if baseline == "pong":
                            value.expect(208, b"")
                        else:
                            value.idle()
                    finally:
                        value.close()
                sub = Client(port, "boundary-sub")
                sub.subscribe([("b", 0)])
                pub = Client(port, "boundary-pub")
                # Actual Mochi v2 limit compares RemainingLength + 1, excluding its encoding.
                payload = b"x" * (65535 - len(field("b")))
                self.assertEqual(len(frame(48, field("b") + payload)), 65539)
                pub.publish("b", payload)
                sub.publication("b", payload, 0)
                pub.publish("b", payload + b"x")
                pub.conn.settimeout(3)
                try:
                    self.assertEqual(pub.conn.recv(1), b"")
                except ConnectionResetError:
                    pass  # Rejection before consuming the oversized body can produce TCP RST.
                sub.idle()
                sub.close()
                pub.close()
                for version in (3, 4, 5):
                    value = Client(port, f"will-topic-{version}", version=version,
                                   will=("will/+/#", b"accepted", False))
                    self.assertEqual(value.connack, (32, b"\0\0"))
                    value.close(graceful=True)
                value = Client(port, "tolerated-options", version=5, properties=b"\x17\x02\x19\x02")
                value.conn.sendall(frame(130, b"\0\1\0" + field("tolerated/options") + b"\xf0"))
                value.expect(144, b"\0\1\0\0")
                value.publish("tolerated/options", b"format", properties=b"\x01\x02")
                value.publication("tolerated/options", b"format", 0)
                self.assertEqual(value.last_properties[1], [2])
                value.close(graceful=True)

    def test_standard_mosquitto_clients(self):
        self.assertTrue(shutil.which("mosquitto_sub") and shutil.which("mosquitto_pub"))
        for binary in self.binaries:
            with self.subTest(binary=binary.name), endpoint(binary) as (port, _):
                for version in ("mqttv31", "mqttv311", "mqttv5"):
                    common = ["-h", "127.0.0.1", "-p", str(port), "-V", version]
                    for qos in (0, 1, 2):
                        topic = f"meshcore/standard/{version}/q{qos}"
                        subprocess.run(["mosquitto_pub", *common, "-t", topic, "-q", str(qos),
                                        "-r", "-m", f"retained-qos{qos}"], check=True, timeout=5)
                        result = subprocess.run(["mosquitto_sub", *common, "-t", topic, "-q", str(qos),
                                                 "-C", "1", "-W", "3"], check=True,
                                                capture_output=True, timeout=5)
                        self.assertEqual(result.stdout, f"retained-qos{qos}\n".encode())

    def test_system_topics(self):
        names = {"version", "time", "uptime", "started", "load/bytes/received", "load/bytes/sent",
                 "clients/connected", "clients/disconnected", "clients/maximum", "clients/total",
                 "packets/received", "packets/sent", "messages/received", "messages/sent",
                 "messages/dropped", "messages/inflight", "retained", "subscriptions",
                 "system/memory", "system/threads"}
        for binary in self.binaries:
            with self.subTest(binary=binary.name), endpoint(binary) as (port, _):
                value = Client(port, "sys-reader", version=5)
                try:
                    value.subscribe([("$SYS/broker/#", 0)])
                    observed, retained = {}, set()
                    deadline = time.monotonic() + 3
                    while set(observed) != names and time.monotonic() < deadline:
                        header, topic, payload, _ = value.read_publication()
                        name = topic.decode().removeprefix("$SYS/broker/")
                        self.assertIn(name, names)
                        observed[name] = payload.decode()
                        if header & 1:
                            retained.add(name)
                    self.assertEqual(set(observed), names)
                    self.assertEqual(retained, names)
                    for name in names - {"version"}:
                        self.assertGreaterEqual(int(observed[name]), 0, name)
                    self.assertGreater(int(observed["system/memory"]), 0)
                    self.assertGreater(int(observed["system/threads"]), 0)
                    self.assertLess(abs(int(observed["time"]) - int(time.time())), 5)
                    deadline = time.monotonic() + 3
                    while time.monotonic() < deadline:
                        header, topic, payload, _ = value.read_publication()
                        name = topic.decode().removeprefix("$SYS/broker/")
                        observed[name] = payload.decode()
                        if name == "clients/connected" and int(payload) >= 1:
                            self.assertFalse(header & 1)
                            break
                    else:
                        self.fail("system topic periodic update did not reflect its subscriber")
                finally:
                    value.close()

    def test_client_and_inflight_bounds(self):
        for binary in self.binaries:
            with self.subTest(binary=binary.name), endpoint(binary) as (port, _):
                clients = []
                try:
                    for index in range(128):
                        value = Client(port, f"capacity-{index}")
                        clients.append(value)
                        self.assertEqual(value.connack, (32, b"\0\0"))
                    rejected = Client(port, "over-capacity")
                    clients.append(rejected)
                    self.assertEqual(rejected.connack, (32, b"\0\3"))
                    clients[0].close(graceful=True)
                    time.sleep(0.06)
                    admitted = Client(port, "capacity-recovered")
                    clients.append(admitted)
                    self.assertEqual(admitted.connack, (32, b"\0\0"))
                finally:
                    for value in clients:
                        with contextlib.suppress(OSError):
                            value.close()
            with self.subTest(binary=binary.name, bound="inflight"), endpoint(binary) as (port, _):
                sub = Client(port, "inflight", clean=False)
                pub = Client(port, "producer")
                try:
                    sub.subscribe([("bounded", 1)])
                    for index in range(129):
                        pub.publish("bounded", b"limit", qos=1, identifier=index + 1)
                    identifiers = set()
                    for index in range(128):
                        identifiers.add(sub.publication("bounded", b"limit", 1, acknowledge=False))
                    self.assertEqual(len(identifiers), 128)
                    sub.idle()
                    for identifier in identifiers:
                        sub.conn.sendall(frame(64, identifier.to_bytes(2, "big")))
                    time.sleep(0.06)
                    pub.publish("bounded", b"recovered", qos=1)
                    sub.publication("bounded", b"recovered", 1)
                finally:
                    sub.close()
                    pub.close()

    def test_private_configuration_errors(self):
        path = ROOT / "build" / f"broker-invalid-{os.getpid()}.config"
        try:
            for binary in self.binaries[1:]:
                for contents, mode in [
                    ("host=127.0.0.1\n", 0o644),
                    ("host=localhost\n", 0o600),
                    ("host=0.0.0.0\n", 0o600),
                    ("host=127.0.0.1\0unexpected\n", 0o600),
                    ("host=127.0.0.1\nusername=fixture-secret\n", 0o600),
                    ("host=127.0.0.1\npassword=fixture-secret\n", 0o600),
                    ("port=0\nport=1\n", 0o600),
                    ("port=65536\n", 0o600),
                    ("unknown=fixture-secret\n", 0o600),
                ]:
                    with self.subTest(binary=binary.name, mode=mode):
                        path.write_text(contents)
                        path.chmod(mode)
                        result = subprocess.run([str(binary), str(path)], capture_output=True, timeout=5)
                        self.assertNotEqual(result.returncode, 0)
                        self.assertNotIn(b"MQTT_BROKER_READY", result.stdout)
                        self.assertNotIn(b"fixture-secret", result.stdout + result.stderr)
        finally:
            if path.exists():
                path.unlink()

    def test_slow_client_is_bounded_and_other_clients_remain_usable(self):
        for binary in self.binaries:
            with self.subTest(binary=binary.name), endpoint(binary) as (port, _):
                slow = Client(port, "slow", keepalive=0)
                producer = Client(port, "flood", keepalive=0)
                try:
                    slow.conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
                    slow.subscribe([("flood", 0)])
                    producer.conn.settimeout(40)
                    encoded = frame(48, field("flood") + b"x" * 8192)
                    producer.conn.sendall(encoded * 1000)
                    producer.conn.sendall(frame(192))
                    producer.expect(208, b"")
                    slow.conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
                    slow.conn.settimeout(1)
                    slow.conn.sendall(frame(192))
                    received, deadline = 0, time.monotonic() + 30
                    while time.monotonic() < deadline:
                        header, data = packet(slow.conn)
                        if header == 208:
                            self.assertEqual(data, b"")
                            break
                        self.assertEqual(header, 48)
                        received += 1
                    else:
                        self.fail("slow subscriber did not respond after draining its queue")
                    self.assertGreater(received, 0)
                    self.assertLess(received, 1000)
                    healthy = Client(port, "healthy")
                    try:
                        self.assertEqual(healthy.connack, (32, b"\0\0"))
                        healthy.conn.sendall(frame(192))
                        healthy.expect(208, b"")
                    finally:
                        healthy.close()
                finally:
                    slow.close()
                    producer.close()

    def test_mqtt5_properties_options_aliases_and_limits(self):
        metadata = (b"\x01\x01\x03" + field("text/plain") + b"\x08" + field("reply") +
                    b"\x09" + field(b"\x00\xff") + b"\x26" + field("key") + field("value"))
        for binary in self.binaries:
            with self.subTest(binary=binary.name), endpoint(binary) as (port, _):
                clients = []
                def client(name, **kwargs):
                    value = Client(port, name, version=5, **kwargs)
                    clients.append(value)
                    self.assertEqual(value.connack, (32, b"\0\0"))
                    return value
                try:
                    pub = client("properties-publisher")
                    sub = client("properties-subscriber", properties=b"\x22\x00\x02")
                    sub.subscribe([("fixture/value", 0)], properties=b"\x0b\x07")
                    sub.subscribe([("fixture/#", 0)], properties=b"\x0b\x03")
                    pub.publish("fixture/value", b"first", properties=metadata + b"\x23\x00\x01")
                    sub.publication("fixture/value", b"first", 0)
                    self.assertEqual(sub.last_properties[11], [3, 7])
                    self.assertEqual(sub.last_properties[38], [(b"key", b"value")])
                    self.assertEqual(sub.last_properties[3], [b"text/plain"])
                    self.assertEqual(sub.last_properties[1], [1])
                    self.assertEqual(sub.last_properties[8], [b"reply"])
                    self.assertEqual(sub.last_properties[9], [b"\x00\xff"])
                    self.assertEqual(sub.last_properties[35], [1])
                    pub.publish("", b"reuse", properties=b"\x23\x00\x01")
                    sub.publication("fixture/value", b"reuse", 0)
                    pub.publish("", b"unknown", qos=1, properties=b"\x23\x00\x63")
                    sub.idle()
                    pub.conn.sendall(frame(240, b"\x00\x00"))
                    pub.conn.sendall(frame(192))
                    pub.expect(208, b"")
                    local = client("no-local")
                    local.subscribe([("self/value", 4), ("self/#", 0)])
                    local.publish("self/value", b"self")
                    local.idle()
                    pub.publish("fixture/retained", b"stored", retained=True)
                    sub.publication("fixture/retained", b"stored", 0)
                    retained = client("retain-options")
                    retained.subscribe([("fixture/retained", 8)])
                    retained.publication("fixture/retained", b"stored", 0, retained=True)
                    retained.subscribe([("fixture/retained", 24)])
                    retained.idle()
                    retained.unsubscribe(["fixture/retained"])
                    retained.subscribe([("fixture/retained", 40)])
                    retained.idle()
                    pub.publish("fixture/retained", b"live-retain", retained=True)
                    retained.publication("fixture/retained", b"live-retain", 0, retained=True)
                    sub.publication("fixture/retained", b"live-retain", 0)
                    quiet = client("no-problem-info", properties=b"\x17\x00")
                    quiet.subscribe([("properties/quiet", 0)])
                    pub.publish("properties/quiet", b"ok", properties=metadata)
                    quiet.publication("properties/quiet", b"ok", 0)
                    self.assertNotIn(38, quiet.last_properties)
                    limited = client("small-packets", properties=b"\x27\x00\x00\x00\x64")
                    limited.subscribe([("properties/limited", 0)])
                    pub.publish("properties/limited", b"ok", properties=b"\x26" + field("long") + field("x" * 200))
                    limited.publication("properties/limited", b"ok", 0)
                    self.assertNotIn(38, limited.last_properties)
                    pub.publish("properties/limited", b"x" * 200)
                    limited.idle()
                    limited.conn.sendall(frame(192))
                    limited.expect(208, b"")
                    pub.publish("$fixture/value", b"namespace", retained=True)
                    wildcard = client("retained-namespace")
                    wildcard.subscribe([("$fixture/#", 0)])
                    wildcard.publication("$fixture/value", b"namespace", 0, retained=True)
                    wildcard.unsubscribe(["$fixture/#"])
                    wildcard.subscribe([("#", 0)])
                    received = {wildcard.read_publication()[1] for _ in range(2)}
                    self.assertEqual(received, {b"$fixture/value", b"fixture/retained"})
                    pub.conn.sendall(frame(50, field("fixture/value") + b"\x00\x51" + b"\x03\x23\x00\x00" + b"invalid"))
                    pub.expect(224, b"", reason=148)
                finally:
                    for value in clients:
                        value.close()

    def test_mqtt5_receive_maximum_deferred_reconnect(self):
        # Preserve the configured oracle's deferred-flight deletion behavior.
        for binary in self.binaries:
            with self.subTest(binary=binary.name), endpoint(binary) as (port, _):
                sub = Client(port, "quota-session", clean=False, version=5,
                             properties=b"\x11\x00\x00\x00\x1e\x21\x00\x01")
                pub = Client(port, "quota-publisher", version=5)
                try:
                    sub.subscribe([("quota", 1)])
                    for value in (b"one", b"two", b"three"):
                        pub.publish("quota", value, qos=1)
                    first = sub.publication("quota", b"one", 1, acknowledge=False)
                    sub.idle()
                    sub.conn.sendall(frame(64, first.to_bytes(2, "big")))
                    sub.publication("quota", b"two", 1)
                    sub.conn.sendall(frame(192))
                    sub.expect(208, b"")
                    sub.idle()
                    sub.close(graceful=True)
                    sub = Client(port, "quota-session", clean=False, version=5,
                                 properties=b"\x11\x00\x00\x00\x1e\x21\x00\x01")
                    self.assertEqual(sub.connack, (32, b"\x01\x00"))
                    sub.publication("quota", b"three", 1, duplicate=True)
                finally:
                    sub.close()
                    pub.close()

    def test_mqtt5_expiry_and_delayed_wills(self):
        for binary in self.binaries:
            with self.subTest(binary=binary.name), endpoint(binary) as (port, _):
                pub = Client(port, "expiry-publisher", version=5)
                sub = Client(port, "expiry-subscriber", version=5)
                try:
                    pub.publish("expires/value", b"short", qos=1, retained=True,
                                properties=b"\x02\x00\x00\x00\x01")
                    sub.subscribe([("expires/#", 0)])
                    sub.publication("expires/value", b"short", 0, retained=True)
                    self.assertEqual(sub.last_properties[2], [1])
                    time.sleep(3.1)
                    sub.subscribe([("expires/#", 0)])
                    sub.idle()
                    session = Client(port, "expires-session", clean=False, version=5,
                                     properties=b"\x11\x00\x00\x00\x01")
                    session.subscribe([("expires/offline", 1)])
                    session.close(graceful=True)
                    time.sleep(3.1)
                    session = Client(port, "expires-session", clean=False, version=5)
                    self.assertEqual(session.connack, (32, b"\x00\x00"))
                    session.close(graceful=True)
                    sub.subscribe([("will/#", 0)])
                    will_props = b"\x18\x00\x00\x00\x01\x26" + field("will-key") + field("will-value")
                    will = Client(port, "delay-cancel", version=5,
                                  properties=b"\x11\x00\x00\x00\x1e",
                                  will=("will/cancel", b"cancelled", True), will_properties=will_props)
                    will.close()
                    replacement = Client(port, "delay-cancel", version=5)
                    replacement.close(graceful=True)
                    sub.idle(2.2)
                    will = Client(port, "delay-deliver", version=5,
                                  properties=b"\x11\x00\x00\x00\x01",
                                  will=("will/clamped", b"clamped", False),
                                  will_properties=b"\x18\x00\x00\x00\x05" + will_props[5:])
                    will.close()
                    sub.conn.settimeout(4)
                    sub.publication("will/clamped", b"clamped", 0)
                    will = Client(port, "delay-retained", version=5,
                                  properties=b"\x11\x00\x00\x00\x1e",
                                  will=("will/delivered", b"delayed", True), will_properties=will_props)
                    will.close()
                    sub.publication("will/delivered", b"delayed", 0)
                    self.assertEqual(sub.last_properties[38], [(b"will-key", b"will-value")])
                    sub.subscribe([("will/delivered", 0)])
                    sub.publication("will/delivered", b"delayed", 0, retained=True)
                    self.assertEqual(sub.last_properties[2], [1])
                    implicit = Client(port, "implicit-expiry", version=5)
                    implicit.subscribe([("expires/implicit", 1)])
                    implicit.close(graceful=True)
                    time.sleep(0.06)
                    implicit = Client(port, "implicit-expiry", clean=False, version=5)
                    self.assertEqual(implicit.connack, (32, b"\0\0"))
                    implicit.close(graceful=True)
                    assigned = Client(port, "", version=5, properties=b"\x11\x00\x00\x00\x1e")
                    assigned_name = assigned.server_properties[18][0].decode()
                    self.assertTrue(assigned_name)
                    assigned.close(graceful=True)
                    assigned = Client(port, assigned_name, clean=False, version=5)
                    self.assertEqual(assigned.connack, (32, b"\x01\0"))
                    assigned.close(graceful=True)
                    will = Client(port, "disconnect-normal", version=5,
                                  will=("will/normal", b"not-sent", False))
                    will.conn.sendall(frame(224, b"\x04"))
                    will.close()
                    sub.idle()
                    will = Client(port, "disconnect-will", version=5,
                                  properties=b"\x11\x00\x00\x00\x1e",
                                  will=("will/disconnect", b"explicit", False), will_properties=will_props)
                    will.conn.sendall(frame(224, b"\x04\x05\x11\x00\x00\x00\x00"))
                    will.close()
                    sub.idle()
                    sub.conn.settimeout(4)
                    sub.publication("will/disconnect", b"explicit", 0)
                    zero = Client(port, "zero-expiry", version=5)
                    zero.conn.sendall(frame(224, b"\x00\x05\x11\x00\x00\x00\x01"))
                    zero.expect(224, b"", reason=130)
                    zero.close()
                finally:
                    sub.close()
                    pub.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
