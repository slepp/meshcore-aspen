# SPDX-License-Identifier: Apache-2.0
import os
import json
from pathlib import Path
import subprocess
import unittest
import bot_packages

ROOT = Path(__file__).resolve().parents[3]
RUNNER = Path(os.environ["BOT_HOST_RUNNER"])
MODULES = ROOT / ".tmp/wasm-examples"


class WasmHostRunnerTests(unittest.TestCase):
    def conflict_replay(self, name, initial, sources, commands):
        state = ROOT / ".tmp" / name
        state.mkdir()
        paths = {key: state / key for key in sources}
        replay = state / "commands.replay"
        try:
            for key, data in sources.items():
                paths[key].write_bytes(data.encode() if isinstance(data, str) else data)
            paths.update({key: MODULES / key for key in ("c-notes.wasm", "rust-notes.wasm", "c-owner.wasm")})
            expanded = []
            for command in commands:
                for key, path in paths.items():
                    command = command.replace("{" + key + "}", str(path))
                expanded.append(command)
            replay.write_text("\n".join(expanded) + "\n")
            result = subprocess.run(
                [str(RUNNER), "--format", "jsonl", "--source", str(paths[initial]), "--replay", str(replay)],
                capture_output=True, text=True, check=True,
            )
            return [json.loads(line) for line in result.stdout.splitlines()]
        finally:
            for path in state.iterdir():
                path.unlink()
            state.rmdir()

    @staticmethod
    def source_snapshot():
        return ["@source status", "@source hash", "@source metadata",
                "@source wasm status", "@source wasm hash", "@source wasm metadata"]

    def assert_journals_unchanged(self, records, snapshots=3):
        for selector in ("", "wasm "):
            for operation in ("status", "hash", "metadata"):
                values = [r["text"] for r in records if r["type"] == "source_control" and
                          r["command"] == selector + operation]
                self.assertEqual(len(values), snapshots)
                if operation == "status":
                    values = [value.split(" upload=")[0] for value in values]
                self.assertEqual(values, [values[0]] * snapshots)

    def test_lua_conflict_never_publishes_or_replaces_either_retained_program(self):
        prior = ("function lkeep() local n=(kv.get('lua-count') or '')..'x' "
                 "kv.put('lua-count',n) return 'Lua '..#n end command('lkeep','','Prior Lua','lkeep')")
        bad = "function wnote() return 'HIJACK' end function lcustom() return 'REJECTED ACTIVE' end"
        records = self.conflict_replay("wasm-conflict-lua", "c-notes.wasm",
            {"prior.lua": prior, "bad.lua": bad}, [
                "@install {prior.lua}", "!wnote durable", "!lkeep", *self.source_snapshot(),
                "@status", "@install {bad.lua}", *self.source_snapshot(), "@status",
                "!wnote", "!lkeep", "!lcustom", "@restart", *self.source_snapshot(),
                "!wnote", "!lkeep", "!lcustom",
            ])
        self.assert_journals_unchanged(records)
        installs = [r for r in records if r["type"] == "install"]
        self.assertTrue(installs[0]["ok"]); self.assertFalse(installs[1]["ok"])
        self.assertIn("Command !wnote is already registered by Wasm runtime", installs[1]["message"])
        statuses = [r["generation"] for r in records if r["type"] == "status"]
        self.assertEqual(statuses[0], statuses[1])
        self.assertEqual([r["text"] for r in records if r["type"] == "reply" and r["command"] == "!lkeep"],
                         ["Lua 1", "Lua 2", "Lua 3"])
        self.assertEqual([r["text"] for r in records if r["type"] == "reply" and r["command"] == "!wnote"],
                         ["durable", "durable"])
        self.assertTrue(all("unknown command" in r["text"] for r in records
                            if r["type"] == "reply" and r["command"] == "!lcustom"))

    def test_wasm_conflict_never_publishes_or_displaces_original_wasm_after_restart(self):
        lua = ("function wnote() local n=(kv.get('lua-count') or '')..'x' "
               "kv.put('lua-count',n) return 'Lua '..#n end command('wnote','','Lua note','wnote')")
        records = self.conflict_replay("wasm-conflict-wasm", "rust-notes.wasm", {"prior.lua": lua}, [
            "@install {prior.lua}", "!rnote durable", "!wnote", *self.source_snapshot(),
            "@status", "@install {c-notes.wasm}", *self.source_snapshot(), "@status",
            "!rnote", "!wnote", "@restart", *self.source_snapshot(), "!rnote", "!wnote",
        ])
        self.assert_journals_unchanged(records)
        installs = [r for r in records if r["type"] == "install"]
        self.assertTrue(installs[0]["ok"]); self.assertFalse(installs[1]["ok"])
        self.assertIn("Command !wnote is already registered by Lua runtime", installs[1]["message"])
        statuses = [r["generation"] for r in records if r["type"] == "status"]
        self.assertEqual(statuses[0], statuses[1])
        self.assertEqual([r["text"] for r in records if r["type"] == "reply" and r["command"] == "!wnote"],
                         ["Lua 1", "Lua 2", "Lua 3"])
        self.assertEqual([r["text"] for r in records if r["type"] == "reply" and r["command"] == "!rnote"],
                         ["durable", "durable"])

    def test_both_rollback_directions_recheck_namespace_before_publication(self):
        lua = "function wnote() return 'Lua note' end"
        prior = "function lkeep() return kv.get('keep') or 'prior Lua' end"
        for runtime in ("lua", "wasm"):
            with self.subTest(runtime=runtime):
                if runtime == "wasm":
                    initial = "c-notes.wasm"
                    setup = ["@install {rust-notes.wasm}", "!rnote durable", "@install {note.lua}"]
                    rollback = "@source wasm rollback"
                    checks = ["!rnote", "!wnote"]
                else:
                    initial = "c-owner.wasm"
                    setup = ["@install {note.lua}", "@install {prior.lua}",
                             "@install {c-notes.wasm}", "!wnote durable"]
                    rollback = "@source rollback"
                    checks = ["!wnote", "!lkeep"]
                records = self.conflict_replay("wasm-conflict-rollback-" + runtime, initial,
                    {"note.lua": lua, "prior.lua": prior},
                    setup + self.source_snapshot() +
                    [rollback, "@pump 1200"] + self.source_snapshot() + checks +
                    ["@restart"] + self.source_snapshot() + checks)
                self.assert_journals_unchanged(records)
                status = [r["text"] for r in records if r["type"] == "source_control" and
                          r["command"] == ("" if runtime == "lua" else "wasm ") + "status"]
                self.assertIn("Command !wnote is already registered by", status[1])
                replies = [r["text"] for r in records if r["type"] == "reply"]
                self.assertNotIn("HIJACK", replies)
                self.assertNotIn("unknown command", " ".join(replies))
                self.assertEqual(replies[-4:], ["durable", "Lua note", "durable", "Lua note"] if runtime == "wasm"
                                 else ["durable", "prior Lua", "durable", "prior Lua"])

    def test_bundled_remove_and_builtin_policy_keep_other_runtime_intact(self):
        reserved = (MODULES / "c-owner.wasm").read_bytes().replace(b"wowner", b"choose")
        records = self.conflict_replay("wasm-conflict-remove", "c-notes.wasm",
            {"prior.lua": "function lkeep() return 'prior Lua' end", "reserved.wasm": reserved}, [
                "@install {prior.lua}", "!wnote durable", *self.source_snapshot(),
                "@install {reserved.wasm}", *self.source_snapshot(), "@restart", *self.source_snapshot(),
                "!lkeep", "!wnote", "@source remove", "@pump 1200",
                "!wnote", "!ping", "@restart", "!wnote", "!ping",
                "@source wasm remove", "@pump 1200", "!ping", "@restart", "!ping", "!wnote",
            ])
        # The first three snapshots precede deliberate successful removals.
        self.assert_journals_unchanged(records)
        installs = [r for r in records if r["type"] == "install"]
        self.assertFalse(installs[-1]["ok"])
        self.assertIn("reserved", installs[-1]["message"])
        for command in ("remove", "wasm remove"):
            removed = next(r for r in records if r["type"] == "source_control" and r["command"] == command)
            self.assertTrue(removed["ok"])
        notes = [r["text"] for r in records if r["type"] == "reply" and r["command"] == "!wnote"]
        self.assertEqual(notes[:3], ["durable"] * 3)
        self.assertIn("unknown command", notes[-1])
        self.assertEqual([r["text"] for r in records if r["type"] == "reply" and r["command"] == "!ping"],
                         ["Pong"] * 4)

    @staticmethod
    def immediate_snapshot():
        return [command.replace("@source ", "@source-now ", 1)
                for command in WasmHostRunnerTests.source_snapshot()]

    def assert_retry_denied_without_mutation(self, records):
        retries = [r for r in records if r["type"] == "source_control" and r["command"] == "retry"]
        self.assertEqual(len(retries), 1)
        self.assertFalse(retries[0]["ok"])
        self.assertIn("source wasm", retries[0]["text"])
        self.assert_journals_unchanged(records, snapshots=2)
        stages = [r for r in records if r["type"] == "staging"]
        self.assertEqual(len(stages), 2)
        self.assertEqual(stages[0], stages[1])
        statuses = [r for r in records if r["type"] == "status"]
        self.assertEqual(len(statuses), 2)
        self.assertEqual(statuses[0], statuses[1])

    def test_lua_retry_cannot_interrupt_wasm_copy_or_live_publication(self):
        for fault in ("source-copy-pause", "source-live-pause"):
            with self.subTest(fault=fault):
                records = self.conflict_replay("wasm-retry-" + fault, "c-notes.wasm",
                    {"prior.lua": "function lkeep() return kv.get('keep') or 'prior Lua' end"}, [
                        "@install {prior.lua}", "!wnote durable", "@install {rust-notes.wasm}", "!rnote durable",
                        "@fault " + fault, "@source-now wasm rollback", "@pump 60",
                        "@fault source-copy-wait", *self.immediate_snapshot(), "@staging", "@status",
                        "@source-now retry", "@staging", *self.immediate_snapshot(), "@status",
                        "@fault off", "@pump 1200", "!lkeep", "!wnote", "!ping",
                    ])
                self.assert_retry_denied_without_mutation(records)
                wasm_status = next(r["text"] for r in records if r["type"] == "source_control" and
                                   r["command"] == "wasm status")
                self.assertIn("gen=2 active=1" if fault == "source-copy-pause" else "gen=3 active=0",
                              wasm_status)
                self.assertIn("activation not committed" if fault == "source-copy-pause" else
                              "source selected; live copy pending", wasm_status)
                replies = {r["command"]: r["text"] for r in records if r["type"] == "reply"}
                self.assertEqual(replies["!lkeep"], "prior Lua")
                self.assertEqual(replies["!wnote"], "durable")
                self.assertEqual(replies["!ping"], "Pong")
                self.assertFalse(any(r["type"] == "restart" for r in records))

    def test_exhausted_wasm_live_retries_keep_lease_and_selected_recovery_works(self):
        prior = ("function lkeep() local n=(kv.get('lua-count') or '')..'x' "
                 "kv.put('lua-count',n) return 'Lua '..#n end")
        records = self.conflict_replay("wasm-retry-exhausted", "c-notes.wasm",
            {"prior.lua": prior}, [
                "@install {prior.lua}", "!wnote durable", "@install {rust-notes.wasm}", "!lkeep", "!rnote durable",
                "@fault source-live-read", "@source-now wasm rollback",
                "@pump 10000", "@pump 10000", "@fault off",
                *self.immediate_snapshot(), "@staging", "@status", "@source-now retry",
                "@staging", *self.immediate_snapshot(), "@status", "!lkeep", "!rnote",
                "@source-now wasm retry", "@pump 1200", "!lkeep", "!wnote", "!ping",
            ])
        self.assert_retry_denied_without_mutation(records)
        wasm_status = next(r["text"] for r in records if r["type"] == "source_control" and
                           r["command"] == "wasm status")
        self.assertIn("gen=3 active=0", wasm_status)
        self.assertIn("use source wasm retry; prior live retained", wasm_status)
        self.assertNotIn("live retry pending", wasm_status)
        selected = next(r for r in records if r["type"] == "source_control" and r["command"] == "wasm retry")
        self.assertTrue(selected["ok"])
        self.assertEqual([r["text"] for r in records if r["type"] == "reply" and r["command"] == "!lkeep"],
                         ["Lua 1", "Lua 2", "Lua 3"])
        self.assertTrue(any(r.get("command") == "!rnote" and r.get("text") == "durable" for r in records))
        self.assertTrue(any(r.get("command") == "!wnote" and r.get("text") == "durable" for r in records))
        self.assertFalse(any(r["type"] == "restart" for r in records))

    def test_refused_live_staging_is_bounded_and_recovers_without_reboot(self):
        records = self.conflict_replay("wasm-retry-stage-refusal", "c-notes.wasm",
            {"prior.lua": "function lkeep() return 'prior Lua' end"}, [
                "@install {prior.lua}", "!wnote durable", "@install {rust-notes.wasm}", "!rnote durable",
                "@fault source-result-held", "@source-now wasm rollback", "@pump 4000",
                *self.immediate_snapshot(), "@status", "@source-now retry",
                *self.immediate_snapshot(), "@status", "@fault off", "!lkeep", "!rnote",
                "@source-now wasm retry", "@pump 1200", "!lkeep", "!wnote", "!ping",
            ])
        self.assert_journals_unchanged(records, snapshots=2)
        wasm_status = next(r["text"] for r in records if r["type"] == "source_control" and
                           r["command"] == "wasm status")
        self.assertIn("Wasm live staging refused; saved source retained", wasm_status)
        self.assertIn("use source wasm retry", wasm_status)
        self.assertNotIn("live retry pending", wasm_status)
        retry = next(r for r in records if r["type"] == "source_control" and r["command"] == "retry")
        self.assertFalse(retry["ok"])
        replies = [r["text"] for r in records if r["type"] == "reply"]
        self.assertEqual(replies[-3:], ["prior Lua", "durable", "Pong"])
        self.assertTrue(any(r.get("command") == "!rnote" and r.get("text") == "durable" for r in records))
        self.assertFalse(any(r["type"] == "restart" for r in records))

    def test_refused_live_removal_is_bounded_and_recovers_without_reboot(self):
        prior = "function lkeep() return 'prior Lua' end"
        records = self.conflict_replay("wasm-retry-remove-refusal", "c-notes.wasm",
            {"prior.lua": prior}, [
                "@install {prior.lua}", "!wnote durable", "@send !mt 30",
                "@source-now wasm remove", "@pump 4000",
                *self.immediate_snapshot(), "@source-now retry", *self.immediate_snapshot(),
                "@context dm other", "!lkeep", "!wnote",
                "@advance 30000", "@pump 200", "@source-now wasm retry", "@pump 1200",
                "!lkeep", "!ping", "!wnote",
            ])
        self.assert_journals_unchanged(records, snapshots=2)
        wasm_status = next(r["text"] for r in records if r["type"] == "source_control" and
                           r["command"] == "wasm status")
        self.assertIn("gen=2 active=3", wasm_status)
        self.assertIn("Wasm live removal refused; saved source retained", wasm_status)
        self.assertIn("use source wasm retry", wasm_status)
        self.assertNotIn("live retry pending", wasm_status)
        retry = next(r for r in records if r["type"] == "source_control" and r["command"] == "retry")
        self.assertFalse(retry["ok"])
        selected = next(r for r in records if r["type"] == "source_control" and r["command"] == "wasm retry")
        self.assertTrue(selected["ok"])
        replies = [r["text"] for r in records if r["type"] == "reply"]
        self.assertEqual(replies[-3:-1], ["prior Lua", "Pong"])
        self.assertIn("unknown command", replies[-1])
        self.assertFalse(any(r["type"] == "restart" for r in records))

    def test_native_command_dispatch_storage_coexistence_and_trap_recovery(self):
        state = ROOT / ".tmp/wasm-native-replay"
        state.mkdir(exist_ok=True)
        lua = state / "custom.lua"
        replay = state / "commands.replay"
        lua.write_text(
            "function custom() local value=(kv.get('custom_count') or '')..'x' "
            "kv.put('custom_count',value) return 'Lua still here '..#value end "
            "command('custom','','Retained Lua fixture','custom')"
        )
        replay.write_text(
            "!wnote durable\n"
            f"@install {lua}\n"
            "!custom\n"
            "@restart\n"
            "!custom\n"
            "!wnote\n"
            f"@install {MODULES / 'fault-0.wasm'}\n"
            "!wfault\n"
            "!custom\n"
            "!ping\n"
            f"@install {MODULES / 'c-notes.wasm'}\n"
            "!wnote\n"
            "@restart\n"
            "!wnote\n"
            "!custom\n"
            f"@install {MODULES / 'rust-notes.wasm'}\n"
            "!rnote Rust durable\n"
            "@restart\n"
            "!rnote\n"
            "!custom\n"
            "@status\n"
        )
        try:
            result = subprocess.run([str(RUNNER), "--source", str(MODULES / "c-notes.wasm"),
                                     "--replay", str(replay)], capture_output=True, text=True, check=True)
            self.assertIn("REPLY !wnote => durable", result.stdout)
            self.assertIn("REPLY !rnote => Rust durable", result.stdout)
            for count in range(1, 6):
                self.assertIn(f"REPLY !custom => Lua still here {count}", result.stdout)
            self.assertEqual(result.stdout.count("REPLY !wnote => durable"), 3)
            self.assertEqual(result.stdout.count("RESTART generation="), 3)
            self.assertIn("REPLY !wfault => Error:", result.stdout)
            self.assertIn("Wasm instruction budget exceeded", result.stdout)
            self.assertNotIn("FAULT ", result.stdout)
        finally:
            replay.unlink(missing_ok=True)
            lua.unlink(missing_ok=True)
            state.rmdir()

    def test_binary_package_journals_and_native_owner_authority_survive_restart(self):
        state = ROOT / ".tmp/wasm-native-owner-replay"
        state.mkdir(exist_ok=True)
        wasm = state / "owner.bot.wasm"
        lua = state / "owner.lua"
        replay = state / "owner.replay"
        wasm.write_bytes(bot_packages.create(
            (MODULES / "c-owner.wasm").read_bytes(), "wasm-owner", "1.0.0", ["cmdmeta"]
        ).source)
        lua.write_text(
            "function lua_owner() return 'Lua owner' end "
            "command('lua-owner','','Owner fixture','lua_owner','owner')"
        )
        replay.write_text(
            f"@install {lua}\n"
            "@owner alice\n@context dm alice\n!wowner\n!lua-owner\n"
            "@context dm bob\n!wowner\n!lua-owner\n"
            "@restart\n@context dm alice\n!wowner\n!lua-owner\n"
            "@context dm bob\n!wowner\n!lua-owner\n"
            f"@install {wasm}\n"
            "@restart\n@context dm alice\n!wowner\n!lua-owner\n"
            "@owner none\n!wowner\n!lua-owner\n"
        )
        try:
            result = subprocess.run(
                [str(RUNNER), "--format", "jsonl", "--source", str(wasm), "--replay", str(replay)],
                capture_output=True, text=True, check=True,
            )
            records = [json.loads(line) for line in result.stdout.splitlines()]
            self.assertEqual(sum(r["type"] == "restart" for r in records), 2)
            for command, success in [("!wowner", "Wasm owner"), ("!lua-owner", "Lua owner")]:
                replies = [r["text"] for r in records if r["type"] == "reply" and r["command"] == command]
                self.assertEqual(replies[::2], [success, success, success])
                self.assertEqual(len(replies), 6)
                self.assertTrue(all("permission not granted" in r for r in replies[1::2]))
            self.assertTrue(all(r["ok"] for r in records if r["type"] == "install"))
        finally:
            for path in (wasm, lua, replay):
                path.unlink(missing_ok=True)
            state.rmdir()


if __name__ == "__main__":
    unittest.main()
