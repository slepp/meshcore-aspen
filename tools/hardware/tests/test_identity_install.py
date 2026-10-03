# SPDX-License-Identifier: Apache-2.0
import copy
import hashlib
import json
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.hardware import identity_install as field_keys
from tools.hardware import companion_cli as stock


from test_support.operator_inventory import configure_inventory

class FieldKeysTests(unittest.TestCase):
    def setUp(self):
        configure_inventory(self)
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.path = self.directory / "manifest.json"
        entries = []
        prefixes = set()
        for identifier, (name, role) in field_keys.peer_roles().items():
            for nonce in range(10000):
                seed = hashlib.sha256(f"disposable-{identifier}-{nonce}".encode()).digest()
                public = field_keys.public_hex(seed)
                if 0xb7 <= int(public[:2], 16) <= 0xbb and public[:6] not in prefixes:
                    break
            else:
                self.fail("Could not construct a disposable fixture")
            prefixes.add(public[:6])
            self.write(identifier + ".seed", seed)
            self.write(identifier + ".key", stock.native_private_key(seed))
            entries.append({"id": identifier, "name": name, "role": role,
                            "previous_public_key": field_keys.public_hex(bytes(32)),
                            "public_key": public, "prefix_3_bytes": public[:6],
                            "seed_file": identifier + ".seed", "native_key_file": identifier + ".key",
                            "advertised_role": True})
        self.document = {"format": "meshcore-fleet-keys-v1", "path_hash_bytes": 3, "identities": entries}
        self.save_manifest()

    def write(self, name, value):
        path = self.directory / name
        path.write_bytes(value)
        path.chmod(0o600)

    def save_manifest(self):
        self.write(self.path.name, json.dumps(self.document).encode())

    def test_native_material_matches_manifest_without_generating_replacements(self):
        records, material = field_keys.load_manifest(self.path)
        self.assertEqual(set(records), set(field_keys.peer_roles()))
        for identifier, native in material.items():
            self.assertEqual(native, (self.directory / (identifier + ".key")).read_bytes())

    def test_wrong_native_bytes_rejected(self):
        self.write("pine-bot.key", bytes(64))
        with self.assertRaisesRegex(ValueError, "does not match"):
            field_keys.load_manifest(self.path)

    def test_mismatched_public_key_and_colliding_prefix_rejected(self):
        for change in ("key", "collision", "width", "filename"):
            with self.subTest(change=change):
                original = copy.deepcopy(self.document)
                if change == "key":
                    self.document["identities"][0]["public_key"] = "b7" + "00" * 31
                    self.document["identities"][0]["prefix_3_bytes"] = "b70000"
                elif change == "collision":
                    self.document["identities"][1].update({
                        key: self.document["identities"][0][key] for key in ("public_key", "prefix_3_bytes")})
                elif change == "width":
                    self.document["path_hash_bytes"] = 1
                else:
                    self.document["identities"][0]["seed_file"] = "../outside.seed"
                self.save_manifest()
                with self.assertRaises(ValueError):
                    field_keys.load_manifest(self.path)
                self.document = original

    def test_secret_permissions_and_symlinks_rejected(self):
        secret = self.directory / "pine-bot.key"
        secret.chmod(0o644)
        with self.assertRaisesRegex(ValueError, "private regular"):
            field_keys.load_manifest(self.path)
        secret.chmod(0o600)
        secret.unlink()
        secret.symlink_to(self.directory / "cedar-base.key")
        with self.assertRaises(OSError):
            field_keys.load_manifest(self.path)

    def test_only_authorized_old_or_new_live_key_is_accepted(self):
        entry = self.document["identities"][0]
        for key in (entry["previous_public_key"], entry["public_key"]):
            field_keys.check_current(entry, key.upper())
        with self.assertRaisesRegex(ValueError, "neither"):
            field_keys.check_current(entry, "ff" * 32)

    def test_stock_replacement_must_retain_every_setting_and_channel(self):
        before = {"info": {"public_key": "old", "name": "Cedar-Base", "radio_sf": 7},
                  "channels": [{"slot": 0, "key_sha256": "saved"}], "path_hash_mode": 2}
        after = copy.deepcopy(before)
        after["info"]["public_key"] = "new"
        field_keys.verify_stock_retained(before, after, "new")
        after["channels"][0]["key_sha256"] = "changed"
        with self.assertRaisesRegex(ValueError, "changed settings"):
            field_keys.verify_stock_retained(before, after, "new")

    def test_private_import_echo_is_never_in_error(self):
        native = stock.native_private_key(bytes(32))
        with patch.object(field_keys.nrf, "serial_command", return_value="??: " + native.hex()):
            with self.assertRaises(ValueError) as raised:
                field_keys.stage_pine_bot("local-usb", native, "b7" + "00" * 31)
        self.assertNotIn(native.hex(), str(raised.exception))
        self.assertIn("withheld", str(raised.exception))

    def test_encrypted_aspen_import_checks_full_public_key_without_echoing_private_bytes(self):
        native = stock.native_private_key(bytes(32))
        public = field_keys.public_hex(bytes(32))
        client = Mock()
        for response in ("Pending " + public + "; reboot required; peers must learn new key",
                         "KEY " + public + "; already active; no reboot required"):
            client.command.return_value = response
            field_keys.stage_aspen_key(client, "command-bot", native, public)
            self.assertEqual(client.command.call_args.args[0], "key bot " + native.hex())
        for response in ("Error: " + native.hex(), "Pending " + "ab" * 32):
            client.command.return_value = response
            with self.assertRaisesRegex(ValueError, "response withheld") as raised:
                field_keys.stage_aspen_key(client, "management", native, public)
            self.assertNotIn(native.hex(), str(raised.exception))
        client.command.side_effect = ValueError(native.hex())
        with self.assertRaisesRegex(ValueError, "response withheld") as raised:
            field_keys.stage_aspen_key(client, "management", native, public)
        self.assertNotIn(native.hex(), str(raised.exception))

    def test_multiple_private_channel_reads_do_not_write_secret_logs(self):
        channels = [{"channel_idx": index, "channel_name": "", "channel_secret": "00" * 16}
                    for index in range(2)]
        port = Mock()
        port.is_symlink.return_value = True
        port.resolve.return_value.name = "ttyACM2"
        completed = SimpleNamespace(returncode=0, stdout="\n".join(map(repr, channels)), stderr="")
        with patch.object(stock, "DIRECTORY", self.directory), patch.object(stock, "device_port", return_value=port), \
                patch.object(stock.shutil, "which", return_value="/usr/bin/meshcli"), \
                patch.object(stock.subprocess, "run", return_value=completed) as run:
            actual = stock.run_cli("private-channels", ["get_channel 0", "get_channel 1"],
                                   channel_info=True, private_script=True)
        self.assertEqual(actual, channels)
        self.assertTrue(run.call_args.kwargs["pass_fds"])
        self.assertFalse(list(self.directory.glob("private-channels.*")))

    def test_advert_retries_only_explicit_queue_rejection(self):
        client = Mock()
        busy = "Error: advert unavailable; role inactive, radio/queue busy or rate/airtime limit"
        client.command.side_effect = [busy, "Queued zero-hop advert; delivery/peer learning unconfirmed"]
        with patch.object(field_keys.time, "sleep") as sleep:
            field_keys.advertise_aspen(client, "repeater")
            sleep.assert_called_once_with(1)
        client.command.side_effect = ["Error: role disabled"]
        with patch.object(field_keys.time, "sleep") as sleep:
            with self.assertRaisesRegex(ValueError, "role disabled"):
                field_keys.advertise_aspen(client, "repeater")
            sleep.assert_not_called()


if __name__ == "__main__":
    unittest.main()
