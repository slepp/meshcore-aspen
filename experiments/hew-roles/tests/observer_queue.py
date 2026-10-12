"""Checkpoint recovery through separate real Hew processes, in both build modes."""
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Checkpoint(unittest.TestCase):
    def test_restart_ack_expiry_failure_and_bounds(self):
        for binary in ("observer-queue-checks", "observer-queue-checks-release"):
            with self.subTest(binary=binary), tempfile.TemporaryDirectory() as directory:
                path = Path(directory)/"observer.outbox"

                def run(mode):
                    result = subprocess.run([str(ROOT/"build"/binary), str(path), mode],
                        capture_output=True, text=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                    return result.stdout

                def pending(text):
                    return next(line for line in text.splitlines() if line.startswith("PENDING "))

                original = run("seed")
                saved = path.read_bytes()
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)
                self.assertEqual(pending(run("resume")), pending(original))
                run("replace")
                path.write_bytes(saved)
                # PUBACK changed RAM only: a crash can duplicate that exact event.
                self.assertEqual(path.read_bytes(), saved)
                self.assertEqual(pending(run("resume")), pending(original))
                # A power interruption leaves the prior committed file authoritative.
                stage = Path(str(path)+".pending")
                stage.write_bytes(saved[:len(saved)//2])
                stage.chmod(0o600)
                self.assertEqual(pending(run("drain")), pending(original))
                self.assertFalse(stage.exists())
                run("empty")
                run("restoring")
                for mode in ("generation", "failure"):
                    path.unlink()
                    run("seed" if mode == "failure" else mode)
                    if mode == "failure":
                        self.assertIn("OBSERVER_STORE_FAILED", run(mode))
                for mode in ("expiry", "future"):
                    path.unlink()
                    run(mode+"-seed")
                    before = path.read_bytes()
                    run(mode)
                    self.assertEqual(path.read_bytes(), before)
                for invalid in (b"", saved[:40], saved[:-1], saved[:-32]+bytes(32)):
                    path.write_bytes(invalid)
                    self.assertIn("OBSERVER_STORE_FAILED", run("invalid"))
                    self.assertEqual(path.read_bytes(), invalid)
                body = b"WOBS9999"+saved[8:-32]
                unsupported = body+hashlib.sha256(body).digest()
                path.write_bytes(unsupported)
                self.assertIn("format is unsupported", run("invalid"))
                self.assertEqual(path.read_bytes(), unsupported)
                path.unlink()
                run("capacity")
                self.assertLessEqual(path.stat().st_size, 4194304)


if __name__ == "__main__":
    unittest.main(verbosity=2)
