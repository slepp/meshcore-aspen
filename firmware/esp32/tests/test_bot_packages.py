# SPDX-License-Identifier: Apache-2.0
import hashlib
import tempfile
import unittest
from unittest.mock import patch
from cryptography.hazmat.primitives import serialization

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "runtime"))
import bot_packages
from tools.hardware import admin as mast_cli

ROOT = Path(__file__).resolve().parents[3]


class PackageTests(unittest.TestCase):
    def test_configured_fetch_rejects_invalid_alias_and_hash_before_device_access(self):
        class Client:
            def command(self, command):
                raise AssertionError(f"Device must not be contacted for invalid input: {command}")

        for alias, digest, message in (
            ("../package", "ab" * 32, "endpoint alias"),
            ("package", "AB" * 32, "lowercase expected SHA256"),
        ):
            with self.subTest(alias=alias, digest=digest), self.assertRaisesRegex(ValueError, message):
                mast_cli.fetch_package(Client(), alias, digest)

    def package(self, version="1.0.0", schema="notes@1", rollback="none", capabilities=("kv", "kv.atomic")):
        return bot_packages.create(
            b"function ping() reply('Pong') end\n",
            "notes", version, capabilities, schema, rollback,
        )

    def test_plain_lua_package_round_trip_and_metadata_validation(self):
        package = self.package()
        self.assertTrue(package.source.startswith(bot_packages.PREFIX))
        self.assertTrue(package.source.endswith(b"function ping() reply('Pong') end\n"))
        inspected = bot_packages.inspect(package.source)
        self.assertEqual(inspected.name, "notes")
        self.assertEqual(inspected.capabilities, ("kv", "kv.atomic"))
        self.assertEqual(inspected.digest, hashlib.sha256(package.source).hexdigest())
        for invalid in (
            package.source.replace(b"runtime=lua-5.5.1", b"runtime=lua-5.4.7"),
            package.source.replace(b"api=named-commands-v1", b"api=other"),
            package.source.replace(b"caps=kv,kv.atomic", b"caps=owner"),
            package.source.replace(b"schema=notes@1", b"schema=notes@0"),
            package.source.replace(b"rollback=none", b"rollback=notes@1;extra=x"),
            package.source.split(b"\n", 1)[0],
            b"\x1b" + package.source[1:],
        ):
            with self.subTest(invalid=invalid[:40]), self.assertRaises(ValueError):
                bot_packages.inspect(invalid)

    def test_package_size_and_source_boundaries(self):
        with self.assertRaisesRegex(ValueError, "envelope limit"):
            bot_packages.create(b"--" + b"x" * 4088, "large", "1.0.0")
        with self.assertRaisesRegex(ValueError, "already a packaged"):
            bot_packages.create(self.package().source, "nested", "1.0.0")
        with self.assertRaisesRegex(ValueError, "NUL"):
            bot_packages.create(b"function bad()\0 end", "bad", "1.0.0")

    def test_version_length_matches_firmware_metadata_field(self):
        supported = "1234567890123456789.1.1"
        self.assertEqual(self.package(version=supported).version, supported)

        too_long = "12345678901234567890.1.1"
        with self.assertRaisesRegex(ValueError, "23 characters"):
            self.package(version=too_long)
        malformed = self.package().source.replace(
            b"version=1.0.0", ("version=" + too_long).encode("ascii"))
        with self.assertRaisesRegex(ValueError, "23 characters"):
            bot_packages.inspect(malformed)

    def test_schema_compatibility_must_preserve_previous_format(self):
        old = self.package()
        unchanged = bot_packages.create(b"function ping() end", "notes", "1.1.0", ("kv",),
                                        "notes@1", "none")
        bot_packages.validate_transition(old, unchanged)
        compatible = bot_packages.create(b"function ping() end", "notes", "2.0.0", ("kv",),
                                         "notes@2", "notes@1")
        bot_packages.validate_transition(old, compatible)
        incompatible = bot_packages.create(b"function ping() end", "notes", "2.0.0", ("kv",),
                                            "notes@2", "none")
        with self.assertRaisesRegex(ValueError, "must declare rollback=notes@1"):
            bot_packages.validate_transition(old, incompatible)

    def test_optional_detached_signatures_are_not_required_for_packages(self):
        package = self.package()
        signing_key = bytes(range(32))
        signature = bot_packages.sign(package.source, signing_key)
        public = bot_packages._private_key(signing_key).public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        self.assertEqual(bot_packages.verify(package.source, signature, public), package.digest)
        with self.assertRaisesRegex(ValueError, "different package SHA256"):
            bot_packages.verify(package.source + b"--tampered", signature, public)
        other = bot_packages._private_key(bytes(reversed(range(32)))).public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        with self.assertRaisesRegex(ValueError, "differs from the selected key"):
            bot_packages.verify(package.source, signature, other)

    def test_cli_creates_and_inspects_package_without_device_access(self):
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp") as directory:
            source = Path(directory) / "hello.lua"
            package_path = Path(directory) / "hello.bot.lua"
            source.write_bytes(b"function hello(name) reply('Hello '..name) end\n")
            with patch.object(sys, "argv", ["mast_cli.py", "package", "create", str(source),
                                             str(package_path), "--name", "hello", "--version", "1.0.0"]):
                mast_cli.main()
            self.assertEqual(bot_packages.inspect(package_path.read_bytes()).name, "hello")
            with patch.object(sys, "argv", ["mast_cli.py", "package", "inspect", str(package_path)]):
                mast_cli.main()

    def test_package_install_checks_device_contract_before_transfer(self):
        package = self.package(schema="none",
                               capabilities=("cmdmeta", "kv", "kv.atomic", "mesh-chan", "mesh-dest"))
        digest = package.digest
        state = {"active": False}
        calls = []

        class Client:
            def command(self, command):
                calls.append(command)
                if command == "source api":
                    return "API named-commands-v1 lua=5.5.1 cmdmeta=1 mesh=dm+,wait=ack/text/chan/trace,advert"
                if command == "source api package":
                    return ("Package api=named-commands-v1 caps=cmdmeta,events,https,kv,kv.atomic,"
                            "mesh,mesh-chan,mesh-dest,modules,reminders,timers,utilities")
                if command == "source metadata":
                    return "BUNDLED schema=none"
                if command.startswith("source begin "):
                    return "ACK " + command.split()[2] + " next=0"
                if command.startswith("source chunk "):
                    parts = command.split()
                    return f"ACK {parts[2]} next={int(parts[3]) + 1}"
                if command.startswith("source commit "):
                    state["active"] = True
                    return "Accepted"
                if command == "source status":
                    return "source durably saved and active"
                if command == "source hash":
                    return f"SHA256 {digest} gen=2" if state["active"] else "SHA256 " + "0" * 64 + " gen=1"
                return "OK"

        client = Client()
        with patch.object(mast_cli.time, "sleep"):
            result = mast_cli.package_install(client, package.source, progress=lambda message: None)
        self.assertIn("durably saved and active", result)
        self.assertLess(calls.index("source api"), next(i for i, call in enumerate(calls) if call.startswith("source begin ")))
        self.assertLess(calls.index("source metadata"), next(i for i, call in enumerate(calls) if call.startswith("source begin ")))

    def test_device_capability_and_schema_mismatch_prevent_source_transfer(self):
        package = self.package(schema="notes@2", rollback="none")
        calls = []

        class Client:
            def command(self, command):
                calls.append(command)
                if command == "source api":
                    return "API named-commands-v1 lua=5.5.1"
                if command == "source api package":
                    return "Package runtime=lua-5.5.1 api=named-commands-v1 caps=events,kv,kv.atomic,mesh,modules,reminders,timers,utilities"
                if command == "source metadata":
                    return "META name=notes ver=1.0.0 schema=notes@1 rollback=none"
                return "OK"

        with self.assertRaisesRegex(ValueError, "rollback=notes@1"):
            mast_cli.package_install(Client(), package.source, progress=lambda _: None)
        self.assertFalse(any(command.startswith("source begin ") for command in calls))

    def test_unavailable_device_capability_prevents_transfer(self):
        package = self.package(schema="none", capabilities=("https",))
        calls = []

        class Client:
            def command(self, command):
                calls.append(command)
                if command == "source api":
                    return "API named-commands-v1 lua=5.5.1 commands=8"
                if command == "source api package":
                    return "Package runtime=lua-5.5.1 api=named-commands-v1 caps=events,kv,mesh,modules,reminders,timers,utilities"
                return "OK"

        with self.assertRaisesRegex(ValueError, "required capabilities: https"):
            mast_cli.package_install(Client(), package.source, progress=lambda _: None)
        self.assertFalse(any(command == "source metadata" or command.startswith("source begin ")
                             for command in calls))

    def test_configured_host_fetch_requires_streaming_contract_and_verifies_expected_hash(self):
        expected = "ab" * 32
        calls = []
        fetch_command = "source fetch package " + expected

        class Client:
            def command(self, command):
                calls.append(command)
                if command == "source api fetch":
                    return mast_cli.PACKAGE_FETCH_API
                if command == "source hash":
                    return f"SHA256 {expected} gen=2" if fetch_command in calls else "SHA256 " + "00" * 32 + " gen=1"
                if command.startswith("source fetch "):
                    return "Accepted configured-host fetch"
                if command == "source status":
                    return "gen=2 active=1 prev=3 durable; durably saved and active"
                if command == "source metadata":
                    return "META name=notes ver=1.0.0 schema=none rollback=none"
                return "OK"

        with patch("tools.hardware.admin.time.sleep"), patch("builtins.print"):
            result = mast_cli.fetch_package(Client(), "package", expected)
        self.assertIn("SHA256 " + expected + " gen=2", result)
        self.assertIn(fetch_command, calls)


if __name__ == "__main__":
    unittest.main()
