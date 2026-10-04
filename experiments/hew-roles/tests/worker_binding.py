"""Worker verification uses private synthetic inputs, never edits the real manifest."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WorkerBinding(unittest.TestCase):
    def test_manifest_binding_and_changed_input(self):
        root = ROOT/"build"/f"worker-check-{os.getpid()}"
        root.mkdir(mode=0o700)
        try:
            source = root/"source"
            source.write_bytes(b"compiler input")
            manifest = root/"inputs"
            manifest.write_text(hashlib.sha256(source.read_bytes()).hexdigest()+"\t"+str(source)+"\n")
            expected = hashlib.sha256(manifest.read_bytes()).hexdigest()
            def check(value=expected):
                return subprocess.check_output([str(ROOT/"build/worker-checks"),str(manifest),value],text=True).strip()
            self.assertEqual(check(),"true")
            self.assertEqual(check("0"*64),"false")
            source.write_bytes(b"changed source")
            self.assertEqual(check(),"false")
            manifest.write_text("0"*64+"\t"+str(source))
            self.assertEqual(check(hashlib.sha256(manifest.read_bytes()).hexdigest()),"false")
        finally:
            shutil.rmtree(root)

    def test_external_worker_rejected_before_connect(self):
        root = ROOT/"build"/f"worker-reject-{os.getpid()}"
        root.mkdir(mode=0o700)
        try:
            config = root/"config"
            config.write_text("address=127.0.0.1\nport=1\nprofile=c806643690d003000705020000803f000000\nworker=/bin/true\n")
            config.chmod(0o600)
            completed = subprocess.run([os.environ.get("MESHCORE_HEW_HOST",str(ROOT/"build/hew-host")),str(root)],
                                       capture_output=True,text=True,timeout=5)
            self.assertNotEqual(completed.returncode,0)
            self.assertIn("unverified external native worker",completed.stderr+completed.stdout)
            self.assertNotIn("ONLINE",completed.stdout)
            self.assertFalse(list(root.glob("*.seed")))
            completed = subprocess.run(["python3","-B",str(ROOT/"build_worker.py"),"--verify","--worker","/bin/true"],
                                       capture_output=True,text=True,timeout=5)
            self.assertNotEqual(completed.returncode,0)
            self.assertIn("unverified external worker",completed.stderr)
        finally:
            shutil.rmtree(root)


if __name__ == "__main__":
    unittest.main(verbosity=2)
