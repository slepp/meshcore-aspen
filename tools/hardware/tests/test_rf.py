# SPDX-License-Identifier: Apache-2.0
"""Check the operator sender against the native frame and cryptographic layout."""
import hashlib
import hmac
import socket
import struct
import unittest

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.hardware import rf as role_rf


class OperatorFrames(unittest.TestCase):
    def test_native_flood_encryption_and_signature(self):
        operator_seed = bytes(range(32))
        management_seed = bytes(reversed(range(32)))
        management = ed25519.Ed25519PrivateKey.from_private_bytes(management_seed)
        target = management.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw
        )
        packet = role_rf.frame(operator_seed, target, 9, 13, 5)
        self.assertEqual(len(packet), 181)
        self.assertEqual(packet[:3], bytes([0x1d, 0x80, target[0]]))
        sender = packet[3:35]
        encrypted = packet[37:]
        self.assertEqual(len(encrypted), 144)
        scalar = hashlib.sha512(management_seed).digest()[:32]
        y = int.from_bytes(sender, "little") & ((1 << 255) - 1)
        montgomery = ((1 + y) * pow(1 - y, -1, role_rf.P) % role_rf.P).to_bytes(32, "little")
        secret = x25519.X25519PrivateKey.from_private_bytes(scalar).exchange(
            x25519.X25519PublicKey.from_public_bytes(montgomery)
        )
        self.assertEqual(packet[35:37], hmac.new(secret, encrypted, hashlib.sha256).digest()[:2])
        decryptor = Cipher(algorithms.AES(secret[:16]), modes.ECB()).decryptor()
        plain = decryptor.update(encrypted) + decryptor.finalize()
        self.assertEqual(plain[:65], role_rf.DOMAIN + target + struct.pack("<QQB", 9, 13, 5))
        self.assertEqual(plain[129:], bytes(15))
        ed25519.Ed25519PrivateKey.from_private_bytes(operator_seed).public_key().verify(
            plain[65:129], plain[:65]
        )
        encoded = role_rf.kiss(packet)
        self.assertEqual(encoded[0], 0xc0)
        self.assertEqual(encoded[1], 0)
        self.assertEqual(encoded[-1], 0xc0)
        self.assertEqual(encoded[2:-1].replace(b"\xdb\xdc", b"\xc0")
                         .replace(b"\xdb\xdd", b"\xdb"), packet)

    def test_input_limits(self):
        with self.assertRaises(ValueError):
            role_rf.target_key("ab")
        for value in ("room,room", "none,room", "bot", "", "room,"):
            with self.assertRaises(ValueError):
                role_rf.role_mask(value)
        self.assertEqual(role_rf.role_mask("none"), 0)
        self.assertEqual(role_rf.role_mask("repeater,room,companion,observer"), 15)
        with self.assertRaises(ValueError):
            role_rf.frame(bytes(range(32)), bytes([2]) * 32, 0, 1, 0)
        with self.assertRaises(ValueError):
            role_rf.key_path(role_rf.ROOT / ".tmp/operator.seed")


class SignedReceipts(unittest.TestCase):
    def setUp(self):
        self.management_seed = bytes(reversed(range(32)))
        self.management = ed25519.Ed25519PrivateKey.from_private_bytes(self.management_seed)
        self.target = self.management.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw
        )
        self.request, self.context = role_rf.frame_with_context(
            bytes(range(32)), self.target, 9, 13, 5, sender_seed=bytes([42]) * 32
        )

    def receipt(self, status=1, *, domain=role_rf.RECEIPT_DOMAIN, target=None,
                sender=None, generation=9, nonce=13, signature=None, padding=None,
                path_encoding=0, path=b"", flood=False):
        target = self.target if target is None else target
        sender = self.context.sender_public if sender is None else sender
        body = domain + target + sender + struct.pack("<QQB", generation, nonce, status)
        self.assertEqual(len(body), 99)
        plaintext = body + (self.management.sign(body) if signature is None else signature)
        plaintext += bytes(13) if padding is None else padding
        self.assertEqual(len(plaintext), 176)
        scalar = hashlib.sha512(self.management_seed).digest()[:32]
        y = int.from_bytes(self.context.sender_public, "little") & ((1 << 255) - 1)
        montgomery = ((1 + y) * pow(1 - y, -1, role_rf.P) % role_rf.P).to_bytes(32, "little")
        secret = x25519.X25519PrivateKey.from_private_bytes(scalar).exchange(
            x25519.X25519PublicKey.from_public_bytes(montgomery)
        )
        self.assertEqual(secret, self.context.secret)
        encryptor = Cipher(algorithms.AES(secret[:16]), modes.ECB()).encryptor()
        ciphertext = encryptor.update(plaintext) + encryptor.finalize()
        payload = (bytes((self.context.sender_public[0], self.target[0])) +
                   hmac.new(secret, ciphertext, hashlib.sha256).digest()[:2] +
                   ciphertext)
        self.assertEqual(len(payload), 180)
        return bytes((0x05 if flood else 0x06, path_encoding)) + path + payload

    def receive(self, stream, *, seconds=0.2, close=True, context=None,
                verifier=role_rf.verify_receipt, kind="receipt"):
        sender, gateway = socket.socketpair()
        try:
            gateway.sendall(stream)
            if close:
                gateway.shutdown(socket.SHUT_WR)
            return role_rf.wait_receipt(
                sender, self.context if context is None else context,
                seconds, verifier, kind
            )
        finally:
            sender.close()
            gateway.close()

    @staticmethod
    def kiss_rx(packet):
        return role_rf.kiss(packet)

    def test_signed_encrypted_response_fixture_and_statuses(self):
        self.assertEqual(len(role_rf.RECEIPT_DOMAIN), 18)
        self.assertEqual(self.request[3:35], self.context.sender_public)
        self.assertEqual(
            self.context.sender_key.public_key().public_bytes(
                serialization.Encoding.Raw, serialization.PublicFormat.Raw
            ), self.context.sender_public
        )
        for status in (1, 2, 3):
            with self.subTest(status=status):
                packet = self.receipt(status, path_encoding=0x42, path=b"\x01\x02\x03\x04",
                                      flood=True)
                self.assertEqual(role_rf.verify_receipt(packet, self.context), status)
                self.assertEqual(self.receive(self.kiss_rx(packet)), status)
        self.assertEqual(
            role_rf.verify_receipt(
                self.receipt(path_encoding=0x81, path=b"\x01\x02\x03"), self.context
            ), 1
        )
        self.assertEqual(
            self.receive(self.kiss_rx(
                self.receipt(path_encoding=0x95, path=bytes(range(63)), flood=True)
            )), 1
        )

    def test_rejects_mac_signature_domain_correlation_and_padding(self):
        valid = self.receipt()
        corrupt_mac = bytearray(valid)
        corrupt_mac[4] ^= 0x80
        wrong_hash = bytearray(valid)
        wrong_hash[2] ^= 0x80
        for packet in (
            bytes(corrupt_mac), bytes(wrong_hash),
            self.receipt(signature=bytes(64)),
            self.receipt(domain=b"MCORE-ROLE-RCPT-V0"),
            self.receipt(target=bytes(32)),
            self.receipt(sender=bytes([23]) * 32),
            self.receipt(generation=10),
            self.receipt(nonce=14),
            self.receipt(status=0), self.receipt(status=4),
            self.receipt(padding=bytes(12) + b"\x01"),
        ):
            with self.subTest(packet=packet[:10].hex()):
                self.assertIsNone(role_rf.verify_receipt(packet, self.context))

    def test_strict_direct_path_and_payload_length(self):
        valid = self.receipt()
        for packet in (
            valid[:1] + b"\xff" + valid[2:],
            valid[:1] + b"\xc0" + valid[2:],
            valid[:1] + b"\x01" + valid[2:],
            valid + b"\x00", valid[:-1],
            b"\x1e" + valid[1:],
            b"\x06", b"\x06\x00",
        ):
            with self.subTest(packet=packet[:10].hex()):
                self.assertIsNone(role_rf.verify_receipt(packet, self.context))

    def test_transport_hints_do_not_replace_signature_authentication(self):
        valid = self.receipt()
        for route in (0, 3):
            scoped = bytes((4 | route,)) + b"\x12\x34\x56\x78" + valid[1:]
            self.assertEqual(role_rf.verify_receipt(scoped, self.context), 1)
            corrupt = scoped[:-1] + bytes((scoped[-1] ^ 1,))
            self.assertIsNone(role_rf.verify_receipt(corrupt, self.context))
        self.assertIsNone(role_rf.verify_receipt(b"\x04\x00", self.context))

    def test_tx_done_before_and_after_receipt_is_not_receipt(self):
        signed = self.kiss_rx(self.receipt(status=2))
        tx_done = b"\xc0\x06\xf8\x01\xc0"
        self.assertEqual(self.receive(tx_done + signed), 2)
        self.assertEqual(self.receive(signed + tx_done), 2)
        for terminal in (tx_done, b"\xc0\x06\xf8\x00\xc0"):
            with self.subTest(terminal=terminal):
                with self.assertRaisesRegex(ValueError, "disconnected before a signed mast receipt"):
                    self.receive(terminal)

    def test_invalid_traffic_and_malformed_kiss_cannot_pass_as_receipt(self):
        good = self.kiss_rx(self.receipt())
        raw = self.kiss_rx(self.receipt(status=2))
        bad_escape = raw[:5] + b"\xdb\xaa" + raw[5:]
        incomplete = raw[:-1]
        with self.assertRaisesRegex(ValueError, "signed mast receipt"):
            self.receive(bad_escape)
        with self.assertRaisesRegex(ValueError, "signed mast receipt"):
            self.receive(incomplete)
        noise = b"\xc0\x00\xaa\xc0" * 2000
        oversized = b"\xc0\x00" + b"\xaa" * 512 + b"\xc0"
        self.assertEqual(self.receive(noise + bad_escape + oversized + good), 1)
        with self.assertRaisesRegex(ValueError, "signed mast receipt"):
            self.receive(self.kiss_rx(self.receipt(signature=bytes(64))))

    def test_deadline_and_total_stream_bound(self):
        with self.assertRaisesRegex(ValueError, "before deadline"):
            self.receive(b"\xc0\x06\xf8\x01\xc0", seconds=0.01, close=False)
        original = role_rf.MAX_RX_BYTES
        try:
            role_rf.MAX_RX_BYTES = 200
            with self.assertRaisesRegex(ValueError, "RX stream limit"):
                self.receive(b"\xc0\x00\xaa\xc0" * 60)
        finally:
            role_rf.MAX_RX_BYTES = original


class SignedStatus(unittest.TestCase):
    receive = SignedReceipts.receive
    kiss_rx = staticmethod(role_rf.kiss)

    def setUp(self):
        SignedReceipts.setUp(self)
        self.query, self.query_context = role_rf.query_with_context(
            bytes(range(32)), self.target, 0x12345678,
            sender_seed=bytes([42]) * 32
        )

    def status_packet(self, *, generation=1 << 62, saved=5, applied=0,
                      flags=1, nonce=None, signature=None, padding=None,
                      path_encoding=0, path=b"", flood=True):
        context = self.query_context
        body = (role_rf.STATUS_DOMAIN + self.target + context.sender_public +
                struct.pack("<QQBBB", context.nonce if nonce is None else nonce,
                            generation, saved, applied, flags))
        self.assertEqual(len(body), 101)
        plaintext = body + (self.management.sign(body) if signature is None else signature)
        plaintext += bytes(11) if padding is None else padding
        encryptor = Cipher(algorithms.AES(context.secret[:16]), modes.ECB()).encryptor()
        ciphertext = encryptor.update(plaintext) + encryptor.finalize()
        payload = (bytes((context.sender_public[0], self.target[0])) +
                   hmac.new(context.secret, ciphertext, hashlib.sha256).digest()[:2] +
                   ciphertext)
        return bytes((0x05 if flood else 0x06, path_encoding)) + path + payload

    def test_query_envelope_and_status_wire(self):
        self.assertEqual(len(self.query), 165)
        self.assertEqual(self.query[:3], bytes((0x1d, 0x80, self.target[0])))
        self.assertEqual(self.query[3:35], self.query_context.sender_public)
        self.assertEqual(len(role_rf.QUERY_DOMAIN), 17)
        self.assertEqual(len(role_rf.STATUS_DOMAIN), 18)
        scalar = hashlib.sha512(self.management_seed).digest()[:32]
        y = int.from_bytes(self.query_context.sender_public, "little") & ((1 << 255) - 1)
        montgomery = ((1 + y) * pow(1 - y, -1, role_rf.P) % role_rf.P).to_bytes(32, "little")
        secret = x25519.X25519PrivateKey.from_private_bytes(scalar).exchange(
            x25519.X25519PublicKey.from_public_bytes(montgomery)
        )
        self.assertEqual(secret, self.query_context.secret)
        ciphertext = self.query[37:]
        self.assertEqual(self.query[35:37],
                         hmac.new(secret, ciphertext, hashlib.sha256).digest()[:2])
        decryptor = Cipher(algorithms.AES(secret[:16]), modes.ECB()).decryptor()
        plain = decryptor.update(ciphertext) + decryptor.finalize()
        self.assertEqual(plain[:57], role_rf.QUERY_DOMAIN + self.target +
                         struct.pack("<Q", 0x12345678))
        self.assertEqual(plain[121:], bytes(7))
        ed25519.Ed25519PrivateKey.from_private_bytes(bytes(range(32))).public_key().verify(
            plain[57:121], plain[:57]
        )
        original = self.status_packet(path_encoding=0x42, path=b"\x01\x02\x03\x04")
        result = role_rf.verify_status(original, self.query_context)
        self.assertEqual(result, {
            "valid": True, "sealed": False,
            "profile_generation": 1 << 62, "saved_mask": 5, "applied_mask": 0,
        })
        self.assertEqual(
            self.receive(self.kiss_rx(original), context=self.query_context,
                         verifier=role_rf.verify_status, kind="status"), result
        )
        self.assertEqual(
            self.receive(b"\xc0\x06\xf8\x01\xc0" + self.kiss_rx(original),
                         context=self.query_context,
                         verifier=role_rf.verify_status, kind="status"), result
        )
        self.assertEqual(role_rf.status_document(result), {
            "profile_generation": str(1 << 62),
            "saved_roles": ["repeater", "companion"],
            "applied_roles": [],
            "sealed": False,
        })
        self.assertEqual(
            role_rf.verify_status(self.status_packet(flags=3), self.query_context)["sealed"],
            True
        )
        unreadable = role_rf.verify_status(
            self.status_packet(generation=0, saved=0, applied=0, flags=2),
            self.query_context
        )
        self.assertEqual(unreadable["valid"], False)
        with self.assertRaisesRegex(ValueError, "state unknown"):
            role_rf.status_document(unreadable)

    def test_rejects_invalid_status_or_wrong_query(self):
        original = self.status_packet()
        corrupt = bytearray(original)
        corrupt[5] ^= 1
        previous_query, previous_context = role_rf.query_with_context(
            bytes(range(32)), self.target, 0x12345677,
            sender_seed=bytes([42]) * 32
        )
        self.assertEqual(len(previous_query), len(self.query))
        for packet, context in (
            (bytes(corrupt), self.query_context),
            (self.status_packet(signature=bytes(64)), self.query_context),
            (self.status_packet(nonce=0x12345677), self.query_context),
            (original, previous_context),
            (self.status_packet(flags=4), self.query_context),
            (self.status_packet(saved=16), self.query_context),
            (self.status_packet(applied=16), self.query_context),
            (self.status_packet(flags=0), self.query_context),
            (self.status_packet(flags=2), self.query_context),
            (self.status_packet(padding=bytes(10) + b"\x01"), self.query_context),
            (original[:1] + b"\xc0" + original[2:], self.query_context),
            (b"\x04" + original[1:], self.query_context),
        ):
            with self.subTest(packet=packet[:9].hex()):
                self.assertIsNone(role_rf.verify_status(packet, context))
        with self.assertRaises(ValueError):
            role_rf.query_with_context(bytes(range(32)), self.target, 0)
        with self.assertRaises(ValueError):
            role_rf.query_with_context(bytes(range(32)), self.target, 1 << 64)


if __name__ == "__main__":
    unittest.main()
