#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Send short direct DMs and listen to one verified contact over native TCP."""
import argparse
from collections import deque
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import socket
import stat
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.companion import Companion, sent_reply


class NativeChat(Companion):
    """Reuse the native handshake, retaining fragmented frames across idle deadlines."""

    def __init__(self, host, port):
        super().__init__(host, port)
        self.buffer = bytearray()
        self.info = b""

    def frame(self, deadline):
        while True:
            if len(self.buffer) >= 3:
                size = struct.unpack_from("<H", self.buffer, 1)[0]
                if self.buffer[0] != ord(">") or not 0 < size <= 512:
                    raise ValueError("Malformed native TCP frame header")
                if len(self.buffer) >= size + 3:
                    result = bytes(self.buffer[3:size + 3])
                    del self.buffer[:size + 3]
                    return result
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Native TCP receive deadline exceeded")
            self.socket.settimeout(remaining)
            part = self.socket.recv(512)
            if not part:
                raise ConnectionError("Native TCP companion disconnected")
            self.buffer.extend(part)

    def response(self, deadline):
        for _ in range(128):
            frame = self.frame(deadline)
            if frame[0] < 128:
                return frame
            if len(self.pushes) >= 128:
                raise ValueError("Native TCP pending push limit exceeded")
            self.pushes.append(frame)
        raise ValueError("Native TCP response obscured by too many pushes")

    def command(self, data, allow_error=False):
        deadline = time.monotonic() + 5
        self.socket.settimeout(5)
        self.socket.sendall(b"<" + struct.pack("<H", len(data)) + data)
        result = self.response(deadline)
        if result[0] == 1 and not allow_error:
            code = result[1] if len(result) == 2 else "malformed"
            raise ValueError(f"Native command {data[0]} rejected (error {code})")
        if data[0] == 1:
            self.info = result
        return result


def contact(client, peer):
    record = client.command(b"\x1e" + peer, allow_error=True)
    if record == b"\x01\x02":
        return None
    if len(record) != 148 or record[0] != 3 or record[1:33] != peer:
        raise ValueError("Contact lookup did not return the requested full public key")
    return record


def direct_contact(client, peer):
    current = contact(client, peer)
    if current is not None and current[35] == 0:
        return False
    update = bytearray(current if current is not None else bytes(148))
    update[0], update[35] = 9, 0
    if current is None:
        update[1:33], update[33] = peer, 1
        name = ("peer-" + peer[:6].hex()).encode("ascii")
        update[100:132] = name.ljust(32, b"\0")
        struct.pack_into("<I", update, 144, int(time.time()))
    # Last-modified is server-owned and may advance when the route changes.
    if client.command(bytes(update)) != b"\0":
        raise ValueError("Zero-hop contact update was not accepted")
    actual = contact(client, peer)
    if actual is None or actual[1:144] != update[1:144]:
        raise ValueError("Zero-hop contact readback did not preserve the contact data")
    return True


def verify_peer(client, peer):
    if contact(client, peer) is None:
        raise ValueError("Requested full public key is absent from the contact table")
    start = client.command(b"\x04")
    if len(start) != 5 or start[0] != 2:
        raise ValueError("Native contact list did not start")
    expected = struct.unpack_from("<I", start, 1)[0]
    if expected > 4096:
        raise ValueError("Native contact list exceeds 4096 entries")
    matches = []
    deadline = time.monotonic() + 10
    for _ in range(expected + 1):
        record = client.response(deadline)
        # The count includes contacts skipped by the firmware's lastmod filter.
        if len(record) == 5 and record[0] == 4:
            if matches != [peer]:
                raise ValueError("Contact prefix is missing, ambiguous, or not the requested full key")
            return
        if len(record) != 148 or record[0] != 3:
            raise ValueError("Malformed native contact list entry")
        if record[1:7] == peer[:6]:
            matches.append(record[1:33])
    raise ValueError("Native contact list did not end within its reported count")


def decode_dm(frame, peer):
    # Native queue replies: 7 has no metadata; 16 adds SNR and two reserved bytes.
    if frame[0] not in (7, 16):
        if frame[0] in (8, 17, 27):
            return None
        raise ValueError(f"Unexpected native queue reply code {frame[0]}")
    offset = 4 if frame[0] == 16 else 1
    if len(frame) < offset + 6:
        raise ValueError("Truncated native DM sender prefix")
    if frame[offset:offset + 6] != peer[:6]:
        return None
    if len(frame) < offset + 12:
        raise ValueError("Truncated requested contact's native DM")
    path, kind = frame[offset + 6:offset + 8]
    if kind not in (0, 1, 2):
        raise ValueError("Unsupported native DM text type")
    timestamp = struct.unpack_from("<I", frame, offset + 8)[0]
    text_offset = offset + 12
    extra = {}
    if kind == 2:
        if len(frame) < text_offset + 4:
            raise ValueError("Truncated signed-plain DM author prefix")
        extra["signed_sender_prefix"] = frame[text_offset:text_offset + 4].hex()
        text_offset += 4
    try:
        text = frame[text_offset:].rstrip(b"\0").decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError("Requested contact's DM is not valid UTF-8") from error
    return {
        "sender": peer.hex(), "sender_prefix": peer[:6].hex(),
        "sender_timestamp": timestamp,
        "sender_time": datetime.fromtimestamp(timestamp, timezone.utc).isoformat(),
        "text": text, "text_type": kind, "frame_code": frame[0],
        "authenticated_by": "native_companion", "full_key_verified": True,
        "direct": path == 255, "path_encoded": path,
        "snr_db": struct.unpack("b", frame[1:2])[0] / 4 if offset == 4 else None,
        **extra,
    }


def log_path(path):
    path = Path(path).absolute()
    if not path.resolve().is_relative_to(ROOT / ".tmp"):
        raise ValueError("Chat logs must be inside the repository .tmp directory")
    return path


def wait_reply(path, peer, after_id=None, timeout=43200, sleep=time.sleep):
    """Return one verified DM from the existing append-only log, without radio I/O."""
    path = log_path(path)
    deadline = time.monotonic() + timeout
    found = after_id is None
    seen = set()
    pending = bytearray()
    line_number = 0
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as source:
        original = os.fstat(source.fileno())
        if not stat.S_ISREG(original.st_mode) or original.st_mode & 0o077:
            raise ValueError("wait-reply requires an existing private regular JSONL file (mode 0600)")
        while True:
            current = os.stat(path, follow_symlinks=False)
            if ((current.st_dev, current.st_ino) != (original.st_dev, original.st_ino) or
                    current.st_size < source.tell()):
                raise ValueError("Chat log was replaced or truncated while waiting")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Timed out waiting for the next authenticated DM")
            part = source.readline(65537)
            if not part:
                if not found:
                    raise ValueError("--after-id was not found among this peer's authenticated DMs")
                sleep(min(0.25, remaining))
                continue
            pending.extend(part)
            if len(pending) > 65536:
                raise ValueError("Chat JSONL record exceeds 65536 bytes")
            if not pending.endswith(b"\n"):
                continue
            line_number += 1
            try:
                entry = json.loads(pending)
            except (ValueError, UnicodeDecodeError) as error:
                raise ValueError(f"Invalid chat JSONL at line {line_number}") from error
            pending.clear()
            if not isinstance(entry, dict):
                raise ValueError(f"Expected a chat JSON object at line {line_number}")
            if (entry.get("event") != "dm" or entry.get("sender") != peer.hex() or
                    entry.get("authenticated_by") != "native_companion" or
                    entry.get("full_key_verified") is not True):
                continue
            identifier = entry.get("message_id")
            if (not isinstance(identifier, str) or not re.fullmatch("[0-9a-f]{64}", identifier) or
                    not isinstance(entry.get("text"), str)):
                raise ValueError(f"Invalid authenticated DM metadata at line {line_number}")
            if identifier == after_id:
                found = True
            elif found and identifier not in seen:
                return entry
            seen.add(identifier)


class Events:
    def __init__(self, path=None):
        self.file = None
        if path:
            path = log_path(path)
            path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT | os.O_NOFOLLOW, 0o600)
            self.file = os.fdopen(fd, "a")
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                os.fchmod(fd, 0o600)
            except OSError:
                self.file.close()
                raise

    def emit(self, event, **fields):
        value = {"event": event, "observed_at": datetime.now(timezone.utc).isoformat(),
                 "pid": os.getpid(), **fields}
        line = json.dumps(value, ensure_ascii=True)
        if self.file:
            self.file.write(line + "\n")
            self.file.flush()
            os.fsync(self.file.fileno())
        print(line, flush=True)

    def close(self):
        if self.file:
            self.file.close()


class Chat:
    def __init__(self, peer, events):
        self.peer, self.events = peer, events
        self.client = None
        self.pending = {}
        self.seen = set()
        self.order = deque()
        self.needs_pull = False

    def emit(self, event, **fields):
        self.events.emit(event, peer=self.peer.hex(), **fields)

    def push(self, frame):
        if frame[0] == 0x83:
            self.needs_pull = True
        elif frame[0] == 0x82:
            if len(frame) != 9:
                raise ValueError("Malformed native send-confirmed push")
            tag = frame[1:5].hex()
            entry = self.pending.get(tag)
            if entry is not None and not entry["acked"]:
                entry["acked"] = True
                self.emit("ack", sequence=entry["sequence"], tag=tag,
                          rtt_ms=struct.unpack_from("<I", frame, 5)[0],
                          late=entry["timed_out"], outcome="acknowledged")
        elif frame[0] < 128:
            raise ValueError("Unexpected native command reply while awaiting pushes")

    def drain(self, backlog):
        self.needs_pull = False
        for _ in range(4096):
            frame = self.client.command(b"\x0a")
            if frame == b"\x0a":
                self.emit("queue_drained", backlog=backlog)
                return
            message = decode_dm(frame, self.peer)
            if message is None:
                continue
            # Recheck the full key before attributing the short on-wire prefix.
            verify_peer(self.client, self.peer)
            identity = json.dumps([message[k] for k in
                                   ("sender", "sender_timestamp", "text_type", "text")] +
                                  [message.get("signed_sender_prefix")]).encode()
            digest = hashlib.sha256(identity).hexdigest()
            if digest in self.seen:
                continue
            if len(self.order) == 2048:
                self.seen.remove(self.order.popleft())
            self.order.append(digest)
            self.seen.add(digest)
            self.emit("dm", message_id=digest, backlog=backlog, **message)
        raise ValueError("Native message queue did not empty after 4096 pulls")

    def wait(self, deadline, ack_tag=None):
        while True:
            while self.client.pushes:
                self.push(self.client.pushes.pop(0))
            if self.needs_pull:
                self.drain(backlog=False)
                continue
            if ack_tag and self.pending[ack_tag]["acked"]:
                return
            if time.monotonic() >= deadline:
                return
            try:
                self.push(self.client.frame(deadline))
            except TimeoutError:
                return

    def send(self, messages, advertise, spacing, ack_wait):
        if advertise:
            self.emit("advert_requested", route="flood", outcome="unknown")
            if self.client.command(b"\x07\x01") != b"\0":
                raise ValueError("Native flood advert was not accepted")
            self.emit("advert", route="flood", outcome="admitted", rf_reception="unknown")
            self.wait(time.monotonic() + spacing)
        for sequence, text in enumerate(messages, 1):
            changed = direct_contact(self.client, self.peer)
            if changed:
                self.emit("contact", route="direct", hops=0, updated=True)
            verify_peer(self.client, self.peer)
            timestamp = int(time.time())
            data = b"\x02\0\0" + struct.pack("<I", timestamp) + self.peer[:6] + text.encode("ascii")
            self.emit("send_requested", sequence=sequence, text=text, bytes=len(text),
                      sender_timestamp=timestamp, route="direct", hops=0, outcome="unknown")
            sent = sent_reply(self.client.command(data))
            self.emit("sent", sequence=sequence, text=text, outcome="admitted", **sent)
            if sent["flood"]:
                raise ValueError("Companion admitted a flood DM despite the zero-hop route")
            tag = sent["tag"]
            self.pending[tag] = {"sequence": sequence, "acked": False, "timed_out": False}
            self.wait(time.monotonic() + ack_wait, tag)
            entry = self.pending[tag]
            entry["timed_out"] = not entry["acked"]
            self.emit("delivery", sequence=sequence, tag=tag,
                      outcome="acknowledged" if entry["acked"] else "unknown",
                      retry=False)
            if sequence != len(messages):
                self.wait(time.monotonic() + spacing)
        self.emit("outbound_complete", messages=len(messages),
                  acknowledged=sum(entry["acked"] for entry in self.pending.values()))

    def listen(self, heartbeat):
        self.emit("listening", reconnect="receive_only", heartbeat_seconds=heartbeat)
        while True:
            self.wait(time.monotonic() + heartbeat)
            clock = self.client.command(b"\x05")
            if len(clock) != 5 or clock[0] != 9:
                raise ValueError("Companion heartbeat did not return its clock")
            self.emit("heartbeat", device_timestamp=struct.unpack_from("<I", clock, 1)[0])
            # A slow fallback pull also covers a missed message-waiting notification.
            self.drain(backlog=False)


def public_key(value):
    if not re.fullmatch(r"[0-9a-fA-F]{64}", value):
        raise argparse.ArgumentTypeError("Use a full 64-hex-character public key")
    return bytes.fromhex(value)


def message_text(value):
    if not 1 <= len(value) <= 120 or any(not 32 <= ord(char) <= 126 for char in value):
        raise argparse.ArgumentTypeError("Messages must contain 1..120 printable ASCII bytes")
    return value


def positive_seconds(value):
    result = float(value)
    if not 1 <= result <= 3600:
        raise argparse.ArgumentTypeError("Seconds must be between 1 and 3600")
    return result


def positive_timeout(value):
    result = float(value)
    if not math.isfinite(result) or result <= 0:
        raise argparse.ArgumentTypeError("Timeout must be a finite positive number of seconds")
    return result


def run(args, events, factory=NativeChat, sleep=time.sleep):
    chat = Chat(args.peer, events)
    outbound = args.action == "send"
    keep_listening = args.action == "listen" or args.listen
    reconnect_delay = 2
    while True:
        try:
            with factory(args.host, args.port) as client:
                chat.client = client
                if client.public_key != args.expected_sender.hex():
                    raise ValueError("TCP companion public key differs from --expected-sender")
                if len(client.info) < 58:
                    raise ValueError("Native self-info is missing the current PHY")
                freq, bandwidth = struct.unpack_from("<II", client.info, 48)
                chat.emit("connected", sender=client.public_key, host=args.host, port=args.port,
                          protocol=13, frequency_khz=freq, bandwidth_hz=bandwidth,
                          sf=client.info[56], cr=client.info[57], receive_only=not outbound)
                if outbound:
                    changed = direct_contact(client, args.peer)
                    chat.emit("contact", route="direct", hops=0, updated=changed)
                verify_peer(client, args.peer)
                chat.drain(backlog=True)
                reconnect_delay = 2
                if outbound:
                    # This batch is never replayed, even if admission or ACK is lost.
                    outbound = False
                    chat.send(args.message, args.advertise, args.spacing, args.ack_wait)
                if not keep_listening:
                    return 0
                chat.listen(args.heartbeat)
        except OSError as error:
            outbound = False
            chat.emit("error", error=str(error), phase="connection",
                      outbound_replayed=False, delivery_if_interrupted="unknown",
                      reconnect=keep_listening, retry_in_seconds=reconnect_delay)
            if not keep_listening:
                return 1
            sleep(reconnect_delay)
            reconnect_delay = min(reconnect_delay * 2, 30)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("send", "listen", "wait-reply"))
    parser.add_argument("--host")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--peer", type=public_key, required=True)
    parser.add_argument("--expected-sender", type=public_key)
    parser.add_argument("--message", type=message_text, action="append", default=[])
    parser.add_argument("--advertise", action="store_true", help="Send one normal native flood advert")
    parser.add_argument("--listen", action="store_true", help="Keep receiving after the send batch")
    parser.add_argument("--spacing", type=positive_seconds, default=3)
    parser.add_argument("--ack-wait", type=positive_seconds, default=30)
    parser.add_argument("--heartbeat", type=positive_seconds, default=30)
    parser.add_argument("--log", help="Private JSONL under repository .tmp; read-only for wait-reply")
    parser.add_argument("--after-id", help="Last processed DM message_id, already present in --log")
    parser.add_argument("--timeout", type=positive_timeout, default=43200,
                        help="wait-reply deadline in seconds (default: 43200 / 12 hours)")
    args = parser.parse_args()
    if args.action == "wait-reply":
        if not args.log:
            parser.error("wait-reply requires an existing --log")
        if args.host or args.expected_sender or args.message or args.advertise or args.listen:
            parser.error("wait-reply does not accept radio or outbound options")
    elif not args.host or not args.expected_sender:
        parser.error("send and listen require --host and --expected-sender")
    elif args.after_id is not None or args.timeout != 43200:
        parser.error("--after-id and --timeout are only used by wait-reply")
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if args.action == "send" and not args.message:
        parser.error("send requires at least one --message")
    if args.action == "listen" and (args.message or args.advertise or args.listen):
        parser.error("listen does not accept outbound options")
    os.umask(0o077)
    events = None
    try:
        if args.action == "wait-reply":
            reply = wait_reply(args.log, args.peer, args.after_id, args.timeout)
            print(json.dumps(reply, ensure_ascii=True), flush=True)
            return 0
        events = Events(args.log)
        return run(args, events)
    except KeyboardInterrupt:
        if events:
            events.emit("stopped", reason="interrupt")
        return 130
    except (OSError, ValueError) as error:
        if events:
            events.emit("error", error=str(error), phase="fatal", outbound_replayed=False)
        else:
            print(json.dumps({"event": "error", "error": str(error)}), file=sys.stderr)
        return 1
    finally:
        if events:
            events.close()


if __name__ == "__main__":
    sys.exit(main())
