# SPDX-License-Identifier: Apache-2.0
from datetime import datetime, timedelta, timezone
import hashlib
import json
import unittest
from unittest.mock import MagicMock
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID
from tools.hardware import admin, cloudroom_config as config


def fixture():
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "CloudRoom test CA")])
    now = datetime.now(timezone.utc)
    certificate = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
                   .public_key(key.public_key()).serial_number(1)
                   .not_valid_before(now - timedelta(days=1)).not_valid_after(now + timedelta(days=1))
                   .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
                   .sign(key, hashes.SHA256()))
    return {
        "schema": 1, "enabled": True, "aliases": [{
            "host": "room.example", "address": "192.0.2.1",
            "ca": certificate.public_bytes(serialization.Encoding.PEM).decode("ascii"),
            "token": "fixture-token", "id": "test-room", "name": "Test Room",
            "public_key": bytes(range(32)).hex(),
        }],
    }


class CloudRoomConfig(unittest.TestCase):
    def setUp(self):
        self.profile = fixture()

    def test_exact_record_layout_and_digest(self):
        record = config.encode(json.dumps(self.profile))
        self.assertEqual(len(record), 9296)
        self.assertEqual(record[:8], b"CRC\1\1\1\0\0")
        self.assertEqual(record[-32:], hashlib.sha256(record[:-32]).digest())
        alias = config.ALIAS.unpack_from(record, 8)
        self.assertEqual(alias[0].rstrip(b"\0"), b"room.example")
        self.assertEqual(alias[2].rstrip(b"\0"), self.profile["aliases"][0]["ca"].encode())
        self.assertEqual(alias[3].rstrip(b"\0"), b"fixture-token")
        self.assertEqual(alias[6], bytes(range(32)))
        self.assertEqual(record[8 + config.ALIAS.size:-32], bytes(config.ALIAS.size))
        second = self.profile["aliases"][0] | {"id": "second", "public_key": "12" * 32, "address": ""}
        self.profile["aliases"].append(second)
        self.profile["enabled"] = False
        self.assertEqual(config.encode(json.dumps(self.profile))[:8], b"CRC\1\0\2\0\0")

    def test_rejects_malformed_unknown_fields_and_duplicates(self):
        for content in ("{}", "[]", '{"schema":1,"schema":1}', b"\xff", "[" * 1500 + "]" * 1500,
                        " " * (config.LIMIT + 1)):
            with self.subTest(content=str(content)[:20]), self.assertRaises(ValueError):
                config.encode(content)
        for change in ({"schema": True}, {"schema": 2}, {"enabled": 1}, {"aliases": []},
                       {"aliases": self.profile["aliases"] * 3}, {"secret": "private"}):
            with self.subTest(change=change.keys()), self.assertRaises(ValueError):
                config.encode(json.dumps(self.profile | change))

    def test_unsafe_or_oversized_aliases_never_send(self):
        for field, value in (
            ("host", "https://room.example"), ("host", "a:443"), ("host", "room.example/path"),
            ("host", "-room.example"), ("host", "room..example"), ("host", "a" * 64 + ".example"),
            ("host", "a" * 128), ("address", "256.0.0.1"), ("address", "192.00.2.1"),
            ("ca", "-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----"),
            ("ca", self.profile["aliases"][0]["ca"] + "garbage"),
            ("token", "Bearer token"), ("token", "secret\r\nInjected: yes"), ("token", "x" * 257),
            ("id", "../room"), ("id", "a" * 65), ("name", ""), ("name", "a" * 33),
            ("public_key", "0" * 64), ("public_key", "f" * 64), ("public_key", "AB" * 32),
        ):
            profile = self.profile | {"aliases": [self.profile["aliases"][0] | {field: value}]}
            client = MagicMock()
            with self.subTest(field=field, value=str(value)[:20]), self.assertRaises(ValueError):
                admin.configure_cloudroom(client, json.dumps(profile), progress=lambda _: None)
            client.command.assert_not_called()
        for change in ({}, {"id": "second"},
                       {"host": "ROOM.EXAMPLE", "public_key": "12" * 32}):
            profile = self.profile | {"aliases": self.profile["aliases"] + [self.profile["aliases"][0] | change]}
            with self.assertRaises(ValueError):
                config.encode(json.dumps(profile))

    def test_idempotent_chunk_retries_and_uncertain_commit_not_replayed(self):
        record = config.encode(json.dumps(self.profile))
        digest, identifier = record[-32:].hex(), record[-32:].hex()[:16]
        calls, chunks = [], {}
        def command(text):
            calls.append(text)
            if text == "cloudroom config api":
                return "Cloud room config ABI=1 bytes=9296 chunk=48 aliases=2 apply=restart"
            if text == "cloudroom config begin " + digest:
                return "Cloud room upload ready received=48"
            if text.startswith("cloudroom config chunk "):
                _, _, _, actual, index, data = text.split()
                self.assertEqual(actual, identifier)
                offset = int(index) * 48
                self.assertEqual(bytes.fromhex(data), record[offset:offset + 48])
                chunks[index] = chunks.get(index, 0) + 1
                if index == "1" and chunks[index] == 1:
                    raise TimeoutError("chunk reply lost")
                return f"Cloud room chunk saved received={offset + len(bytes.fromhex(data))}"
            if text == "cloudroom config commit " + identifier:
                raise TimeoutError("commit reply lost")
            if text == "cloudroom config hash":
                return digest
            self.fail(text)
        result = admin.configure_cloudroom(MagicMock(command=command), json.dumps(self.profile), progress=lambda _: None)
        self.assertEqual(result, digest)
        self.assertEqual(chunks["1"], 2)
        self.assertEqual(sum(text.startswith("cloudroom config commit ") for text in calls), 1)
        self.assertTrue(all(len(text) <= 145 for text in calls))
        self.assertFalse(any(self.profile["aliases"][0]["token"] in text for text in calls))

    def test_bad_resume_failed_publication_and_hash_mismatch(self):
        api = "Cloud room config ABI=1 bytes=9296 chunk=48 aliases=2 apply=restart"
        for received in (-1, 1, 9297):
            client = MagicMock(command=MagicMock(side_effect=[api, f"Cloud room upload ready received={received}"]))
            with self.assertRaisesRegex(ValueError, "resume offset"):
                admin.configure_cloudroom(client, json.dumps(self.profile), progress=lambda _: None)
            self.assertEqual(client.command.call_count, 2)
        for result, expected in (
            (["Error: cloud room settings write/readback failed; saved selection unchanged"], "write/readback"),
            (["Cloud room settings saved; restart to apply", "0" * 64], "differs"),
        ):
            client = MagicMock(command=MagicMock(side_effect=[api, "Cloud room upload ready received=9296", *result]))
            with self.assertRaisesRegex(ValueError, expected):
                admin.configure_cloudroom(client, json.dumps(self.profile), progress=lambda _: None)

    def test_credential_upload_rejects_web_and_unix_without_commands(self):
        for connection_type in (admin.WebClient, admin.UnixClient):
            for wrap in (False, True):
                client = MagicMock(spec=connection_type)
                connection = admin.RuntimeClient(client, "wamr-2.4.1") if wrap else client
                with self.assertRaisesRegex(ValueError, "encrypted Management RF"):
                    admin.configure_cloudroom(connection, json.dumps(self.profile), progress=lambda _: None)
                client.command.assert_not_called()


if __name__ == "__main__":
    unittest.main()
