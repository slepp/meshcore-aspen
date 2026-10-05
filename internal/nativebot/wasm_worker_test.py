# SPDX-License-Identifier: Apache-2.0
"""Actual host worker: independent durable Lua/Wasm journals and recovery."""
import hashlib
from pathlib import Path
import struct
import time
import unittest
import zlib
import worker_test

ROOT = Path(__file__).resolve().parents[2]


def package(module, name):
    return (f"--@meshcore-bot/1;name={name};version=1.0.0;runtime=wamr-2.4.1;"
            "api=meshcore-v1;caps=cmdmeta,kv;schema=none;rollback=none\n").encode() + module


class WasmLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.helper = worker_test.WorkerProcessTest()
        self.helper.setUp()
        self.addCleanup(self.helper.doCleanups)

    def command(self, process, runtime, text):
        return self.helper.admin(process, f"source {'wasm ' if runtime == 'wasm' else ''}{text}")

    def active(self, process, runtime, slot):
        for _ in range(100):
            status = self.command(process, runtime, "status")
            if f"active={slot}" in status and "durably saved and active" in status:
                return status
            self.assertNotIn("Error:", status)
            time.sleep(.03)
        self.fail(f"{runtime} activation did not complete: {status}")

    def upload(self, process, runtime, data):
        digest = hashlib.sha256(data).hexdigest()
        ident = digest[:16]
        self.assertEqual(self.command(process, runtime, f"begin {ident} {len(data)} {digest}"),
                         f"ACK {ident} next=0")
        for index, offset in enumerate(range(0, len(data), 48)):
            self.assertEqual(self.command(process, runtime, f"chunk {ident} {index} {data[offset:offset + 48].hex()}"),
                             f"ACK {ident} next={index + 1}")
        self.assertIn("Accepted verification", self.command(process, runtime, f"commit {ident}"))
        return digest

    def test_real_worker_runtime_independent_update_rollback_and_restart(self):
        lua = b"function custom() reply('Lua v1') end"
        next_lua = b"function custom() reply('Lua v2') end"
        arithmetic = package((ROOT / ".tmp/wasm-examples/c-arithmetic.wasm").read_bytes(), "wasm-add")
        notes = package((ROOT / ".tmp/wasm-examples/c-notes.wasm").read_bytes(), "wasm-notes")
        first = self.helper.start()
        key = self.helper.ready(first)
        self.assertIn("wamr-2.4.1/meshcore-v1", self.command(first, "lua", "api runtimes"))
        lua_hash = self.upload(first, "lua", lua)
        self.active(first, "lua", 0)
        wasm_hash = self.upload(first, "wasm", arithmetic)
        self.active(first, "wasm", 0)
        self.assertIn(lua_hash, self.command(first, "lua", "hash"))
        self.upload(first, "wasm", notes)
        self.active(first, "wasm", 1)
        self.assertIn("Accepted verification", self.command(first, "wasm", "rollback"))
        self.active(first, "wasm", 0)
        self.assertIn(lua_hash, self.command(first, "lua", "hash"))
        self.assertIn(wasm_hash, self.command(first, "wasm", "hash"))
        self.helper.stop(first)

        second = self.helper.start()
        self.assertEqual(self.helper.ready(second), key)
        self.assertIn(wasm_hash, self.command(second, "wasm", "hash"))
        self.assertIn(lua_hash, self.command(second, "lua", "hash"))
        self.upload(second, "lua", next_lua)
        self.active(second, "lua", 1)
        self.assertIn(wasm_hash, self.command(second, "wasm", "hash"))
        self.assertIn("Accepted verification", self.command(second, "lua", "rollback"))
        self.active(second, "lua", 0)
        self.assertIn("Accepted verification", self.command(second, "wasm", "remove"))
        self.active(second, "wasm", 3)
        self.assertIn(lua_hash, self.command(second, "lua", "hash"))
        self.helper.stop(second)

        third = self.helper.start()
        self.assertEqual(self.helper.ready(third), key)
        self.assertIn("active=3", self.command(third, "wasm", "status"))
        self.assertIn(lua_hash, self.command(third, "lua", "hash"))
        self.helper.stop(third)

    def test_corrupt_wasm_package_keeps_lua_and_native_management_ready(self):
        lua = b"function custom() reply('Lua retained') end"
        arithmetic = package((ROOT / ".tmp/wasm-examples/c-arithmetic.wasm").read_bytes(), "wasm-add")
        first = self.helper.start()
        key = self.helper.ready(first)
        lua_hash = self.upload(first, "lua", lua)
        self.active(first, "lua", 0)
        self.upload(first, "wasm", arithmetic)
        self.active(first, "wasm", 0)
        self.helper.stop(first)
        path = worker_test.STATE / "spiffs/spiffs.wa.lua"
        damaged = bytearray(path.read_bytes())
        # Keep the native filesystem envelope valid; damage only package metadata.
        damaged[16] = 0
        struct.pack_into("<I", damaged, 12, zlib.crc32(damaged[16:]))
        path.write_bytes(damaged)
        second = self.helper.start()
        second.stdin.write(self.helper.hello)
        second.stdin.flush()
        management = None
        for _ in range(4):
            tag, body = worker_test.receive(second.stdout, timeout=3)
            self.assertIn(tag, (0x86, 0x83), "corrupt Wasm source transmitted or became ready")
            if tag == 0x86:
                management = body
            else:
                self.assertEqual(body[:2], b"\x00\x01")
                break
        else:
            self.fail("corrupt Wasm source did not publish faulted status")
        self.assertIsNotNone(management)
        self.assertEqual(management[:32], key)
        self.assertIn(lua_hash, self.helper.admin(second, "source hash", faulted=True))
        deadline = time.monotonic() + 9
        while True:
            status = self.helper.admin(second, "source wasm status", faulted=True)
            metadata, separator, outcome = status.partition("; ")
            self.assertTrue(separator, status)
            self.assertIn(" active=0 ", metadata)
            self.assertTrue(outcome.startswith("Error: durable package runtime does not match source selector; "), status)
            if outcome == "Error: durable package runtime does not match source selector; use source wasm retry; startup blocked":
                break
            self.assertEqual(outcome, "Error: durable package runtime does not match source selector; live retry pending")
            self.assertLess(time.monotonic(), deadline, "corrupt-Wasm retries did not terminate")
            time.sleep(.05)
        self.assertTrue(self.helper.admin(second, "advert.zerohop", faulted=True).startswith("Error:"))
        self.assertEqual(self.command(second, "wasm", "remove"),
                         "Accepted verification; source status reports durable activation outcome")
        self.assertEqual(self.helper.ready(second, hello=False), key)
        self.active(second, "wasm", 3)
        self.assertIn(lua_hash, self.command(second, "lua", "hash"))
        self.helper.stop(second)

    def test_real_tls_wasm_fetch_hash_failure_cancel_and_restart(self):
        self.helper.package_fixture = package(
            (ROOT / ".tmp/wasm-examples/c-arithmetic.wasm").read_bytes(), "wasm-fetch")
        self.helper.package_mime = "application/wasm"
        admin = self.helper.admin
        def selected(process, text, request_id=17):
            if text.startswith("source "):
                text = "source wasm " + text[7:]
            return admin(process, text, request_id)
        self.helper.admin = selected
        self.helper.test_real_tls_streamed_package_hash_failure_and_cancellation_preserve_source()
        process = self.helper.start()
        self.helper.ready(process)
        self.assertIn("runtime=wamr-2.4.1", selected(process, "source api fetch"))
        self.assertIn("active=3", admin(process, "source status"), "Wasm fetch must leave Lua bundled selection")
        self.assertIn(hashlib.sha256(self.helper.package_fixture).hexdigest(),
                      selected(process, "source hash"))
        self.helper.stop(process)

    def test_selected_runtime_rejects_wrong_binary_or_text_without_replacement(self):
        lua = b"function custom() reply('Lua retained') end"
        module = (ROOT / ".tmp/wasm-examples/c-arithmetic.wasm").read_bytes()
        process = self.helper.start()
        self.helper.ready(process)
        lua_hash = self.upload(process, "lua", lua)
        self.active(process, "lua", 0)
        wasm_hash = self.upload(process, "wasm", module)
        self.active(process, "wasm", 0)
        for runtime, wrong in (("lua", module), ("wasm", lua)):
            self.upload(process, runtime, wrong)
            for _ in range(100):
                status = self.command(process, runtime, "status")
                if "package runtime does not match source selector" in status:
                    break
                time.sleep(.03)
            self.assertIn("package runtime does not match source selector", status)
            self.assertIn(lua_hash, self.command(process, "lua", "hash"))
            self.assertIn(wasm_hash, self.command(process, "wasm", "hash"))
        self.helper.stop(process)


if __name__ == "__main__":
    unittest.main()
