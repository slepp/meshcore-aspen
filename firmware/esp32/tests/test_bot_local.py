import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
ONCHIP = ROOT / "firmware/esp32"
RUNNER = Path(os.environ["BOT_HOST_RUNNER"])
RUNTIME = ROOT / "firmware/runtime"
EXAMPLES = RUNTIME / "plugins/examples"


class LocalLuaTests(unittest.TestCase):
    def local(self, source, scenario):
        args = ["python3", str(RUNTIME / "bot_local.py"), "--runner", str(RUNNER)]
        args += ["--source", str(source)] if source else ["--bundled"]
        return subprocess.run(
            args + ["--scenario", str(scenario)], cwd=ROOT,
            capture_output=True, text=True,
        )

    def test_fresh_package_and_failed_assertions(self):
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp") as directory:
            package = Path(directory) / "hello.bot.lua"
            subprocess.run([
                "python3", str(Path(__file__).resolve().parents[3] / "tools/hardware/admin.py"), "package", "create",
                str(RUNTIME / "plugins/template.lua"), str(package),
                "--name", "hello", "--version", "1.0.0", "--capability", "cmdmeta",
            ], check=True, capture_output=True, text=True)
            success = self.local(package, EXAMPLES / "hello.scenario.json")
            self.assertEqual(success.returncode, 0, success.stderr + success.stdout)
            scenario = json.loads((EXAMPLES / "hello.scenario.json").read_text())
            scenario["expect"][0]["text"] = "not a real reply"
            failing = Path(directory) / "failing.json"
            failing.write_text(json.dumps(scenario))
            failure = self.local(package, failing)
            self.assertEqual(failure.returncode, 1)
            self.assertIn("ASSERT 1 failed", failure.stderr)
            failing.write_text('{"replay":[],"expect":[]}')
            empty = self.local(package, failing)
            self.assertEqual(empty.returncode, 0, empty.stderr)
            failing.write_text('{"replay":["@not-supported"],"expect":[]}')
            unexpected = self.local(package, failing)
            self.assertEqual(unexpected.returncode, 1)
            failing.write_text("{}")
            malformed = self.local(package, failing)
            self.assertEqual(malformed.returncode, 2)
            missing = self.local(Path(directory) / "missing.lua", EXAMPLES / "hello.scenario.json")
            self.assertEqual(missing.returncode, 1)
            self.assertIn("source_read", missing.stderr)

    def test_scoped_storage_and_async_scenarios(self):
        for name, source in [
            ("private", EXAMPLES / "lab.lua"),
            ("board", None),
            ("timers", EXAMPLES / "lab.lua"),
            ("network", None),
            ("http", RUNTIME / "examples/network_api.lua"),
            ("reminders", None),
            ("notes", EXAMPLES / "notes-v2.lua"),
            ("weather", None),
        ]:
            with self.subTest(name=name):
                result = self.local(source, EXAMPLES / f"{name}.scenario.json")
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)

    def test_owner_package_native_authority_and_journal_restart(self):
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp") as directory:
            package = Path(directory) / "owner.bot.lua"
            subprocess.run([
                "python3", str(Path(__file__).resolve().parents[3] / "tools/hardware/admin.py"), "package", "create",
                str(EXAMPLES / "owner.lua"), str(package),
                "--name", "owner-lab", "--version", "1.0.0",
                "--capability", "cmdmeta", "--capability", "kv",
            ], check=True, capture_output=True, text=True)
            result = self.local(package, EXAMPLES / "owner.scenario.json")
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        records = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        owner = next(r for r in records if r["type"] == "owner")
        contexts = [r for r in records if r["type"] == "context"]
        self.assertEqual(len(owner["public_key"]), 64)
        self.assertEqual(contexts[0]["public_key"], owner["public_key"])
        self.assertNotEqual(contexts[1]["public_key"], owner["public_key"])
        self.assertFalse(contexts[2]["authenticated"])
        self.assertFalse(contexts[2]["owner"])
        self.assertEqual(
            [r["text"] for r in records if r["type"] == "reply" and r["command"] == "!owner-count"],
            ["Owner calls 1", "Owner calls 0", "Owner calls 1", "Owner calls 2", "Owner calls 1"],
        )

    def test_native_journal_update_failure_cancel_rollback_and_remove(self):
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp") as directory:
            first, second, broken = [Path(directory) / name for name in ["first.lua", "second.lua", "broken.lua"]]
            first.write_text("function lab() return 'first' end command('lab','','Lab','lab')")
            second.write_text("function lab() return 'second' end command('lab','','Lab','lab')")
            broken.write_text("this is not valid lua")
            scenario = Path(directory) / "journal.json"
            scenario.write_text(json.dumps({
                "replay": [
                    "@source status", "@source hash", "!lab", f"@install {broken}", "!lab",
                    "@restart", "!lab", "@source cancel", f"@install {second}", "!lab",
                    "@restart", "!lab", "@source rollback", "@pump 800", "!lab",
                    "@restart", "!lab", "@source retry", "@pump 800", "@source remove", "@pump 800",
                    "@restart", "!ping",
                ],
                "expect": [
                    {"type": "source_control", "command": "status", "ok": True, "text_contains": "active=0"},
                    {"type": "source_control", "command": "hash", "ok": True,
                     "text_contains": hashlib.sha256(first.read_bytes()).hexdigest()},
                    {"type": "reply", "text": "first"},
                    {"type": "install", "ok": False},
                    {"type": "reply", "text": "first"},
                    {"type": "restart", "source": "journal"},
                    {"type": "reply", "text": "first"},
                    {"type": "source_control", "command": "cancel", "ok": True, "text_contains": "cancelled"},
                    {"type": "install", "ok": True},
                    {"type": "reply", "text": "second"},
                    {"type": "restart", "source": "journal"},
                    {"type": "reply", "text": "second"},
                    {"type": "source_control", "command": "rollback", "ok": True},
                    {"type": "reply", "text": "first"},
                    {"type": "restart", "source": "journal"},
                    {"type": "reply", "text": "first"},
                    {"type": "source_control", "command": "retry", "ok": True},
                    {"type": "source_control", "command": "remove", "ok": True},
                    {"type": "restart", "source": "bundled"},
                    {"type": "reply", "text": "Pong"},
                ],
            }))
            result = self.local(first, scenario)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        failed = next(r for r in records if r["type"] == "install" and not r["ok"])
        self.assertIn("Error:", failed["message"])

    def test_native_event_grant_and_budget_rejection_are_observable(self):
        source = (
            "function observed() if ctx.message=='hello' then "
            "kv.put('seen',ctx.message,'bot') end end "
            "events.on('message','observed') "
            "function read_seen() return kv.get('seen','bot') or 'empty' end "
            "command('seen','','Read event state','read_seen','dm')"
        )
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp") as directory:
            package = Path(directory) / "events.lua"
            package.write_text(source)
            replay = (
                "@grant shared on\n"
                "@message dm alice hello\n!seen\n"
                "@grant events on\n@pump 800\n@message dm alice hello\n!seen\n"
                "@grant events off\n"
            )
            result = subprocess.run(
                [str(RUNNER), "--format", "jsonl", "--source", str(package)],
                input=replay, capture_output=True, text=True,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([r["text"] for r in records if r["type"] == "reply"], ["empty", "hello"])
        self.assertTrue(all(r["ok"] for r in records if r["type"] == "grant"))
        burst = subprocess.run(
            [str(RUNNER), "--format", "jsonl", "--bundled"],
            input="!ping\n" * 30 + "@status\n", capture_output=True, text=True,
        )
        self.assertEqual(burst.returncode, 0, burst.stderr)
        records = [json.loads(line) for line in burst.stdout.splitlines()]
        self.assertTrue(any(r.get("code") == "native_admission" for r in records))
        self.assertGreater(records[-1]["rejected"], 0)
        self.assertFalse(any(r["type"] == "pending" for r in records))

    def test_builtin_override_native_journal_and_identity_lifetime(self):
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp") as directory:
            second, broken = [Path(directory) / name for name in ["second.lua", "broken.lua"]]
            second.write_text(
                "function patched() return call_original('ping')..' v2' end "
                "override_command('ping','patched')"
            )
            broken.write_text("override_command('ping','missing')")
            scenario = Path(directory) / "override-journal.json"
            scenario.write_text(json.dumps({
                "replay": [
                    "@context dm alice", "!about", "!ping", "!remember camp tea", "!recall camp",
                    "@context dm bob", "!recall camp", "@context dm alice",
                    "@restart", "!ping", "!recall camp", f"@install {broken}", "!ping",
                    "@source cancel", f"@install {second}", "!ping", "@restart", "!ping",
                    "@source rollback", "@pump 800", "!ping", "!recall camp",
                    "@source retry", "@pump 800", "!ping",
                    "@source remove", "@pump 800", "@restart", "!ping", "!recall camp", "!about",
                ],
                "expect": [
                    {"type": "reply", "text_contains": "Mesh command bot. !help"},
                    {"type": "reply", "text": "Pong from the regional bot"},
                    {"type": "reply", "text": "Note camp created; committed"},
                    {"type": "reply", "text": "tea"},
                    {"type": "reply", "text": "No note: camp"},
                    {"type": "restart", "source": "journal"},
                    {"type": "reply", "text": "Pong from the regional bot"},
                    {"type": "reply", "text": "tea"},
                    {"type": "install", "ok": False},
                    {"type": "reply", "text": "Pong from the regional bot"},
                    {"type": "source_control", "command": "cancel", "ok": True},
                    {"type": "install", "ok": True},
                    {"type": "reply", "text": "Pong v2"},
                    {"type": "restart", "source": "journal"},
                    {"type": "reply", "text": "Pong v2"},
                    {"type": "source_control", "command": "rollback", "ok": True},
                    {"type": "reply", "text": "Pong from the regional bot"},
                    {"type": "reply", "text": "tea"},
                    {"type": "source_control", "command": "retry", "ok": True},
                    {"type": "reply", "text": "Pong from the regional bot"},
                    {"type": "source_control", "command": "remove", "ok": True},
                    {"type": "restart", "source": "bundled"},
                    {"type": "reply", "text": "Pong"},
                    {"type": "reply", "text": "tea"},
                    {"type": "reply", "text_contains": "Mesh command bot. !help"},
                ],
            }))
            result = self.local(EXAMPLES / "overrides.lua", scenario)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        keys = [r["text"] for r in records if r.get("command") == "!about"]
        self.assertEqual(len(keys), 2)
        self.assertEqual(keys[0], keys[1])

    def test_native_return_route_widths_and_invalid_routes(self):
        for route in ["1:", "1:aa", "2:aAbB", "3:aAbBcC"]:
            with self.subTest(route=route):
                replay = (
                    "@context dm alice\n@clock 1767225600\n@grant reminders on\n"
                    f"@route {route}\n!remind 2s route-test\n@advance 3000\n"
                    "@pump 800\n!reminders\n"
                )
                result = subprocess.run(
                    [str(RUNNER), "--format", "jsonl", "--bundled"],
                    input=replay, capture_output=True, text=True,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                records = [json.loads(line) for line in result.stdout.splitlines()]
                self.assertFalse(any(r["type"] in {"reject", "error"} for r in records))
                self.assertTrue(any(
                    r["type"] == "async_reply" and "route-test" in r["text"] for r in records
                ), records)
                self.assertTrue(records[-1]["text"].startswith("1 sent @"), records[-1])
        invalid = [
            "0:aa", "4:aabbccdd", "8:aabbccddeeff0011", "1:a", "1:gg",
            "2:aa", "3:aabb", "1:" + "aa" * 64,
        ]
        result = subprocess.run(
            [str(RUNNER), "--format", "jsonl", "--bundled"],
            input="\n".join(f"@route {route}" for route in invalid),
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(
            [r["code"] for r in records if r["type"] == "reject"],
            ["invalid_route"] * len(invalid),
        )
        self.assertFalse(any(r["type"] == "route" for r in records))

    def test_shipped_utilities_notes_reminders_diagnostics_and_help(self):
        lines = [
            "!calc (2+3)*4", "!calc 1/0", "!calc",
            "!convert 32 F C", "!convert 1 m kg", "!convert 1 MIB B",
            "!roll", "!roll 0d6", "!choose tea|coffee", "!choose tea||coffee",
            "!remember plan lunch", "!remember plan dinner", "!recall plan",
            "!notes pl", "!list-memories pl", "!forget plan", "!forget plan",
            "!remind 0s tea", "@grant reminders on", "!remind 0s tea",
            "!remind 5m tea", "!reminders", "!cancel 1", "!reminders",
            "!weather", "@network setup", "@grant home on", "!weather",
            "!service unknown", "!service health extra",
            "!ping", "!test", "!path", "!about", "!version", "!uptime",
            "!status", "!signal", "!air", "!plugins", "!neighbors",
            "!admin bot status", "!unknown",
        ]
        names = [
            "ping", "test", "path", "mt", "trace", "remind", "reminders",
            "cancel", "remember", "recall", "forget", "notes", "list-memories",
            "calc", "convert", "roll", "choose", "weather", "service", "board",
            "about", "version", "uptime", "status", "signal", "air", "help",
            "plugins", "neighbors", "admin",
        ]
        lines += [f"!help {name}" for name in names] + [f"!help {n}" for n in range(1, 9)]
        spaced = []
        commands = 0
        for line in lines:
            if line.startswith("!"):
                if commands and commands % 10 == 0:
                    spaced.append("@advance 61000")
                commands += 1
            spaced.append(line)
        result = subprocess.run([str(RUNNER), "--bundled", "--format", "jsonl"],
                                input="\n".join(spaced), capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        replies = {record["command"]: record["text"] for record in records if record["type"] == "reply"}
        self.assertEqual(replies["!calc (2+3)*4"], "= 20")
        self.assertIn("Division by zero", replies["!calc 1/0"])
        self.assertIn("!calc: expression: required", replies["!calc"])
        self.assertEqual(replies["!convert 32 F C"], "32 F = 0 C")
        self.assertIn("Incompatible", replies["!convert 1 m kg"])
        self.assertIn("case-sensitive", replies["!convert 1 MIB B"])
        self.assertTrue(replies["!roll"].startswith("Roll 1d6: ["))
        self.assertTrue(replies["!roll 0d6"].startswith("Error:"))
        self.assertTrue(replies["!choose tea|coffee"].startswith("Choice "))
        self.assertTrue(replies["!choose tea||coffee"].startswith("Error:"))
        self.assertEqual(replies["!remember plan dinner"], "Note plan replaced; committed")
        self.assertEqual(replies["!recall plan"], "dinner")
        self.assertEqual(replies["!notes pl"], replies["!list-memories pl"])
        self.assertEqual(replies["!forget plan"], "No note: plan")
        self.assertIn("cancelled", replies["!cancel 1"])
        self.assertIn("no default place", replies["!weather"])
        self.assertIn("supports health", replies["!service unknown"])
        self.assertIn("takes no arguments", replies["!service health extra"])
        self.assertIn("not granted", replies["!admin bot status"])
        self.assertIn("only the node owner's DM can use administration", replies["!admin bot status"])
        self.assertEqual(replies["!unknown"], "Error: unknown command !unknown; use !help")
        self.assertEqual(replies["!ping"], "Pong")
        for name in names:
            self.assertIn(f"!help {name}", replies)
        self.assertTrue(all(len(text.encode()) <= 156 for text in replies.values()))


if __name__ == "__main__":
    unittest.main()
