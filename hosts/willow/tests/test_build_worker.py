# SPDX-License-Identifier: Apache-2.0
import importlib.util
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "willow_build_worker", Path(__file__).resolve().parents[1] / "tools/build_worker.py")
worker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(worker)


class SourceIdentity(unittest.TestCase):
    def test_git_archive_identity_outside_git_and_inside_another_repository(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            expected = worker.source_identity(worker.ROOT)
            archive = root / "source.tar"
            if (worker.ROOT / ".git").exists():
                # Exercise export-subst using the existing source revision.
                subprocess.run(["git", "-C", str(worker.ROOT), "archive", "--format=tar",
                                "-o", str(archive), "HEAD", "release/source.json"], check=True)
            else:
                # Published source already carries its expanded release identity.
                with tarfile.open(archive, "w") as package:
                    package.add(worker.ROOT / "release/source.json", arcname="release/source.json")
            extracted = root / "extracted"
            extracted.mkdir()
            with tarfile.open(archive) as package:
                package.extractall(extracted, filter="data")
            self.assertEqual(worker.source_identity(extracted), expected)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            self.assertEqual(worker.source_identity(extracted), expected)

    def test_package_core_and_manifest_are_worker_binding_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = ["Makefile", "release/source.json", "release/products.json", "release/versions.mk",
                     "firmware/esp32/Makefile", "firmware/esp32/bot.mk", "firmware/esp32/prepare.py",
                     "firmware/esp32/prepare_bot.py", "firmware/esp32/prepare_wasm.py",
                     "firmware/shared/radio-reconfigure.patch", "firmware/shared/queued-dispatch.patch",
                     "packages/hew-wamr/native/CMakeLists.txt", "packages/hew-lua/hew.toml",
                     "packages/hew-lua/native/hlua_core.hpp", "packages/hew-lua/native/lua/llex.c",
                     "packages/hew-wamr/hew.toml", "packages/hew-wamr/native/runtime.cpp",
                     "packages/hew-wamr/native/vendor/core/iwasm/common/arch/invokeNative_em64.s"]
            for name in paths:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("initial")
            with patch.object(worker, "ROOT", root):
                sources = [root/name for name in paths if Path(name).suffix in (".hpp", ".c", ".cpp", ".s")]
                before = worker.local_inputs(sources)
                for name in paths[2:]:
                    path = root / name
                    path.write_text("changed")
                    after = worker.local_inputs(sources)
                    self.assertNotEqual(before[str(path)], after[str(path)])
                    before = after
                removed = sources[0]
                removed.unlink()
                with self.assertRaises(FileNotFoundError):
                    worker.local_inputs(sources)
                rebuilding = worker.local_inputs(sources, existing_only=True)
                self.assertNotIn(str(removed), rebuilding)
                self.assertIn(str(sources[1]), rebuilding)

    def test_invalid_archive_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "release").mkdir()
            for value in (
                {"commit": "$Format:%H$", "commit_epoch": "$Format:%ct$"},
                {"commit": "a" * 40, "commit_epoch": "0"},
                {"commit": "a" * 40, "commit_epoch": 1791419594},
                {"commit": "wrong", "commit_epoch": "1791419594"},
            ):
                with self.subTest(value=value):
                    (root / "release/source.json").write_text(json.dumps(value))
                    with self.assertRaises(ValueError):
                        worker.source_identity(root)


if __name__ == "__main__":
    unittest.main()
