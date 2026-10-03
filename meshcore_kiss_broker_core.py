#!/usr/bin/env python3
"""Frame-aware shared MeshCore KISS broker."""

from __future__ import annotations

import hashlib
import hmac
import os
import queue
import secrets
import socket
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import BinaryIO

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from nacl import bindings
from nacl.exceptions import BadSignatureError
from nacl.signing import SigningKey, VerifyKey

from meshcore_kiss_monitor import KissDecoder, kiss_frame

KISS_DATA = 0x00
KISS_HARDWARE = 0x06
HW_ERROR = 0xF1
HW_TX_DONE = 0xF8
HW_RX_META = 0xF9
HW_OK = 0xF0
ERR_INVALID_LENGTH = 0x01
ERR_INVALID_PARAM = 0x02
ERR_UNKNOWN_CMD = 0x05
ERR_MAC_FAILED = 0x04
ERR_TX_BUSY = 0x07

LOCAL_HARDWARE_COMMANDS = frozenset(range(0x01, 0x09))
ROLE_PRIORITIES = {"router": 0, "room": 1, "client": 2, "observer": 3}


def _hardware(subcommand: int, payload: bytes = b"") -> bytes:
    return kiss_frame(KISS_HARDWARE, bytes((subcommand,)) + payload)


@dataclass(eq=False)
class ClientSession:
    sock: socket.socket
    role: str
    signal_report: bool = True
    decoder: KissDecoder = field(default_factory=KissDecoder)
    send_lock: threading.Lock = field(default_factory=threading.Lock)

    def send(self, frame: bytes) -> bool:
        try:
            with self.send_lock:
                self.sock.sendall(frame)
            return True
        except OSError:
            return False


@dataclass(order=True)
class Request:
    priority: int
    sequence: int
    session: ClientSession | None = field(compare=False)
    frame: bytes = field(compare=False)
    expected: int | None = field(compare=False)
    timeout: float = field(compare=False)
    expired: bool = field(default=False, compare=False)


class RoleIdentity:
    """Persistent KISS crypto identity for one logical role."""

    def __init__(self, role: str, state_dir: Path):
        state_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
        path = state_dir / f"{role}.seed"
        try:
            seed = path.read_bytes()
            if len(seed) != 32:
                raise ValueError(f"invalid identity seed in {path}")
        except FileNotFoundError:
            seed = secrets.token_bytes(32)
            temporary = path.with_suffix(".seed.new")
            fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "wb") as output:
                output.write(seed)
            os.replace(temporary, path)
        self.signing_key = SigningKey(seed)

    @property
    def public_key(self) -> bytes:
        return bytes(self.signing_key.verify_key)

    def handle(self, subcommand: int, payload: bytes) -> bytes:
        try:
            if subcommand == 0x01:
                if payload:
                    return _hardware(HW_ERROR, bytes((ERR_INVALID_LENGTH,)))
                return _hardware(0x81, self.public_key)
            if subcommand == 0x02:
                if len(payload) != 1 or not 1 <= payload[0] <= 64:
                    return _hardware(HW_ERROR, bytes((ERR_INVALID_PARAM,)))
                return _hardware(0x82, secrets.token_bytes(payload[0]))
            if subcommand == 0x03:
                if len(payload) < 96:
                    return _hardware(HW_ERROR, bytes((ERR_INVALID_LENGTH,)))
                valid = True
                try:
                    VerifyKey(payload[:32]).verify(payload[96:], payload[32:96])
                except (BadSignatureError, ValueError):
                    valid = False
                return _hardware(0x83, bytes((valid,)))
            if subcommand == 0x04:
                return _hardware(0x84, self.signing_key.sign(payload).signature)
            if subcommand == 0x05:
                if len(payload) < 32:
                    return _hardware(HW_ERROR, bytes((ERR_INVALID_LENGTH,)))
                key, plaintext = payload[:32], payload[32:]
                ciphertext = self._crypt(key, plaintext, True)
                mac = hmac.digest(key, ciphertext, "sha256")[:2]
                return _hardware(0x85, mac + ciphertext)
            if subcommand == 0x06:
                if len(payload) < 34 or (len(payload) - 34) % 16:
                    return _hardware(HW_ERROR, bytes((ERR_INVALID_LENGTH,)))
                key, supplied_mac, ciphertext = payload[:32], payload[32:34], payload[34:]
                expected_mac = hmac.digest(key, ciphertext, "sha256")[:2]
                if not hmac.compare_digest(supplied_mac, expected_mac):
                    return _hardware(HW_ERROR, bytes((ERR_MAC_FAILED,)))
                return _hardware(0x86, self._crypt(key, ciphertext, False))
            if subcommand == 0x07:
                if len(payload) != 32:
                    return _hardware(HW_ERROR, bytes((ERR_INVALID_LENGTH,)))
                curve_secret = bindings.crypto_sign_ed25519_sk_to_curve25519(
                    bytes(self.signing_key) + self.public_key
                )
                curve_public = bindings.crypto_sign_ed25519_pk_to_curve25519(payload)
                return _hardware(0x87, bindings.crypto_scalarmult(curve_secret, curve_public))
            if subcommand == 0x08:
                return _hardware(0x88, hashlib.sha256(payload).digest())
        except (ValueError, RuntimeError):
            return _hardware(HW_ERROR, bytes((ERR_INVALID_PARAM,)))
        return _hardware(HW_ERROR, bytes((ERR_UNKNOWN_CMD,)))

    @staticmethod
    def _crypt(key: bytes, data: bytes, encrypt: bool) -> bytes:
        if encrypt and len(data) % 16:
            data += bytes(16 - len(data) % 16)
        operation = Cipher(algorithms.AES(key[:16]), modes.ECB())
        context = operation.encryptor() if encrypt else operation.decryptor()
        return context.update(data) + context.finalize()


class SharedKissBroker:
    """Expose role-specific TCP KISS endpoints over one physical modem."""

    def __init__(
        self,
        upstream: BinaryIO | socket.socket,
        listeners: dict[str, tuple[str, int]] | None = None,
        *,
        state_dir: Path | None = None,
        request_timeout: float = 5.0,
        tx_timeout: float = 30.0,
        listen_host: str | None = None,
        listen_port: int | None = None,
    ):
        if listeners is None:
            listeners = {"client": (listen_host or "0.0.0.0", 8001 if listen_port is None else listen_port)}
        self.upstream = upstream
        self.request_timeout = request_timeout
        self.tx_timeout = tx_timeout
        self._stop = threading.Event()
        self._requests: queue.PriorityQueue[Request] = queue.PriorityQueue()
        self._sequence = 0
        self._sequence_lock = threading.Lock()
        self._pending_lock = threading.Lock()
        self._pending: Request | None = None
        self._pending_done = threading.Event()
        self._clients: set[ClientSession] = set()
        self._clients_lock = threading.Lock()
        self._listeners: dict[str, socket.socket] = {}
        self._threads: list[threading.Thread] = []
        root = state_dir or Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "meshcore-kiss"
        self._identities = {
            role: RoleIdentity(role, root / "identities") for role in listeners
        }
        for role, (host, port) in listeners.items():
            listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((host, port))
            listener.listen(8)
            listener.settimeout(0.25)
            self._listeners[role] = listener

    @property
    def port(self) -> int:
        return self.port_for("client")

    def port_for(self, role: str) -> int:
        return self._listeners[role].getsockname()[1]

    def endpoints(self) -> dict[str, tuple[str, int]]:
        return {role: listener.getsockname() for role, listener in self._listeners.items()}

    def start(self) -> None:
        for role, listener in self._listeners.items():
            thread = threading.Thread(target=self._accept_loop, args=(role, listener), daemon=True)
            thread.start()
            self._threads.append(thread)
        for target in (self._request_loop, self._upstream_loop):
            thread = threading.Thread(target=target, daemon=True)
            thread.start()
            self._threads.append(thread)
        if not hasattr(self.upstream, "take_connected_event"):
            self._enqueue(None, bytes((KISS_HARDWARE, 0x19, 0x01)), expected=0x9A)

    def stop(self) -> None:
        self._stop.set()
        self._pending_done.set()
        for listener in self._listeners.values():
            try:
                listener.close()
            except OSError:
                pass
        with self._clients_lock:
            clients = list(self._clients)
            self._clients.clear()
        for session in clients:
            try:
                session.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            session.sock.close()

    def _accept_loop(self, role: str, listener: socket.socket) -> None:
        while not self._stop.is_set():
            try:
                client, _ = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            client.settimeout(0.25)
            session = ClientSession(client, role)
            with self._clients_lock:
                self._clients.add(session)
            threading.Thread(target=self._client_loop, args=(session,), daemon=True).start()

    def _client_loop(self, session: ClientSession) -> None:
        try:
            while not self._stop.is_set():
                try:
                    data = session.sock.recv(4096)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not data:
                    break
                for frame in session.decoder.feed(data):
                    self._handle_client_frame(session, frame)
        finally:
            with self._clients_lock:
                self._clients.discard(session)
            try:
                session.sock.close()
            except OSError:
                pass

    def _handle_client_frame(self, session: ClientSession, frame: bytes) -> None:
        if not frame:
            return
        command = frame[0] & 0x0F
        if frame[0] >> 4 or (session.role == "observer" and command != KISS_HARDWARE):
            session.send(_hardware(HW_ERROR, bytes((ERR_INVALID_PARAM,))))
            return
        if command == KISS_HARDWARE and len(frame) >= 2:
            subcommand = frame[1]
            if not 0x01 <= subcommand <= 0x1A:
                # Only the known legacy contract can share this upstream session.
                session.send(_hardware(HW_ERROR, bytes((ERR_UNKNOWN_CMD,))))
                return
            if session.role == "observer" and subcommand in (0x09, 0x0A, 0x18):
                session.send(_hardware(HW_ERROR, bytes((ERR_INVALID_PARAM,))))
                return
            if subcommand in LOCAL_HARDWARE_COMMANDS:
                session.send(self._identities[session.role].handle(subcommand, frame[2:]))
                return
            if subcommand == 0x19:
                if len(frame) != 3:
                    session.send(_hardware(HW_ERROR, bytes((ERR_INVALID_LENGTH,))))
                else:
                    session.signal_report = frame[2] != 0
                    session.send(_hardware(0x9A, bytes((session.signal_report,))))
                return
            if subcommand == 0x1A:
                session.send(_hardware(0x9A, bytes((session.signal_report,))))
                return
            expected = HW_OK if subcommand in (0x09, 0x0A, 0x18) else subcommand | 0x80
            self._enqueue(session, frame, expected=expected)
            return
        self._enqueue(session, frame, expected=HW_TX_DONE if command == KISS_DATA else None)

    def _enqueue(self, session: ClientSession | None, frame: bytes, expected: int | None) -> None:
        role = session.role if session else "router"
        with self._sequence_lock:
            sequence = self._sequence
            self._sequence += 1
        timeout = self.tx_timeout if expected == HW_TX_DONE else self.request_timeout
        self._requests.put(Request(ROLE_PRIORITIES.get(role, 2), sequence, session, frame, expected, timeout))

    def _request_loop(self) -> None:
        while not self._stop.is_set():
            try:
                request = self._requests.get(timeout=0.25)
            except queue.Empty:
                continue
            self._pending_done.clear()
            with self._pending_lock:
                self._pending = request if request.expected is not None else None
            try:
                self._upstream_write(kiss_frame(request.frame[0], request.frame[1:]))
            except OSError:
                with self._pending_lock:
                    if self._pending is request:
                        self._pending = None
                if request.session:
                    request.session.send(_hardware(HW_ERROR, bytes((ERR_TX_BUSY,))))
                continue
            if request.expected is not None and not self._pending_done.wait(request.timeout):
                request.expired = True
                if request.session:
                    request.session.send(_hardware(HW_ERROR, bytes((ERR_TX_BUSY,))))
                while not self._stop.is_set() and not self._pending_done.wait(0.25):
                    pass

    def _upstream_loop(self) -> None:
        decoder = KissDecoder()
        while not self._stop.is_set():
            try:
                data = self._upstream_read(512)
            except (OSError, TimeoutError):
                time.sleep(0.1)
                continue
            if hasattr(self.upstream, "take_connected_event") and self.upstream.take_connected_event():
                self._enqueue(None, bytes((KISS_HARDWARE, 0x19, 0x01)), expected=0x9A)
            if not data:
                time.sleep(0.01)
                continue
            for frame in decoder.feed(data):
                self._route_upstream_frame(frame)

    def _route_upstream_frame(self, frame: bytes) -> None:
        if not frame:
            return
        wire = kiss_frame(frame[0], frame[1:])
        command = frame[0] & 0x0F
        if command == KISS_DATA:
            self._broadcast(wire)
            return
        if command != KISS_HARDWARE or len(frame) < 2:
            self._broadcast(wire)
            return
        subcommand = frame[1]
        if subcommand == HW_RX_META:
            self._broadcast(wire, signal_only=True)
            return
        with self._pending_lock:
            pending = self._pending
            matches = pending is not None and subcommand in (pending.expected, HW_ERROR)
            if matches:
                self._pending = None
        if matches:
            if pending.session and not pending.expired:
                pending.session.send(wire)
            if (
                subcommand == HW_TX_DONE
                and len(frame) >= 3
                and frame[2] != 0
                and (pending.frame[0] & 0x0F) == KISS_DATA
            ):
                self._broadcast(
                    kiss_frame(pending.frame[0], pending.frame[1:]),
                    exclude=pending.session,
                )
                self._broadcast(
                    _hardware(HW_RX_META, b"\x80\x7f"),
                    signal_only=True,
                    exclude=pending.session,
                )
            self._pending_done.set()
        # Responses have no transaction ID. Unmatched replies are stale and
        # must never be attributed or broadcast to another logical client.

    def _broadcast(
        self,
        frame: bytes,
        *,
        signal_only: bool = False,
        exclude: ClientSession | None = None,
    ) -> None:
        with self._clients_lock:
            clients = list(self._clients)
        dead = []
        for session in clients:
            if session is exclude:
                continue
            if signal_only and not session.signal_report:
                continue
            if not session.send(frame):
                dead.append(session)
        if dead:
            with self._clients_lock:
                for session in dead:
                    self._clients.discard(session)

    def _upstream_read(self, size: int) -> bytes:
        if hasattr(self.upstream, "read"):
            return self.upstream.read(size)
        return self.upstream.recv(size)

    def _upstream_write(self, data: bytes) -> None:
        if hasattr(self.upstream, "write"):
            written = self.upstream.write(data)
            if written is not None and written != len(data):
                raise OSError("short write to upstream modem")
        else:
            self.upstream.sendall(data)
