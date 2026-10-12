"""Kill real writers after partial staging writes; never use live state."""
import os
from pathlib import Path
import shutil
import select
import signal
import subprocess
import tempfile
import time
import unittest
BUILD = Path(__file__).resolve().parents[1]/"build"


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
        global BIN, Host, public
        from parity import BIN, Host, public
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
            ready=select.select([writer.proc.stdout],[],[],0)[0]
            reply=os.read(writer.proc.stdout.fileno(),4096) if ready else b""
            self.assertNotIn(b"END\n",reply)
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

    def test_unsupported_and_malformed_snapshots_stop_promptly(self):
        victim=self.root/"retained"
        victim.write_bytes(b"malformed snapshot"); victim.chmod(0o600)
        for kind in ("fifo", "directory", "symlink", "malformed"):
            with self.subTest(kind=kind):
                target=self.root/("snapshot-"+kind)
                if kind=="fifo": os.mkfifo(target,0o600)
                elif kind=="directory": target.mkdir(mode=0o700)
                elif kind=="symlink": target.symlink_to(victim)
                else: target.write_bytes(b"malformed snapshot"); target.chmod(0o600)
                result=subprocess.run([str(BIN), "serve", "3", "01"*32,
                    "Hew Room", "room", "admin", "3", "-", "-"],
                    input=f"state {target}\nstatus\n", text=True,
                    capture_output=True, timeout=3, check=True)
                self.assertIn("ERROR invalid state; role stopped",result.stdout)
                self.assertIn("closed=true",result.stdout)
                self.assertEqual(victim.read_bytes(),b"malformed snapshot")
                self.assertTrue(target.exists())

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


class StorageFileTypes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture_directory = tempfile.TemporaryDirectory(prefix="willow-storage-types-")
        cls.fixture = Path(cls.fixture_directory.name)/"storage-types"
        source = Path(__file__).with_name("storage_bridge_types.c")
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-O2", str(source), "-o", str(cls.fixture)], check=True, timeout=30)

    @classmethod
    def tearDownClass(cls):
        cls.fixture_directory.cleanup()

    def call(self, operation, path):
        result = subprocess.run([str(self.fixture), operation, str(path)],
                                capture_output=True, text=True, check=True, timeout=2)
        return int(result.stdout)

    def test_regular_files_keep_read_append_commit_and_recovery(self):
        with tempfile.TemporaryDirectory(prefix="willow-storage-state-") as name:
            root = Path(name)
            target = root/"snapshot"
            target.write_bytes(b"malformed snapshot"); target.chmod(0o600)
            # File formats belong to Hew; the bridge reads regular bytes unchanged.
            self.assertEqual(self.call("read", target), len(b"malformed snapshot"))
            self.assertEqual(self.call("commit", target), 0)
            self.assertEqual(target.read_bytes(), b"snapshot")
            self.assertEqual(self.call("append", target), 0)
            self.assertEqual(target.read_bytes(), b"snapshotsnapshot")
            self.assertEqual(self.call("lock", target), 0)
            pending = Path(str(target)+".pending")
            pending.write_bytes(b"partial"); pending.chmod(0o600)
            self.assertEqual(self.call("recover", target), 0)
            self.assertFalse(pending.exists())
            self.assertEqual(target.read_bytes(), b"snapshotsnapshot")

    def test_special_files_refuse_without_waiting_or_replacing(self):
        with tempfile.TemporaryDirectory(prefix="willow-storage-state-") as name:
            root = Path(name)
            victim = root/"private"
            victim.write_bytes(b"retained"); victim.chmod(0o600)
            for operation in ("read", "lock", "append", "commit", "recover"):
                for kind in ("fifo", "directory", "symlink"):
                    with self.subTest(operation=operation, kind=kind):
                        target = root/(operation+"-"+kind)
                        actual = Path(str(target)+(".lock" if operation=="lock" else
                                                   ".pending" if operation=="recover" else ""))
                        if kind=="fifo": os.mkfifo(actual, 0o600)
                        elif kind=="directory": actual.mkdir(mode=0o700)
                        else: actual.symlink_to(victim)
                        self.assertEqual(self.call(operation, target), 0 if operation=="read" else -1)
                        self.assertTrue(actual.exists())
                        self.assertEqual(victim.read_bytes(), b"retained")
            self.assertEqual(self.call("read", Path("/dev/null")), 0)


if __name__=="__main__":
    unittest.main(verbosity=2)
