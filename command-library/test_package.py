# SPDX-License-Identifier: Apache-2.0
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
spec = importlib.util.spec_from_file_location("command_library_package", HERE / "package.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class Downloads(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="command-library-test-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.output = Path(cls.temporary.name)
        cls.catalog = package.build(cls.output)

    def files(self, entry):
        archive = self.output / entry["download"]["file"]
        with zipfile.ZipFile(archive) as bundle:
            prefix = archive.stem + "/"
            self.assertTrue(all(name.startswith(prefix) for name in bundle.namelist()))
            return {name.removeprefix(prefix): bundle.read(name) for name in bundle.namelist()}

    def test_archives_contain_canonical_code_and_complete_checksums(self):
        for entry in self.catalog["entries"]:
            with self.subTest(entry=entry["id"]):
                archive = self.output / entry["download"]["file"]
                self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(), entry["download"]["sha256"])
                self.assertEqual(archive.stat().st_size, entry["download"]["bytes"])
                files = self.files(entry)
                self.assertEqual(set(files), {"README.md", "manifest.json", "SHA256SUMS", *package.LEGAL,
                                              *(item["name"] for item in entry["files"])})
                for item in entry["files"]:
                    self.assertEqual(files[item["name"]], (ROOT / item["source"]).read_bytes())
                sums = dict(line.split("  ", 1)[::-1] for line in files["SHA256SUMS"].decode().splitlines())
                self.assertEqual(set(sums), set(files) - {"SHA256SUMS"})
                for name, digest in sums.items():
                    self.assertEqual(hashlib.sha256(files[name]).hexdigest(), digest)
                manifest = json.loads(files["manifest.json"])
                self.assertEqual(manifest["entry"], {key: value for key, value in entry.items() if key != "download"})
                for record in manifest["files"]:
                    self.assertEqual(len(files[record["name"]]), record["bytes"])
                    self.assertEqual(hashlib.sha256(files[record["name"]]).hexdigest(), record["sha256"])

    @unittest.skipUnless(os.environ.get("BOT_HOST_RUNNER"), "set BOT_HOST_RUNNER for native packet replays")
    def test_packaged_commands_and_bundled_main_on_native_runner(self):
        runner = Path(os.environ["BOT_HOST_RUNNER"]).resolve()
        examples = ROOT / "firmware/runtime/plugins/examples"
        sources = {"main": None}
        for entry in self.catalog["entries"]:
            files = self.files(entry)
            lua = next(item["name"] for item in entry["files"] if item["runtime"] == "lua")
            sources[entry["source_name"]] = files[lua]
            if entry["id"] in ("greeting", "private-notes"):
                source = self.output / lua
                source.write_bytes(files[lua])
                scenario = examples / ("hello.scenario.json" if entry["id"] == "greeting" else "notes.scenario.json")
                self.replay(runner, source, scenario)
        sys.path.insert(0, str(ROOT))
        from tools.hardware.lua_sources import encode
        source = self.output / "additive.lua"
        source.write_bytes(encode(sources))
        scenario = self.output / "additive.scenario.json"
        scenario.write_text(json.dumps({"replay": ["!ping", "!hello mesh", "@context dm alice",
            "!note-save lunch", "!note-read", "@context dm bob", "!note-read", "!snr"], "expect": [
            {"type": "reply", "text": "Pong"}, {"type": "reply", "text": "Hello mesh"},
            {"type": "reply", "text": "Saved"}, {"type": "reply", "text": "lunch"},
            {"type": "reply", "text": "No note"},
            {"type": "reply", "text": "No repeater signal samples available"}]}))
        self.replay(runner, source, scenario)

    def replay(self, runner, source, scenario):
        result = subprocess.run([sys.executable, str(ROOT / "firmware/runtime/bot_local.py"),
                                 "--runner", str(runner), "--source", str(source), "--scenario", str(scenario)],
                                cwd=ROOT, capture_output=True, text=True, timeout=130)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
