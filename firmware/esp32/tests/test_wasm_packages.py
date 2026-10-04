# SPDX-License-Identifier: Apache-2.0
import unittest
from unittest.mock import patch
import bot_packages
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.hardware import admin as mast_cli


class WasmPackageTests(unittest.TestCase):
    def package(self):
        return bot_packages.create(b"\0asm\1\0\0\0", "wasm-example", "1.0.0", ["kv"])

    def test_portable_module_metadata_and_signing(self):
        package = self.package()
        self.assertEqual(package.runtime, "wamr-2.4.1")
        self.assertEqual(package.api, "meshcore-v1")
        self.assertEqual(bot_packages.inspect(package.source), package)
        private = bytes(range(32))
        signature = bot_packages.sign(package.source, private)
        public = bot_packages._private_key(private).public_key().public_bytes(
            bot_packages.serialization.Encoding.Raw, bot_packages.serialization.PublicFormat.Raw)
        self.assertEqual(bot_packages.verify(package.source, signature, public), package.digest)
        with self.assertRaises(ValueError):
            bot_packages.create(b"\0aotbinary", "bad", "1.0.0")
        with self.assertRaises(ValueError):
            bot_packages.create(b"\0asm\1\0\0\0", "bad", "1.0.0", ["modules"])

    def test_capability_checked_before_any_bulk_transfer(self):
        class Client:
            def __init__(self):
                self.commands = []

            def command(self, text):
                self.commands.append(text)
                return {
                    "source wasm api": "API meshcore-v1 runtime=wamr-2.4.1 source-bytes=4096",
                    "source wasm api package": "Package runtime=wamr-2.4.1 api=meshcore-v1 caps=cmdmeta,events,timers",
                }[text]
        client = Client()
        with self.assertRaisesRegex(ValueError, "required capabilities"):
            mast_cli.package_install(client, self.package().source)
        self.assertEqual(client.commands, ["source wasm api", "source wasm api package"])

    def test_selector_preserves_native_commands_and_hash(self):
        class Client:
            def __init__(self):
                self.commands = []

            def command(self, text):
                self.commands.append(text)
                return "ok"
        client = Client()
        selected = mast_cli.RuntimeClient(client, bot_packages.WASM_RUNTIME)
        selected.command("source rollback")
        selected.command("bot status")
        selected.command("data status")
        self.assertEqual(client.commands, ["source wasm rollback", "bot status", "data status"])

    def test_fetch_requires_selected_runtime_specific_contract(self):
        digest = "ab" * 32
        calls = []
        class Client:
            def command(self, text):
                calls.append(text)
                if text.endswith("api fetch"):
                    return ("Package GET runtime=wamr-2.4.1 alias=package raw=1..4096 "
                            "type=application/wasm|application/octet-stream SHA256=source")
                if text == "source wasm hash":
                    generation = 2 if any(" fetch package " in c for c in calls) else 1
                    return f"SHA256 {digest} gen={generation}"
                if text == "source wasm fetch package " + digest:
                    return "Accepted package fetch"
                if text == "source wasm status":
                    return "gen=2 active=0; source durably saved and active"
                if text == "source wasm metadata":
                    return "META name=wasm-fixture ver=1.0.0 schema=none rollback=none"
                raise AssertionError(text)
        with self.assertRaisesRegex(ValueError, "native streaming package fetch"):
            mast_cli.fetch_package(Client(), "package", digest)
        self.assertEqual(calls, ["source api fetch"], "Lua fetch must not accept a Wasm-only contract")
        calls.clear()
        selected = mast_cli.RuntimeClient(Client(), bot_packages.WASM_RUNTIME)
        with patch("builtins.print"), patch("tools.hardware.admin.time.sleep"):
            mast_cli.fetch_package(selected, "package", digest)
        self.assertIn("source wasm fetch package " + digest, calls)
        self.assertNotIn("source fetch package " + digest, calls)

    def test_raw_module_checks_runtime_before_upload(self):
        calls = []
        class Client:
            def command(self, text):
                calls.append(text)
                return "Error: Wasm runtime unavailable in this build"
        with self.assertRaisesRegex(ValueError, "Wasm runtime unavailable"):
            mast_cli.install(Client(), b"\0asm\1\0\0\0")
        self.assertEqual(calls, ["source wasm api"])
        with self.assertRaisesRegex(ValueError, "Selected Wasm runtime"):
            mast_cli.install(mast_cli.RuntimeClient(Client(), bot_packages.WASM_RUNTIME),
                             b"function lua() end")


if __name__ == "__main__":
    unittest.main()
