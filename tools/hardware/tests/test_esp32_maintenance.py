# SPDX-License-Identifier: Apache-2.0
from concurrent.futures import ThreadPoolExecutor
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
import time
from pathlib import Path
import socket
import struct
import tarfile
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from tools.hardware import esp32_maintenance as maintenance
from tools.hardware.rf import _shared_secret, public_hex
from tools import node_backup


def backup(seed):
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode="w") as stream:
        for name, content in {"manifest.json": b'{"schema_version":1,"format":"meshcore-node-backup"}',
                              "identities/repeater": bytes(64)}.items():
            entry = tarfile.TarInfo(name)
            entry.size = len(content)
            stream.addfile(entry, io.BytesIO(content))
    raw = archive.getvalue()
    encoded = b"".join(bytes((len(raw[offset:offset + 128]) - 1,)) + raw[offset:offset + 128]
                       for offset in range(0, len(raw), 128))
    ephemeral = bytes(reversed(seed))
    shared = _shared_secret(Ed25519PrivateKey.from_private_bytes(ephemeral), bytes.fromhex(public_hex(seed)))
    iv = bytes(16)
    key = hashlib.sha256(node_backup.ENCRYPTION_DOMAIN + shared).digest()[:16]
    cipher = Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()
    raw = (b"MCB\x01\x01\0\0\0" + bytes.fromhex(public_hex(ephemeral)) +
           bytes.fromhex(public_hex(seed)) + iv + cipher.update(encoded) + cipher.finalize())
    return raw + hmac.new(hashlib.sha256(node_backup.AUTHENTICATION_DOMAIN + shared).digest(), raw,
                          hashlib.sha256).digest()


def image():
    payload = b"meshcore-esp-target-v1:xiao-esp32s3:firmware"
    header = bytearray(24)
    header[0:2] = bytes((0xe9, 1))
    struct.pack_into("<H", header, 12, 9)
    raw = bytes(header) + struct.pack("<II", 0x3f400000, len(payload)) + payload
    checksum = 0xef
    for byte in payload:
        checksum ^= byte
    return raw + bytes(15 - len(raw) % 16) + bytes((checksum,))


class RadioHandler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, value, code=200):
        raw = json.dumps(value).encode() if isinstance(value, dict) else value.encode()
        self.send_response(code)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def disconnect(self):
        self.connection.shutdown(socket.SHUT_RDWR)
        self.connection.close()

    def do_GET(self):
        radio = self.server.radio
        if self.path == "/admin/update":
            self.reply(radio.status())
        elif self.path == "/api/status":
            key = "f" * 64 if radio.changed_identity and radio.booted else "1" * 64
            self.reply({"roles": [{"role": "repeater", "name": "test-radio", "public_key": key, "ready": True}],
                        "profile": {"frequency_hz": 900000000, "generation": int(radio.booted)}})
        else:
            self.reply("unknown", 404)

    def do_POST(self):
        radio = self.server.radio
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        radio.posts.append((self.path, body))
        if self.path == "/admin/login":
            self.reply("a" * 32)
        elif self.path == "/admin/logout":
            self.reply("OK")
        elif self.path == "/admin/command":
            command = body.decode()
            if command.startswith("key ") and command.endswith(" pending"):
                self.reply("KEY pending" if radio.pending_identity else "Error: identity or pending key unavailable")
            elif command == "bot contacts flush":
                self.reply("Error: learned contacts checkpoint failed; dirty=1" if radio.flush_failure
                           else "Saved learned contacts; dirty=0")
            elif command == "packet 1 status":
                self.reply("runtime=none saved=0 live=0 sealed=0")
            elif command == "packet 1 hash":
                self.reply("empty")
            elif command == "packet 1 budget":
                self.reply("1 100000 20000 9")
            elif command == "packet 0 status":
                self.reply(f"runtime=wasm saved={int(radio.sampler_enabled)} live={int(radio.sampler_enabled)} sealed=0")
            elif command == "packet 0 hash":
                self.reply("b" * 64)
            elif command == "packet 0 budget":
                self.reply("1 100000 20000 9")
            elif command.startswith("packet 0 enable "):
                radio.sampler_enabled = command.endswith("on")
                self.reply("Saved packet enable state")
            elif command.endswith(" status") and command.startswith("source"):
                size = 200 if radio.booted else 100
                slot = 3 if radio.bundled else 0
                self.reply(f"gen=7 active={slot} prev=3 size={size if radio.bundled else 100} upload=none next=0/0; source durably saved and active")
            elif command.endswith(" hash") and command.startswith("source"):
                hashed = "d" * 64 if radio.booted and (radio.changed_source or radio.bundled) else "c" * 64
                self.reply("SHA256 " + hashed + " gen=7")
            elif " list " in command:
                offset = command.rsplit(" ", 1)[1]
                self.reply("rules=0 next=" + offset)
            else:
                self.reply("saved " + command)
        elif self.path == "/admin/update":
            record = json.loads(radio.receipt.read_text())
            assert record["upload_attempted"] is True
            assert record.get("sampler_restore_pending", False) == record.get("sampler", {}).get("enabled", False)
            assert bool(self.headers.get("X-Mast-Allow-Unsaved-State")) == record["allow_unsaved_state"]
            radio.uploads += 1
            if radio.http_flush_failure:
                self.reply({"error": "Native state flush failed before update; inspect native role storage and bot contacts"}, 503)
                return
            radio.uploaded = True
            radio.digest = self.headers["X-Mast-SHA256"]
            radio.size = len(body)
            if radio.lost_upload:
                self.disconnect()
            else:
                self.reply(radio.status())
        elif self.path == "/admin/update/reboot":
            record = json.loads(radio.receipt.read_text())
            assert record["reboot_attempted"] is True
            radio.reboots += 1
            if radio.reboot_release is not None:
                radio.reboot_started.set()
                radio.reboot_release.wait(5)
            radio.booted = not radio.stuck_reboot
            if radio.lost_reboot:
                self.disconnect()
            else:
                self.reply({"reboot": True})
        else:
            self.reply("unknown", 404)


class Radio:
    def __init__(self):
        self.posts = []
        self.uploads = self.reboots = 0
        self.uploaded = self.booted = False
        self.digest, self.size = "", 0
        self.current = "0" * 64
        self.sampler_enabled = True
        self.reboot_started = threading.Event()
        self.reboot_release = None
        self.flush_failure = self.lost_upload = self.lost_reboot = self.stuck_reboot = False
        self.unsaved_supported = True
        self.http_flush_failure = False
        self.changed_identity = self.changed_source = self.bundled = self.pending_identity = False

    def status(self):
        return {"running_sha256": self.digest if self.booted else self.current,
                "running_size": self.size if self.booted else 100, "running_hash_error": "",
                "state": "idle" if self.booted or not self.uploaded else "verified",
                "unsaved_state_supported": self.unsaved_supported, "boot_health": "healthy", "reboot_ready": self.uploaded and not self.booted,
                "sha256": self.digest, "received": self.size}


class MaintenanceTests(unittest.TestCase):
    def setUp(self):
        delay = patch.object(maintenance, "COMMAND_DELAY", 0)
        delay.start()
        self.addCleanup(delay.stop)
        self.directory = tempfile.TemporaryDirectory()
        root = Path(self.directory.name)
        self.radio = Radio()
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), RadioHandler)
        self.server.radio = self.radio
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.args = SimpleNamespace(action="update", url=f"http://127.0.0.1:{self.server.server_port}",
                                    password_file=root / "password", receipt=root / "receipt.json", wait=1,
                                    poll_interval=.1, image=root / "firmware.bin", build_manifest=root / "manifest.json",
                                    key_file=root / "operator.seed", backup=root / "backup.mcb",
                                    expected_current_sha256="0" * 64, target="xiao-esp32s3",
                                    retain_command=[], sampler_slot=0, sampler_sha256="b" * 64,
                                    legacy_no_cache_flush=False, allow_unsaved_state=False)
        seed = bytes(range(32))
        for path, raw in ((self.args.password_file, b"password"), (self.args.key_file, seed),
                          (self.args.backup, backup(seed)), (self.args.image, image())):
            path.write_bytes(raw)
            path.chmod(0o600)
        self.radio.receipt = self.args.receipt
        self.args.build_manifest.write_text(json.dumps({"files": [{"name": "firmware.bin",
            "bytes": len(image()), "sha256": hashlib.sha256(image()).hexdigest()}]}))

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.directory.cleanup()

    def receipt(self):
        return json.loads(self.args.receipt.read_text())

    def test_application_update_authenticates_backup_and_retains_state(self):
        maintenance.maintain(self.args)
        record = self.receipt()
        self.assertTrue(record["complete"])
        self.assertTrue(record["contacts_flushed"])
        self.assertEqual(record["backup"]["records"], 2)
        self.assertEqual((self.radio.uploads, self.radio.reboots), (1, 1))
        self.assertTrue(self.radio.sampler_enabled)
        self.assertFalse(record["sampler_restore_pending"])
        self.assertEqual(self.args.receipt.stat().st_mode & 0o777, 0o600)
        self.assertNotIn("password", self.args.receipt.read_text())
        self.assertNotIn(bytes(range(32)).hex(), self.args.receipt.read_text())

    def test_lost_upload_and_reboot_replies_are_never_replayed(self):
        self.radio.lost_upload = self.radio.lost_reboot = True
        maintenance.maintain(self.args)
        self.assertTrue(self.receipt()["complete"])
        self.assertEqual((self.radio.uploads, self.radio.reboots), (1, 1))
        self.args.action = "resume"
        maintenance.maintain(self.args)
        self.assertEqual((self.radio.uploads, self.radio.reboots), (1, 1))

    def test_uncertain_reboot_resume_checks_only(self):
        self.radio.stuck_reboot = True
        with self.assertRaisesRegex(TimeoutError, "has not returned healthy"):
            maintenance.maintain(self.args)
        self.assertTrue(self.receipt()["reboot_attempted"])
        self.assertTrue(self.receipt()["sampler_restore_pending"])
        self.radio.booted = True
        self.args.action = "resume"
        maintenance.maintain(self.args)
        self.assertTrue(self.receipt()["complete"])
        self.assertTrue(self.radio.sampler_enabled)
        self.assertEqual((self.radio.uploads, self.radio.reboots), (1, 1))

    def test_resume_after_verified_upload_sends_one_unattempted_reboot(self):
        with patch.object(maintenance, "finish", side_effect=TimeoutError("interrupted before reboot")):
            with self.assertRaisesRegex(TimeoutError, "interrupted before reboot"):
                maintenance.maintain(self.args)
        self.assertTrue(self.receipt()["upload_attempted"])
        self.assertNotIn("reboot_attempted", self.receipt())
        self.args.action = "resume"
        maintenance.maintain(self.args)
        self.assertEqual((self.radio.uploads, self.radio.reboots), (1, 1))
        self.assertTrue(self.receipt()["complete"])

    def test_concurrent_resumes_share_one_receipt_lock_and_only_one_reboot(self):
        with patch.object(maintenance, "finish", side_effect=TimeoutError("interrupted before reboot")):
            with self.assertRaises(TimeoutError):
                maintenance.maintain(self.args)
        self.args.action = "resume"
        self.args.wait = 5
        self.radio.reboot_release = threading.Event()
        barrier = threading.Barrier(3)
        def resume():
            barrier.wait(timeout=5)
            try:
                maintenance.maintain(self.args)
            except ValueError as error:
                return str(error)
            return "complete"
        with ThreadPoolExecutor(max_workers=2) as executor:
            futures = [executor.submit(resume) for _ in range(2)]
            barrier.wait(timeout=5)
            try:
                self.assertTrue(self.radio.reboot_started.wait(5))
            finally:
                self.radio.reboot_release.set()
            results = [future.result(timeout=5) for future in futures]
        self.assertEqual(results.count("complete"), 1)
        self.assertTrue(any("receipt is already in use" in result for result in results))
        self.assertEqual((self.radio.uploads, self.radio.reboots), (1, 1))
        self.assertTrue(self.receipt()["complete"])
        lock = Path(str(self.args.receipt) + ".lock")
        self.assertEqual(lock.stat().st_mode & 0o777, 0o600)

    def test_resume_refuses_reboot_if_baseline_changed(self):
        with patch.object(maintenance, "finish", side_effect=TimeoutError("interrupted before reboot")):
            with self.assertRaises(TimeoutError):
                maintenance.maintain(self.args)
        self.radio.current = "a" * 64
        self.args.action = "resume"
        with self.assertRaisesRegex(ValueError, "changed since preflight"):
            maintenance.maintain(self.args)
        self.assertEqual((self.radio.uploads, self.radio.reboots), (1, 0))

    def test_baseline_hash_mismatch_refuses_every_radio_write(self):
        self.radio.current = "a" * 64
        with self.assertRaisesRegex(ValueError, "Running application differs"):
            maintenance.maintain(self.args)
        self.assertEqual((self.radio.uploads, self.radio.reboots), (0, 0))
        self.assertTrue(self.radio.sampler_enabled)
        self.assertFalse(any(path == "/admin/command" for path, _ in self.radio.posts))

    def test_build_manifest_mismatch_refuses_connection(self):
        self.args.build_manifest.write_text('{"files":[]}')
        with self.assertRaisesRegex(ValueError, "build manifest"):
            maintenance.maintain(self.args)
        self.assertEqual(self.radio.posts, [])

    def test_corrupt_authenticated_backup_refuses_connection(self):
        raw = bytearray(self.args.backup.read_bytes())
        raw[-1] ^= 1
        self.args.backup.write_bytes(raw)
        with self.assertRaisesRegex(ValueError, "authentication failed"):
            maintenance.maintain(self.args)
        self.assertEqual(self.radio.posts, [])

    def test_failed_contact_flush_refuses_upload_and_leaves_sampler_enabled(self):
        self.radio.flush_failure = True
        with self.assertRaisesRegex(ValueError, "checkpoint failed"):
            maintenance.maintain(self.args)
        self.assertEqual((self.radio.uploads, self.radio.reboots), (0, 0))
        self.assertTrue(self.radio.sampler_enabled)
        commands = [body for path, body in self.radio.posts if path == "/admin/command"]
        self.assertLess(commands.index(b"packet 0 enable off"), commands.index(b"bot contacts flush"))
        self.assertLess(commands.index(b"bot contacts flush"), commands.index(b"packet 0 enable on"))
        self.assertFalse(self.receipt()["sampler_restore_pending"])

    def test_explicit_unsaved_state_recovery_records_intent_before_upload(self):
        self.args.allow_unsaved_state = True
        self.radio.flush_failure = True
        maintenance.maintain(self.args)
        self.assertTrue(self.receipt()["allow_unsaved_state"])
        self.assertFalse(self.receipt()["contacts_flushed"])
        self.assertEqual(self.radio.uploads, 1)

    def test_unsaved_recovery_refuses_old_backup(self):
        self.args.allow_unsaved_state = True
        old = time.time() - 3601
        os.utime(self.args.backup, (old, old))
        with self.assertRaisesRegex(ValueError, "last hour"):
            maintenance.maintain(self.args)
        self.assertEqual(self.radio.posts, [])

    def test_unsaved_recovery_refuses_unsupported_radio(self):
        self.args.allow_unsaved_state = True
        self.radio.unsaved_supported = False
        with self.assertRaisesRegex(ValueError, "does not support"):
            maintenance.maintain(self.args)
        self.assertEqual(self.radio.uploads, 0)

    def test_http_failure_records_actionable_error_without_replaying_upload(self):
        self.radio.http_flush_failure = True
        with self.assertRaisesRegex(ValueError, "Upload is not verified"):
            maintenance.maintain(self.args)
        self.assertEqual(self.receipt()["upload_failure"]["status"], 503)
        self.assertIn("Native state flush failed", self.receipt()["upload_failure"]["error"])
        self.assertEqual(self.radio.uploads, 1)
        self.assertNotIn("reboot_attempted", self.receipt())

    def test_explicit_legacy_mode_is_recorded_and_does_not_issue_flush(self):
        self.args.legacy_no_cache_flush = True
        maintenance.maintain(self.args)
        self.assertTrue(self.receipt()["legacy_no_cache_flush"])
        self.assertNotIn(("/admin/command", b"bot contacts flush"), self.radio.posts)

    def test_retained_identity_mismatch_fails_with_sampler_restored(self):
        self.radio.changed_identity = True
        with self.assertRaisesRegex(ValueError, "Retained state changed"):
            maintenance.maintain(self.args)
        self.assertFalse(self.receipt()["complete"])
        self.assertTrue(self.radio.sampler_enabled)

    def test_bundled_program_can_change_but_selected_custom_program_cannot(self):
        self.radio.bundled = True
        maintenance.maintain(self.args)
        self.assertTrue(self.receipt()["complete"])
        self.args.receipt.unlink()
        self.radio.booted = self.radio.uploaded = self.radio.bundled = False
        self.radio.changed_source = True
        with self.assertRaisesRegex(ValueError, "Retained state changed"):
            maintenance.maintain(self.args)
        self.assertFalse(self.receipt()["complete"])

    def test_existing_receipt_is_never_overwritten(self):
        self.args.receipt.write_text("existing receipt")
        self.args.receipt.chmod(0o600)
        with self.assertRaises(FileExistsError):
            maintenance.maintain(self.args)
        self.assertEqual(self.args.receipt.read_text(), "existing receipt")
        self.assertEqual(self.radio.posts, [])

    def test_default_update_preserves_packet_program_without_pausing_it(self):
        self.args.sampler_slot = self.args.sampler_sha256 = None
        maintenance.maintain(self.args)
        self.assertTrue(self.receipt()["complete"])
        self.assertTrue(self.radio.sampler_enabled)
        self.assertFalse(any(body.startswith(b"packet 0 enable") for _, body in self.radio.posts))

    def test_disabled_pinned_sampler_stays_disabled(self):
        self.radio.sampler_enabled = False
        maintenance.maintain(self.args)
        self.assertTrue(self.receipt()["complete"])
        self.assertFalse(self.radio.sampler_enabled)
        self.assertFalse(any(body.startswith(b"packet 0 enable") for _, body in self.radio.posts))

    def test_pending_identity_refuses_upload_before_restart_can_apply_it(self):
        self.radio.pending_identity = True
        with self.assertRaisesRegex(ValueError, "Pending identity"):
            maintenance.maintain(self.args)
        self.assertEqual((self.radio.uploads, self.radio.reboots), (0, 0))
        self.assertTrue(self.radio.sampler_enabled)

    def test_unknown_sampler_is_not_paused(self):
        self.args.sampler_sha256 = "a" * 64
        with self.assertRaisesRegex(ValueError, "sampler hash differs"):
            maintenance.maintain(self.args)
        self.assertEqual((self.radio.uploads, self.radio.reboots), (0, 0))
        self.assertTrue(self.radio.sampler_enabled)


if __name__ == "__main__":
    unittest.main()
