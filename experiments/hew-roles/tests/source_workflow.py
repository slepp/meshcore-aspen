#!/usr/bin/env python3
"""Use the operator source installer against a real isolated Willow worker."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import time
import unittest

from parity import ROOT
from service_demo import RunningService
sys.path.insert(0, str(ROOT.parents[1]))
from tools.hardware import admin, lua_sources

CLI = ROOT.parents[1] / "tools/hardware/admin.py"


class SourceWorkflow(unittest.TestCase):
    def test_named_lua_wasm_conflict_rollback_and_restart(self):
        with RunningService() as service:
            endpoint = service.root / "admin.sock"
            deadline = time.monotonic() + 10
            while not endpoint.exists() and time.monotonic() < deadline:
                time.sleep(.02)
            client = admin.WillowClient(endpoint)
            keys = {path.name: path.read_bytes() for path in service.root.glob("*.seed")}
            bot_key = client.command("bot key")
            baseline_tx = len(service.emulator.all_submissions)

            def cli(*arguments, runtime="lua"):
                result = subprocess.run([sys.executable, "-B", str(CLI), "--willow-socket", str(endpoint),
                    "--runtime", runtime, *map(str, arguments)], text=True, capture_output=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return result.stdout

            def source(name, value):
                path = service.root / (name + ".lua")
                path.write_text(f"function {name}() return '{value}' end\ncommand('{name}','','Read fixture')\n")
                return path

            one = source("op_first", "one")
            other = source("op_other", "retained")
            cli("source", "install", "first", one)
            cli("source", "install", "other", other)
            first_generation = admin.download(client)
            parts = lua_sources.decode(first_generation)
            self.assertIsNone(parts["main"])
            self.assertEqual(parts["first"], one.read_bytes())
            self.assertEqual(parts["other"], other.read_bytes())
            self.assertIn("main: bundled commands", cli("source", "list"))
            one = source("op_first", "two")
            cli("source", "install", "first", one)
            selected = admin.download(client)
            parts = lua_sources.decode(selected)
            self.assertIsNone(parts["main"])
            self.assertEqual(parts["first"], one.read_bytes())
            self.assertEqual(parts["other"], other.read_bytes())
            exported = service.root / "first-export.lua"
            cli("source", "export", "first", exported)
            self.assertEqual(exported.read_bytes(), one.read_bytes())
            self.assertEqual(exported.stat().st_mode & 0o777, 0o600)

            # A stale writer can stage bytes but cannot replace the selected generation.
            candidate = lua_sources.encode(parts | {"first": b"function op_first() return 'stale' end\ncommand('op_first','','Read fixture')\n"})
            with self.assertRaisesRegex(ValueError, "sources changed"):
                admin.install(client, candidate, progress=lambda _: None,
                              expected_base_hash=hashlib.sha256(first_generation).hexdigest())
            self.assertEqual(admin.download(client), selected)
            admin.checked(client, "source cancel")
            cli("source", "rollback")
            self.assertEqual(admin.download(client), first_generation)

            module = (ROOT / "build/arithmetic.wasm").read_bytes()
            wasm_file = service.root / "arithmetic.wasm"
            wasm_file.write_bytes(module)
            cli("install", wasm_file)
            wasm = admin.runtime_client(client, "wamr-2.4.1")
            self.assertEqual(admin.download(wasm), module)
            self.assertEqual(admin.download(client), first_generation)
            second_module = module + b"\x00\x02\x01x"  # Valid inert custom section gives rollback a distinct generation.
            wasm_file.write_bytes(second_module)
            cli("install", wasm_file)
            self.assertEqual(admin.download(wasm), second_module)
            wasm_export = service.root / "wasm-export.wasm"
            cli("download", wasm_export, runtime="wasm")
            self.assertEqual(wasm_export.read_bytes(), second_module)
            cli("rollback", runtime="wasm")
            self.assertEqual(admin.download(wasm), module)
            self.assertEqual(admin.download(client), first_generation)
            self.assertEqual(client.command("bot key"), bot_key)
            self.assertEqual(len(service.emulator.all_submissions), baseline_tx,
                             "Source administration must not submit radio packets")
            service.stop_process()
            service.start_process(True)
            self.assertEqual(admin.download(client), first_generation)
            self.assertEqual(admin.download(wasm), module)
            self.assertEqual(client.command("bot key"), bot_key)
            self.assertEqual({path.name: path.read_bytes() for path in service.root.glob("*.seed")}, keys)
            print("WILLOW_SOURCE_WORKFLOW_OK named Lua/bundled retention/CAS refusal/Wasm/export/rollback/restart; no source-operation TX")


if __name__ == "__main__":
    unittest.main()
