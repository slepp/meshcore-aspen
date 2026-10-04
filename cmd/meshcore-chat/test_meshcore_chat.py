# SPDX-License-Identifier: Apache-2.0
import argparse
from collections import deque
import json
from pathlib import Path
import socket
import struct
import tempfile
import threading
import time
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import meshcore_chat as chat

PEER = bytes(range(32))
SENDER = bytes(range(32, 64))


def record(key=PEER):
    result = bytearray(148)
    result[0], result[33], result[35] = 3, 1, 255
    result[1:33] = key
    result[36:100] = bytes(range(64))
    result[100:132] = b"existing-name".ljust(32, b"\0")
    result[132:] = struct.pack("<IIII", 1700000000, 123, 456, 1700000001)
    return bytes(result)


def dm(code=16, key=PEER, text=b"Theme A", kind=0):
    header = b"\x10\xf8\0\0" if code == 16 else b"\x07"
    return header + key[:6] + bytes((255, kind)) + struct.pack("<I", 1700000002) + text


class Capture:
    def __init__(self):
        self.events = []

    def emit(self, event, **fields):
        self.events.append({"event": event, **fields})


class FakeClient:
    def __init__(self, current=None, messages=()):
        self.current = current
        self.messages = deque(messages)
        self.pushes = []
        self.commands = []
        self.listing = deque()
        self.collision = None
        self.public_key = SENDER.hex()
        self.info = bytes(48) + struct.pack("<II", 910525, 62500) + b"\x07\x05"

    def __enter__(self):
        return self

    def __exit__(self, *_):
        pass

    def response(self, _):
        return self.listing.popleft()

    def command(self, data, allow_error=False):
        self.commands.append(data)
        if data[0] == 30:
            return self.current if self.current is not None else b"\x01\x02"
        if data[0] == 9:
            self.current = b"\x03" + data[1:]
            return b"\0"
        if data[0] == 4:
            records = [self.current] + ([self.collision] if self.collision else [])
            self.listing = deque(records + [b"\x04" + bytes(4)])
            return b"\x02" + struct.pack("<I", len(records))
        if data[0] == 10:
            return self.messages.popleft() if self.messages else b"\x0a"
        if data[0] == 7:
            return b"\0"
        if data[0] == 2:
            self.pushes += [b"\x82abcd" + struct.pack("<I", 900),
                            b"\x82xxxx" + struct.pack("<I", 100)]
            return b"\x06\0abcd" + struct.pack("<I", 2000)
        if data[0] == 5:
            return b"\x09" + struct.pack("<I", 1700000003)
        raise AssertionError(data[0])

    def frame(self, _):
        raise TimeoutError()


class ChatTests(unittest.TestCase):
    def test_wait_reply_filters_and_finishes_partial_tail_without_radio(self):
        before = {"event": "dm", "sender": PEER.hex(), "authenticated_by": "native_companion",
                  "full_key_verified": True, "message_id": "a" * 64, "text": "Already processed"}
        reply = {**before, "message_id": "b" * 64, "text": "Next reply"}
        with tempfile.TemporaryDirectory(dir=chat.ROOT / ".tmp") as directory:
            path = Path(directory) / "replies.jsonl"
            initial = [before, {"event": "heartbeat"}, before,
                       {**reply, "sender": SENDER.hex()},
                       {**reply, "full_key_verified": False}]
            path.write_bytes(b"".join(json.dumps(row).encode() + b"\n" for row in initial))
            path.chmod(0o600)
            tail = json.dumps(reply).encode() + b"\n"
            with path.open("ab") as output:
                output.write(tail[:30])
            slept = []
            def complete_tail(seconds):
                slept.append(seconds)
                with path.open("ab") as output:
                    output.write(tail[30:])
            with patch.object(chat.socket, "create_connection", side_effect=AssertionError("radio I/O")):
                self.assertEqual(chat.wait_reply(path, PEER, "a" * 64, 1, complete_tail), reply)
            self.assertEqual(len(slept), 1)
            self.assertEqual(chat.wait_reply(path, PEER, timeout=1), before)
            with self.assertRaisesRegex(ValueError, "--after-id was not found"):
                chat.wait_reply(path, PEER, "c" * 64, 1)
            with self.assertRaisesRegex(TimeoutError, "Timed out"):
                chat.wait_reply(path, PEER, "b" * 64, 0.02)
            with path.open("ab") as output:
                output.write(b"invalid json\n")
            with self.assertRaisesRegex(ValueError, "Invalid chat JSONL"):
                chat.wait_reply(path, PEER, "b" * 64, 1)

    def test_message_limit_and_ascii(self):
        self.assertEqual(chat.message_text("A" * 120), "A" * 120)
        for text in ("", "A" * 121, "caf\u00e9", "two\nlines", "\0", "\x7f"):
            with self.subTest(text=text), self.assertRaises(argparse.ArgumentTypeError):
                chat.message_text(text)

    def test_full_public_key_required(self):
        self.assertEqual(chat.public_key(PEER.hex()), PEER)
        for value in (PEER[:6].hex(), "x" * 64, PEER.hex() + "\n"):
            with self.assertRaises(argparse.ArgumentTypeError):
                chat.public_key(value)

    def test_direct_update_preserves_every_other_byte(self):
        before = record()
        client = FakeClient(before)
        self.assertTrue(chat.direct_contact(client, PEER))
        self.assertEqual(client.current[:35], before[:35])
        self.assertEqual(client.current[35], 0)
        self.assertEqual(client.current[36:], before[36:])
        self.assertFalse(chat.direct_contact(client, PEER))
        self.assertEqual(sum(data[0] == 9 for data in client.commands), 1)

    def test_missing_contact_is_added_and_read_back(self):
        client = FakeClient()
        chat.direct_contact(client, PEER)
        self.assertEqual(client.current[1:33], PEER)
        self.assertEqual(client.current[35], 0)
        self.assertEqual(client.current[100:117], b"peer-" + PEER[:6].hex().encode())
        self.assertGreater(struct.unpack_from("<I", client.current, 144)[0], 0)

    def test_route_update_accepts_server_modified_time_but_not_changed_contact_fields(self):
        class UpdatingClient(FakeClient):
            corrupt_name = False

            def command(self, data, allow_error=False):
                response = super().command(data, allow_error)
                if data[0] == 9:
                    updated = bytearray(self.current)
                    struct.pack_into("<I", updated, 144, 1700000010)
                    if self.corrupt_name:
                        updated[100] ^= 1
                    self.current = bytes(updated)
                return response

        client = UpdatingClient(record())
        self.assertTrue(chat.direct_contact(client, PEER))
        self.assertEqual(struct.unpack_from("<I", client.current, 144)[0], 1700000010)
        client = UpdatingClient(record())
        client.corrupt_name = True
        with self.assertRaisesRegex(ValueError, "preserve the contact data"):
            chat.direct_contact(client, PEER)

    def test_contact_count_can_include_filtered_entries(self):
        client = FakeClient(record())
        command = client.command
        def filtered(data, allow_error=False):
            response = command(data, allow_error)
            return b"\x02\x02\0\0\0" if data[0] == 4 else response
        client.command = filtered
        chat.verify_peer(client, PEER)

    def test_full_key_and_prefix_collision_fail_closed(self):
        wrong = PEER[:6] + b"x" * 26
        with self.assertRaises(ValueError):
            chat.contact(FakeClient(record(wrong)), PEER)
        client = FakeClient(record())
        client.collision = record(wrong)
        with self.assertRaises(ValueError):
            chat.verify_peer(client, PEER)

    def test_both_dm_variants_and_signed_plain(self):
        for code in (7, 16):
            decoded = chat.decode_dm(dm(code), PEER)
            self.assertEqual(decoded["text"], "Theme A")
            self.assertEqual(decoded["sender"], PEER.hex())
            self.assertEqual(decoded["snr_db"], -2 if code == 16 else None)
        decoded = chat.decode_dm(dm(text=b"abcdSigned", kind=2), PEER)
        self.assertEqual(decoded["text"], "Signed")
        self.assertEqual(decoded["signed_sender_prefix"], b"abcd".hex())
        with self.assertRaises(ValueError):
            chat.decode_dm(dm(text=b"abc", kind=2), PEER)
        with self.assertRaises(ValueError):
            chat.decode_dm(dm(text=b"\xff"), PEER)

    def test_queue_filters_private_content_marks_backlog_and_deduplicates(self):
        events = Capture()
        session = chat.Chat(PEER, events)
        session.client = FakeClient(record(), [
            dm(key=b"x" * 32, text=b"unrelated private content"),
            b"\x11channel secret", dm(7), dm(16)])
        session.drain(backlog=True)
        session.client.messages.append(dm(text=b"Theme D"))
        session.drain(backlog=False)
        messages = [e for e in events.events if e["event"] == "dm"]
        self.assertEqual([m["text"] for m in messages], ["Theme A", "Theme D"])
        self.assertEqual([m["backlog"] for m in messages], [True, False])
        self.assertNotIn("unrelated", str(events.events))
        self.assertNotIn("channel secret", str(events.events))

    def test_send_advertises_once_directs_and_matches_only_own_ack(self):
        events = Capture()
        session = chat.Chat(PEER, events)
        session.client = FakeClient(record())
        session.send(["Theme A?"], True, 0, 1)
        codes = [data[0] for data in session.client.commands]
        self.assertEqual(codes.count(7), 1)
        self.assertEqual(codes.count(2), 1)
        self.assertLess(codes.index(7), codes.index(2))
        self.assertEqual(session.client.current[35], 0)
        acks = [e for e in events.events if e["event"] == "ack"]
        self.assertEqual(len(acks), 1)
        self.assertEqual(acks[0]["rtt_ms"], 900)
        self.assertEqual(acks[0]["tag"], b"abcd".hex())
        self.assertEqual([e["outcome"] for e in events.events if e["event"] == "delivery"],
                         ["acknowledged"])

    def test_unknown_delivery_and_late_ack(self):
        events = Capture()
        session = chat.Chat(PEER, events)
        session.client = FakeClient(record())
        with patch.object(session, "wait"):
            session.send(["Theme B?"], False, 0, 1)
        self.assertEqual([e["outcome"] for e in events.events if e["event"] == "delivery"],
                         ["unknown"])
        session.push(session.client.pushes[0])
        self.assertTrue(events.events[-1]["late"])

    def test_partial_header_and_body_survive_idle_timeout(self):
        client = chat.NativeChat("unused", 1)
        client.socket, peer = socket.socketpair()
        self.addCleanup(client.socket.close)
        self.addCleanup(peer.close)
        peer.sendall(b">")
        start = time.monotonic()
        with self.assertRaises(TimeoutError):
            client.frame(start + 0.04)
        self.assertGreaterEqual(time.monotonic() - start, 0.03)
        peer.sendall(b"\x03\0\x83")
        with self.assertRaises(TimeoutError):
            client.frame(time.monotonic() + 0.04)
        peer.sendall(b"xy>\x01\0\x83")
        self.assertEqual(client.frame(time.monotonic() + 1), b"\x83xy")
        self.assertEqual(client.frame(time.monotonic() + 1), b"\x83")

    def test_command_uses_one_deadline_and_demultiplexes_push(self):
        client = chat.NativeChat("unused", 1)
        client.socket, peer = socket.socketpair()
        self.addCleanup(client.socket.close)
        self.addCleanup(peer.close)
        def respond():
            peer.recv(512)
            peer.sendall(b">\x01\0\x83>\x05\0\x09abcd")
        worker = threading.Thread(target=respond)
        worker.start()
        self.assertEqual(client.command(b"\x05"), b"\x09abcd")
        worker.join(timeout=1)
        self.assertFalse(worker.is_alive())
        self.assertEqual(client.pushes, [b"\x83"])

    def test_reconnect_never_replays_outbound_batch(self):
        first, second = FakeClient(record()), FakeClient(record())
        events = Capture()
        args = SimpleNamespace(peer=PEER, action="send", listen=True, host="unused", port=5000,
                               expected_sender=SENDER, message=["A", "B"], advertise=True,
                               spacing=0, ack_wait=1, heartbeat=30)
        attempted = []
        clients = iter((first, second))
        def fail_send(session, *unused):
            attempted.append(session.client)
            raise ConnectionError("TCP lost after possible submission")
        with patch.object(chat.Chat, "send", fail_send), \
                patch.object(chat.Chat, "listen", side_effect=KeyboardInterrupt), \
                self.assertRaises(KeyboardInterrupt):
            chat.run(args, events, factory=lambda *_: next(clients), sleep=lambda _: None)
        self.assertEqual(attempted, [first])


if __name__ == "__main__":
    unittest.main()
