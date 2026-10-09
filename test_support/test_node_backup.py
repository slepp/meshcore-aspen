# SPDX-License-Identifier: Apache-2.0
import hashlib
import base64
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from tools.hardware.rf import _shared_secret, public_hex
from tools import node_backup


class BackupClient:
    def __init__(self, raw, interrupt=None, legacy=False):
        self.raw = raw
        self.interrupt = interrupt
        self.commands = []
        self.waited = False
        self.identifier = hashlib.sha256(raw).hexdigest()[:16]
        self.legacy = legacy

    def command(self, text):
        self.commands.append(text)
        if text.startswith(("backup start ", "backup start-ram ")) or text in ("backup load", "backup load-ram"):
            return "PREPARING"
        if text == "backup status":
            return f"READY {self.identifier} bytes={len(self.raw)} sha={hashlib.sha256(self.raw).hexdigest()}"
        if text.startswith("backup read64 ") and self.legacy:
            return "Error: unknown backup command; use backup help"
        if text.startswith(("backup read ", "backup read64 ")):
            if not self.waited:
                self.waited = True
                return "WAIT ms=5000"
            offset = int(text.split()[-1])
            if self.interrupt is not None and offset >= self.interrupt:
                raise TimeoutError("RF reply uncertain")
            if text.startswith("backup read64 "):
                header = f"CHUNK64 {self.identifier} {offset} "
                count = (145 - len(header)) * 3 // 4
                return header + base64.b64encode(self.raw[offset:offset + count]).decode().rstrip("=")
            return f"CHUNK {self.identifier} {offset} {self.raw[offset:offset + 48].hex()}"
        raise AssertionError(text)

    def chunks(self, identifier):
        assert identifier == self.identifier
        for offset in range(0, len(self.raw), 64):
            if self.interrupt is not None and offset >= self.interrupt:
                raise OSError("WiFi connection interrupted")
            yield self.raw[offset:offset + 64]


class NodeBackupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        runner = os.environ.get("NODE_BACKUP_RUNNER")
        if not runner:
            raise RuntimeError("Run make -C firmware/esp32 node-backup-test")
        cls.seed = bytes(range(32))
        ephemeral_seed = hashlib.sha256(b"node-backup-ephemeral-fixture").digest()
        recipient = public_hex(cls.seed)
        ephemeral = public_hex(ephemeral_seed)
        shared = _shared_secret(Ed25519PrivateKey.from_private_bytes(ephemeral_seed), bytes.fromhex(recipient))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.mcb"
            subprocess.run([runner, str(path), ephemeral, recipient, shared.hex()], check=True)
            cls.raw = path.read_bytes()

    def test_native_archive_decrypts_and_contains_useful_records(self):
        decoded = node_backup.decrypt(self.raw, self.seed)
        manifest, files = node_backup.entries(decoded)
        self.assertEqual(manifest["platform"], "test")
        self.assertEqual(files["nvs/mc-onchip/companion.blob"], bytes(range(256)) + bytes(1792))
        self.assertIn(b"return 'hello'", files["files/command-bot/a.lua"])
        self.assertLess(len(self.raw), 1024)
        self.assertEqual(len(decoded), 5632)

    def test_wrong_recipient_and_tampered_bytes_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "different operator"):
            node_backup.decrypt(self.raw, bytes(reversed(self.seed)))
        for offset in (72, 88, len(self.raw) - 1):
            with self.subTest(offset=offset):
                changed = bytearray(self.raw)
                changed[offset] ^= 1
                with self.assertRaisesRegex(ValueError, "authentication failed"):
                    node_backup.decrypt(bytes(changed), self.seed)

    def test_rle_truncation_and_expansion_limit(self):
        for raw in (b"\x02ab", b"\x82", b"\x80a"):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                node_backup.unpack_rle(raw)
        with self.assertRaisesRegex(ValueError, "decoded limit"):
            node_backup.unpack_rle(b"\xff\0" * (node_backup.RAW_LIMIT // 128 + 1))
        self.assertEqual(node_backup.unpack_rle(b"\x02abc\x82x"), b"abcxxx")

    def test_extraction_is_private_and_never_overwrites(self):
        _, files = node_backup.entries(node_backup.decrypt(self.raw, self.seed))
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "backup"
            node_backup.extract(files, output)
            self.assertEqual(output.stat().st_mode & 0o777, 0o700)
            for name, value in files.items():
                self.assertEqual((output / name).stat().st_mode & 0o777, 0o600)
                self.assertEqual((output / name).read_bytes(), value)
            with self.assertRaises(FileExistsError):
                node_backup.extract(files, output)

    def test_wifi_and_radio_download_resume_without_recreating_snapshot(self):
        for radio in (False, True):
            with self.subTest(radio=radio), tempfile.TemporaryDirectory() as directory, patch.object(node_backup.time, "sleep") as sleep:
                output = Path(directory) / "node.mcb"
                first = BackupClient(self.raw, interrupt=144)
                with self.assertRaises((OSError, TimeoutError)):
                    node_backup.download(first, output, self.seed, radio=radio)
                self.assertFalse(output.exists())
                partial = Path(str(output) + ".part")
                size = partial.stat().st_size
                self.assertGreater(size, 0)
                self.assertEqual(partial.stat().st_mode & 0o777, 0o600)
                second = BackupClient(self.raw)
                manifest, count, length = node_backup.download(second, output, self.seed, resume=True, radio=radio)
                self.assertEqual(output.read_bytes(), self.raw)
                self.assertEqual(manifest["platform"], "test")
                self.assertEqual(count, 3)
                self.assertEqual(length, len(self.raw))
                self.assertFalse(partial.exists())
                self.assertFalse(Path(str(partial) + ".json").exists())
                self.assertEqual(second.commands[0], "backup load")
                self.assertFalse(any(text.startswith("backup start") for text in second.commands))
                if radio:
                    self.assertIn(f"backup read64 {second.identifier} {size}", second.commands)
                    self.assertTrue(sleep.called)

    def test_download_rejects_truncation_and_authenticated_tampering(self):
        class Truncated(BackupClient):
            def chunks(self, identifier):
                yield self.raw[:-1]
        for client, message in ((Truncated(self.raw), "incomplete"),):
            with tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / "node.mcb"
                with self.assertRaisesRegex(ValueError, message):
                    node_backup.download(client, output, self.seed)
                self.assertFalse(output.exists())
        damaged = bytearray(self.raw)
        damaged[-1] ^= 1
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "node.mcb"
            with self.assertRaisesRegex(ValueError, "authentication failed"):
                node_backup.download(BackupClient(bytes(damaged)), output, self.seed)
            self.assertFalse(output.exists())

    def test_resume_rejects_changed_snapshot_and_local_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "node.mcb"
            with self.assertRaises(OSError):
                node_backup.download(BackupClient(self.raw, interrupt=144), output, self.seed)
            changed = bytearray(self.raw)
            changed[-1] ^= 1
            with self.assertRaisesRegex(ValueError, "Saved backup changed"):
                node_backup.download(BackupClient(bytes(changed)), output, self.seed, resume=True)
            partial = Path(str(output) + ".part")
            raw = bytearray(partial.read_bytes())
            raw[-1] ^= 1
            partial.write_bytes(raw)
            with self.assertRaisesRegex(ValueError, "prefix differs"):
                node_backup.download(BackupClient(self.raw), output, self.seed, resume=True)

    def test_saved_download_does_not_replace_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            client = BackupClient(self.raw)
            output = Path(directory) / "saved.mcb"
            node_backup.download(client, output, self.seed, saved=True)
            self.assertEqual(client.commands[0], "backup load")
            self.assertFalse(any(text.startswith("backup start") for text in client.commands))
            self.assertEqual(output.read_bytes(), self.raw)

    def test_volatile_download_and_resume_use_only_ram_commands(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "volatile.mcb"
            first = BackupClient(self.raw, interrupt=144)
            with self.assertRaises(OSError):
                node_backup.download(first, output, self.seed, transient=True)
            self.assertTrue(first.commands[0].startswith("backup start-ram "))
            second = BackupClient(self.raw)
            node_backup.download(second, output, self.seed, resume=True, transient=True)
            self.assertEqual(second.commands[0], "backup load-ram")
            self.assertEqual(output.read_bytes(), self.raw)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "saved-volatile.mcb"
            client = BackupClient(self.raw)
            node_backup.download(client, output, self.seed, saved=True, transient=True)
            self.assertEqual(client.commands[0], "backup load-ram")
            self.assertEqual(output.read_bytes(), self.raw)

    def test_rf_legacy_fallback_and_variable_base64_chunks(self):
        metadata = {"id": hashlib.sha256(self.raw).hexdigest()[:16], "bytes": len(self.raw)}
        for legacy in (False, True):
            with self.subTest(legacy=legacy), patch.object(node_backup.time, "sleep"):
                client = BackupClient(self.raw, legacy=legacy)
                chunks = list(node_backup.rf_chunks(client, metadata, 0))
                self.assertEqual(b"".join(chunks), self.raw)
                if legacy:
                    self.assertEqual(len(chunks[0]), 48)
                    self.assertEqual(sum("backup read64 " in command for command in client.commands), 1)
                else:
                    self.assertGreater(len(chunks[0]), 48)

    def test_rf_rejects_invalid_chunks_without_legacy_fallback(self):
        identifier = "a" * 16
        metadata = {"id": identifier, "bytes": 300}
        for payload in ("A", "AB", "AA=", "AA-", "AAAAA", "A" * 164):
            class Invalid:
                def command(self, text):
                    return f"CHUNK64 {identifier} 0 {payload}"
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                list(node_backup.rf_chunks(Invalid(), metadata, 0))


if __name__ == "__main__":
    unittest.main()
