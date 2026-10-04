# SPDX-License-Identifier: Apache-2.0
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
RUNNER = Path(os.environ["BOT_HOST_RUNNER"])


class BotHostRunnerTests(unittest.TestCase):
    def test_native_package_validation_and_generation_restart(self):
        package = ROOT / "firmware/runtime/plugins/examples/notes-v1.lua"
        update = ROOT / "firmware/runtime/plugins/examples/notes-v2.lua"
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp") as directory:
            replay = Path(directory) / "notes.replay"
            replay.write_text(
                "!note-save original\n"
                f"@install {update}\n"
                "!note-read\n"
                "@restart\n"
                "!note-read\n"
                "@status\n"
            )
            validated = subprocess.run(
                [str(RUNNER), "--validate", str(package)],
                capture_output=True, text=True, check=True,
            )
            result = subprocess.run(
                [str(RUNNER), "--source", str(package), "--replay", str(replay)],
                capture_output=True, text=True, check=True,
            )
        self.assertIn("supervisor=BotWorker command=CommandBot", validated.stdout)
        self.assertIn("REPLY !note-save original => Saved", result.stdout)
        self.assertIn("INSTALLED complete source generation", result.stdout)
        self.assertIn("REPLY !note-read => original", result.stdout)
        self.assertIn("RESTART generation=", result.stdout)
        self.assertIn("STATUS generation=", result.stdout)

    def test_native_store_failure_duplicate_and_capacity_replay(self):
        source = (
            "function save(key) local ok,state=kv.put(key,'v') "
            "if not ok then return state end return 'saved' end "
            "command('save','key:string:32','Save one value','save') "
            "function read(key) return kv.get(key) or 'empty' end "
            "command('read','key:string:32','Read one value','read')"
        )
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp") as directory:
            package = Path(directory) / "store.lua"
            package.write_text(source)
            replay = Path(directory) / "store.replay"
            replay.write_text(
                "!save key0\n"
                "@fault commit\n"
                "!save uncertain\n"
                "@fault off\n"
                "@fault storage\n"
                "!read key0\n"
                "@fault off\n"
                "@duplicate\n"
                "!save key1\n"
                "!save key2\n"
                "!save key3\n"
                "!save key4\n"
                "!save key5\n"
                "!save key6\n"
                "!save key7\n"
                "!save key8\n"
                "@status\n"
                "!read key0\n"
            )
            result = subprocess.run(
                [str(RUNNER), "--source", str(package), "--replay", str(replay)],
                capture_output=True, text=True, check=True,
            )
        self.assertIn("REPLY !save key0 => saved", result.stdout)
        self.assertIn("REPLY !save uncertain =>", result.stdout)
        self.assertNotIn("REPLY !save uncertain => saved", result.stdout)
        self.assertIn("REPLY !read key0 => Error:", result.stdout)
        self.assertIn("REPLY !save key1 => saved", result.stdout)
        self.assertIn("REPLY !save key7 => saved", result.stdout)
        self.assertIn("REPLY !save key8 => Error:", result.stdout)
        self.assertIn("capacity", result.stdout.lower())
        self.assertIn("duplicates=1", result.stdout)
        self.assertIn("REPLY !read key0 => v", result.stdout)

    def test_versioned_jsonl_validation_and_native_lifecycle_are_deterministic(self):
        package = ROOT / "firmware/runtime/plugins/examples/notes-v1.lua"
        update = ROOT / "firmware/runtime/plugins/examples/notes-v2.lua"
        source = package.read_bytes()
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp") as directory:
            replay = Path(directory) / "notes-jsonl.replay"
            replay.write_text(
                "!note-save original\n"
                f"@install {update}\n"
                "!note-read\n"
                "@restart\n"
                f"@install {update}\n"
                "!note-read\n"
                "@duplicate\n"
                "!note-read\n"
                "@fault commit\n"
                "!note-save uncertain\n"
                "@fault off\n"
                "@status\n"
            )
            validated = subprocess.run(
                [str(RUNNER), "--format", "jsonl", "--validate", str(package)],
                capture_output=True, text=True,
            )
            command = [
                str(RUNNER), "--format", "jsonl", "--source", str(package),
                "--replay", str(replay),
            ]
            first = subprocess.run(command, capture_output=True, text=True)
            second = subprocess.run(command, capture_output=True, text=True)
            missing = subprocess.run(
                [str(RUNNER), "--format", "jsonl", "--validate",
                 str(Path(directory) / "missing.lua")],
                capture_output=True, text=True,
            )

        self.assertEqual(validated.returncode, 0, validated.stderr)
        self.assertEqual(validated.stderr, "")
        self.assertEqual(validated.stdout, json.dumps({
            "format": "meshcore-bot-replay-v1",
            "type": "validated",
            "runtime": "lua-5.5.1",
            "api": "named-commands-v1",
            "supervisor": "BotWorker",
            "command": "CommandBot",
            "source_bytes": len(source),
            "sha256": hashlib.sha256(source).hexdigest(),
        }, separators=(",", ":")) + "\n")

        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(first.stderr, "")
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(second.stdout, first.stdout)
        records = [json.loads(line) for line in first.stdout.splitlines()]
        self.assertTrue(all(record["format"] == "meshcore-bot-replay-v1" for record in records))
        self.assertEqual([record["type"] for record in records], [
            "source", "reply", "install", "reply", "restart", "install", "reply",
            "reply", "fault", "reply", "fault", "status",
        ])
        self.assertEqual(records[1]["text"], "Saved")
        self.assertTrue(records[2]["ok"])
        self.assertEqual(records[3]["text"], "original")
        self.assertEqual(records[4]["source"], "journal")
        self.assertIn("source durably saved and active", records[4]["deployment"])
        self.assertTrue(records[5]["ok"])
        self.assertEqual(records[6]["text"], "original")
        self.assertTrue(records[7]["duplicate"])
        self.assertEqual(records[8]["fault"], "commit")
        self.assertEqual(records[10]["fault"], "off")
        self.assertEqual(records[11]["duplicates"], 1)

        self.assertEqual(missing.returncode, 2)
        self.assertEqual(missing.stdout, "")
        self.assertEqual(missing.stderr, (
            '{"format":"meshcore-bot-replay-v1","type":"error",'
            '"code":"source_read","message":"cannot open package"}\n'
        ))

    def test_jsonl_replay_uses_native_timer_storage_and_clock(self):
        source = (
            "function arm(name) local result=timer.set(name,1) "
            "return result.state..':'..result.deadline_utc end "
            "function timer_status(name) local result=timer.get(name) "
            "return result.state..':'..result.deadline_utc end "
            "command('arm','name:string:32','Arm a timer','arm') "
            "command('timer_status','name:string:32','Read timer state','timer_status')"
        )
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp") as directory:
            package = Path(directory) / "timers.lua"
            package.write_text(source)
            replay = Path(directory) / "timers.replay"
            replay.write_text(
                "!arm wake\r\n"
                "@clock 1767225605\r\n"
                "@advance 1000\r\n"
                "!timer_status wake\r\n"
                "@clock 1\r\n"
                "@advance 4294967295\r\n"
                "@advance 4294967296\r\n"
            )
            result = subprocess.run(
                [str(RUNNER), "--format", "jsonl", "--source", str(package),
                 "--replay", str(replay)],
                capture_output=True, text=True,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([record["type"] for record in records],
                         ["source", "reply", "clock", "advance", "reply", "clock",
                          "reject", "reject"])
        self.assertTrue(records[1]["text"].startswith("pending:"))
        self.assertEqual(records[2]["epoch"], 1767225605)
        self.assertTrue(records[2]["accepted"])
        self.assertTrue(records[2]["trusted"])
        self.assertEqual(records[3]["milliseconds"], 1000)
        self.assertEqual(records[4]["text"], records[1]["text"])
        self.assertFalse(records[5]["accepted"])
        self.assertFalse(records[5]["trusted"])
        self.assertEqual(records[6]["code"], "invalid_advance")
        self.assertEqual(records[7]["code"], "invalid_advance")

    def test_jsonl_replay_rejects_three_byte_trace_route(self):
        package = ROOT / "firmware/runtime/plugins/examples/notes-v1.lua"
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp") as directory:
            replay = Path(directory) / "trace.replay"
            replay.write_text("!trace 3:aabbcc\n@status\n")
            result = subprocess.run(
                [str(RUNNER), "--format", "jsonl", "--source", str(package),
                 "--replay", str(replay)],
                capture_output=True, text=True,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([record["type"] for record in records],
                         ["source", "reject", "status"])
        self.assertEqual(records[1], {
            "format": "meshcore-bot-replay-v1",
            "type": "reject",
            "code": "invalid_native_command",
            "command": "!trace 3:aabbcc",
            "message": "Use !trace width:hex (width 1,2,4,8)",
        })
        self.assertEqual(records[2]["replies"], 0)
        self.assertEqual(records[2]["vm_failures"], 0)


if __name__ == "__main__":
    unittest.main()
