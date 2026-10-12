"""Ordinary builds follow compiler selection and standard-library upgrades."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"tools"))
from toolchain import update


class Toolchain(unittest.TestCase):
    def installation(self, root):
        (root / "bin").mkdir(parents=True)
        compiler = root / "bin/hew"
        compiler.write_bytes(b"compiler")
        (root / "std").mkdir()
        (root / "std/builtins.hew").write_text("builtin")
        (root / "lib").mkdir()
        (root / "lib/libhew.a").write_bytes(b"runtime")
        return compiler

    def test_selected_compiler_and_library_changes_rebuild_incrementally(self):
        with tempfile.TemporaryDirectory() as name, patch.dict(os.environ, {}, clear=True):
            root = Path(name)
            first = self.installation(root / "first")
            second = self.installation(root / "second")
            os.utime(second, (1, 1))
            output = root / "build/toolchain.json"
            self.assertTrue(update(str(first), output))
            unchanged = output.stat().st_mtime_ns
            self.assertFalse(update(str(first), output))
            self.assertEqual(output.stat().st_mtime_ns, unchanged)
            self.assertTrue(update(str(second), output))
            self.assertFalse(update(str(second), output))
            (root / "second/lib/libhew.a").write_bytes(b"new runtime")
            self.assertTrue(update(str(second), output))
            (root / "second/std/builtins.hew").write_text("new builtins")
            self.assertTrue(update(str(second), output))
            self.assertFalse(update(str(second), output))

    def test_custom_standard_library_directory_is_tracked(self):
        with tempfile.TemporaryDirectory() as name, patch.dict(os.environ, {}, clear=True):
            root = Path(name)
            compiler = self.installation(root / "installation")
            custom = root / "custom-standard-library"
            custom.mkdir()
            module = custom / "builtins.hew"
            module.write_text("custom builtins")
            os.environ["HEW_STD"] = str(custom)
            output = root / "toolchain.json"
            self.assertTrue(update(str(compiler), output))
            self.assertFalse(update(str(compiler), output))
            module.write_text("updated custom builtins")
            self.assertTrue(update(str(compiler), output))


if __name__ == "__main__":
    unittest.main()
