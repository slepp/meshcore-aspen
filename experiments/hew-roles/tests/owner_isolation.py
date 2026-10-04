"""A withheld admin reply must not block private health or DM controls."""
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
import uuid

from parity import ROOT
sys.path.insert(0, str(ROOT))
from willow import owner_rpc, owner_inbox, owner_status


class OwnerIsolation(unittest.TestCase):
    def test_deadline_and_worker_fence_preserve_control_responsiveness(self):
        for binary in ("owner-isolation-checks", "owner-isolation-checks-release"):
            with self.subTest(binary=binary), tempfile.TemporaryDirectory(dir=ROOT/"build") as name:
                root = Path(name)
                (root/"bot.seed").write_bytes(bytes([5])*32)
                (root/"bot.seed").chmod(0o600)
                process = subprocess.Popen([str(ROOT/"build"/binary), str(root)],
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    end = time.monotonic()+4
                    while not ((root/"owner.sock").exists() and (root/"admin.sock").exists()):
                        self.assertLess(time.monotonic(), end, "private fixture sockets unavailable")
                        time.sleep(.02)
                    with ThreadPoolExecutor(max_workers=1) as pool:
                        pending = pool.submit(owner_rpc, root, b"\x01\x05\x03get name")
                        self.assertEqual(process.stdout.readline().strip(), "WITHHELD request=1")
                        started = time.monotonic()
                        for _ in range(10):
                            before = time.monotonic()
                            self.assertEqual(json.loads(owner_rpc(root, b"\x01\x06\x00")),
                                             {"fixture": "live"})
                            self.assertTrue(json.loads(owner_rpc(root, b"\x01\x06\x01"))["ready"])
                            self.assertEqual(owner_inbox(root)["messages"], [])
                            with self.assertRaisesRegex(ValueError, "request-id-not-recorded"):
                                owner_status(root, uuid.uuid4().bytes)
                            self.assertLess(time.monotonic()-before, .5)
                            self.assertFalse(pending.done(), "withheld admin request completed prematurely")
                        with self.assertRaisesRegex(ValueError, "outcome-unknown"):
                            pending.result(timeout=10)
                        self.assertGreaterEqual(time.monotonic()-started, 8)
                        with self.assertRaisesRegex(ValueError, "outcome-unknown"):
                            owner_rpc(root, b"\x01\x05\x03get name")
                        self.assertEqual(process.stdout.readline().strip(), "WITHHELD request=2")
                    self.assertEqual(int.from_bytes((root/"admin.clock").read_bytes()[4:], "little"),
                                     1073741825)
                    self.assertTrue(json.loads(owner_rpc(root, b"\x01\x06\x01"))["ready"])
                    for path, request in (("owner.sock", "0105676574206e616d65"),
                                          ("admin.sock", "010600")):
                        result = subprocess.run([str(ROOT/"build/owner-client-checks"), str(root/path), request],
                                                capture_output=True, text=True, check=True, timeout=4)
                        self.assertEqual(bytes.fromhex(result.stdout.strip()), b"\x01\x01")
                    stdout, stderr = process.communicate(timeout=8)
                    self.assertEqual(process.returncode, 0, stderr)
                    self.assertEqual(stdout.strip(), "")
                    self.assertFalse((root/"owner.sock").exists())
                    self.assertFalse((root/"admin.sock").exists())
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.communicate(timeout=5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
