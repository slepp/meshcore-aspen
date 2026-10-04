"""Kill real writers after partial staging writes; never use live state."""
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time
import unittest
from parity import BIN, BUILD, Host, public


def stopped(process):
    deadline=time.monotonic()+10
    while time.monotonic()<deadline:
        pid,status=os.waitpid(process.pid,os.WUNTRACED|os.WNOHANG)
        if pid:
            if os.WIFSTOPPED(status): return
            raise AssertionError(f"writer exited before interruption: {status}")
        time.sleep(.01)
    raise AssertionError("writer did not reach partial-write interruption")


def kill_writer(process):
    process.kill()
    process.wait(timeout=5)
    for stream in (process.stdin,process.stdout,process.stderr):
        if stream: stream.close()


class StorageRecovery(unittest.TestCase):
    def setUp(self):
        self.root=BUILD/f"recovery-{os.getpid()}-{time.monotonic_ns()}"
        self.root.mkdir(mode=0o700)
    def tearDown(self):
        shutil.rmtree(self.root)

    def test_interrupted_first_identity_is_never_promoted(self):
        target=self.root/"new.seed"
        pending=Path(str(target)+".pending")
        env=os.environ|{"HEW_TEST_COMMIT_INTERRUPT":str(target)}
        process=subprocess.Popen([str(BUILD/"roles-interrupt"),"identity",str(target)],
                                 stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env)
        try:
            stopped(process)
            partial=pending.read_bytes()
            self.assertEqual(len(partial),16)
            self.assertFalse(target.exists())
            contender=subprocess.run([str(BIN),"identity",str(target)],capture_output=True,timeout=5)
            self.assertNotEqual(contender.returncode,0)
            self.assertEqual(pending.read_bytes(),partial)
        finally: kill_writer(process)
        subprocess.run([str(BIN),"identity",str(target)],check=True,capture_output=True,timeout=5)
        committed=target.read_bytes()
        self.assertEqual(len(committed),32)
        self.assertNotEqual(committed[:16],partial)
        self.assertFalse(pending.exists())
        pending.write_bytes(b"abandoned replacement"); pending.chmod(0o600)
        retry=subprocess.run([str(BIN),"identity",str(target)],capture_output=True,timeout=5)
        self.assertNotEqual(retry.returncode,0) # existing identity is never overwritten
        self.assertEqual(target.read_bytes(),committed)
        self.assertFalse(pending.exists())

    def test_interrupted_snapshot_keeps_last_committed_target(self):
        target=self.root/"role.state"
        with Host() as h:
            self.assertEqual(h.command("state "+str(target)),["LOCKED"])
            h.command(f"acl {public(2).hex()} 130")
        committed=target.read_bytes()
        writer=Host(binary=BUILD/"roles-interrupt",env=os.environ|{"HEW_TEST_COMMIT_INTERRUPT":str(target)})
        try:
            self.assertEqual(writer.command("state "+str(target)),["LOCKED"])
            writer.proc.stdin.write(f"acl {public(2).hex()} 2\n"); writer.proc.stdin.flush()
            stopped(writer.proc)
            pending=Path(str(target)+".pending")
            partial=pending.read_bytes()
            self.assertEqual(target.read_bytes(),committed)
            self.assertLess(len(partial),len(committed))
            with Host() as contender:
                self.assertEqual(contender.command("state "+str(target)),["ERROR state lock"])
            self.assertEqual(pending.read_bytes(),partial)
        finally: kill_writer(writer.proc)
        with Host() as restored:
            self.assertEqual(restored.command("state "+str(target)),["LOCKED"])
            self.assertEqual(target.read_bytes(),committed)
            self.assertFalse(pending.exists())
            self.assertIn("permission=130",restored.command("cursor "+public(2).hex())[0])
            restored.command(f"acl {public(2).hex()} 2")
        with Host() as restored:
            self.assertEqual(restored.command("state "+str(target)),["LOCKED"])
            self.assertIn("permission=2",restored.command("cursor "+public(2).hex())[0])

    def test_unsafe_staging_objects_are_not_removed(self):
        target=self.root/"state"
        victim=self.root/"victim"
        victim.write_bytes(b"committed private bytes"); victim.chmod(0o600)
        pending=Path(str(target)+".pending")
        for kind in ("symlink","hardlink","directory","public-file","fifo"):
            with self.subTest(kind=kind):
                if kind=="symlink": pending.symlink_to(victim)
                elif kind=="hardlink": os.link(victim,pending)
                elif kind=="directory": pending.mkdir(mode=0o700)
                elif kind=="fifo": os.mkfifo(pending,0o600)
                else: pending.write_bytes(b"bad mode"); pending.chmod(0o644)
                with Host() as h:
                    self.assertIn("could not be recovered",h.command("state "+str(target))[0])
                    self.assertIn("closed=true",h.command("status")[0])
                self.assertEqual(victim.read_bytes(),b"committed private bytes")
                self.assertTrue(pending.exists())
                if kind=="directory": pending.rmdir()
                else: pending.unlink()


if __name__=="__main__":
    unittest.main(verbosity=2)
