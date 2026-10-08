import fcntl
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location(
    "host_backup", Path(__file__).resolve().parents[1] / "packaging/host-backup.py")
backup = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(backup)


class HostBackupTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name) / "home"
        self.home.mkdir()
        self.worker = self.home / ".local/libexec/custom-worker"
        self.state = self.home / ".config/meshcore-host/data"
        for name, relative in backup.TARGETS.items():
            path = self.home / relative
            if name in ("config", "state"):
                path.mkdir(parents=True)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("old-" + name)
        self.worker.parent.mkdir(parents=True)
        self.worker.write_text("old-worker")
        self.worker.chmod(0o755)
        self.state.mkdir()
        (self.state / "identity").write_text("old-identity")
        (self.home / backup.TARGETS["config"] / "config.json").write_text(json.dumps({
            "state_dir": "data", "bot_runtime": "native_lua",
            "bot_native_worker": str(self.worker)}))
        self.directory = Path(self.temp.name) / "snapshot"

    @patch.object(backup, "service")
    @patch.object(backup.subprocess, "run")
    def test_configured_worker_and_state_restore(self, run, service):
        backup.backup(self.directory, self.home)
        self.worker.write_text("new-worker")
        (self.state / "identity").write_text("new-identity")
        (self.state / "new-envelope").write_text("must-not-overlay")
        backup.restore(self.directory, self.home)
        self.assertEqual(self.worker.read_text(), "old-worker")
        self.assertEqual(self.worker.stat().st_mode & 0o777, 0o755)
        self.assertEqual((self.state / "identity").read_text(), "old-identity")
        self.assertFalse((self.state / "new-envelope").exists())
        previous, = self.directory.glob("replaced-*")
        self.assertEqual((previous / "worker").read_text(), "new-worker")
        self.assertTrue((previous / "state/new-envelope").exists())
        self.assertEqual(service.call_count, 2)

    @patch.object(backup, "service")
    def test_missing_worker_refused_before_stop(self, service):
        self.worker.unlink()
        with self.assertRaisesRegex(RuntimeError, "worker not found"):
            backup.backup(self.directory, self.home)
        service.assert_not_called()

    @patch.object(backup, "service")
    def test_overlapping_backup_refused_before_stop(self, service):
        with self.assertRaisesRegex(RuntimeError, "overlap"):
            backup.backup(self.state / "snapshot", self.home)
        service.assert_not_called()

    @patch.object(backup, "service")
    def test_running_host_refused(self, service):
        with (self.state / ".lock").open("a+b") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(RuntimeError, "Another host"):
                backup.backup(self.directory, self.home)
        self.assertFalse((self.directory / "manifest.json").exists())


if __name__ == "__main__":
    unittest.main()
