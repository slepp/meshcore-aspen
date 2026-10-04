import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("budget_build", Path(__file__).with_name("build.py"))
budget = importlib.util.module_from_spec(spec)
spec.loader.exec_module(budget)


class BuildSourceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.git("init", "--quiet")
        self.git("config", "user.name", "Resource test")
        self.git("config", "user.email", "resource-test@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        (self.root / "firmware").mkdir()
        (self.root / "firmware/main.cpp").write_text("int main() { return 0; }\n")
        (self.root / "firmware/README.md").write_text("# Test firmware\n")
        (self.root / "Makefile").write_text("all:\n\t@true\n")
        self.git("add", ".")
        self.git("commit", "--quiet", "-m", "Synthetic source fixture")
        self.revision = self.git("rev-parse", "HEAD").strip()

    def git(self, *args):
        return subprocess.check_output(["git", *args], cwd=self.root, text=True)

    def test_default_uses_the_clean_checkout_head(self):
        self.assertEqual(budget.verify_source(self.root), self.revision)
        self.assertEqual(self.git("rev-list", "--count", "HEAD").strip(), "1")

    def test_available_baseline_and_documentation_edits_are_allowed(self):
        (self.root / "firmware/README.md").write_text("# Updated guide\n")
        (self.root / "firmware/notes.md").write_text("# Local notes\n")
        self.assertEqual(budget.verify_source(self.root, self.revision), self.revision)

    def test_missing_revision_is_rejected_without_fallback(self):
        with self.assertRaises(subprocess.CalledProcessError):
            budget.verify_source(self.root, "refs/heads/unavailable-baseline")

    def test_changed_tracked_firmware_is_rejected(self):
        (self.root / "firmware/main.cpp").write_text("int main() { return 1; }\n")
        with self.assertRaises(subprocess.CalledProcessError):
            budget.verify_source(self.root)

    def test_changed_makefile_is_rejected(self):
        (self.root / "Makefile").write_text("all:\n\t@false\n")
        with self.assertRaises(subprocess.CalledProcessError):
            budget.verify_source(self.root)

    def test_untracked_firmware_is_rejected(self):
        (self.root / "firmware/untracked.cpp").write_text("int extra = 1;\n")
        with self.assertRaisesRegex(ValueError, "Untracked firmware inputs"):
            budget.verify_source(self.root)


if __name__ == "__main__":
    unittest.main()
