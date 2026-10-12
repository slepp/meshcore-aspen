"""Worker verification uses private synthetic inputs, never edits the real manifest."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
import build_worker
from migrate_go import config_file
from reconcile_go import settings
from willow import check, rebind

PROFILE = "c806643690d003000705020000803f000000"
SECRET = "do-not-print-7f3a"
INIT_PROFILE = "c806643690d003000705020000803f010000"


def host():
    return os.environ.get("MESHCORE_HEW_HOST",str(ROOT/"build/hew-host"))


def init(state, *extra):
    return subprocess.run([host(),"init",str(state),"--address","127.0.0.1","--port","5000",
                           "--profile",INIT_PROFILE,*extra],capture_output=True,text=True,timeout=10)


class WorkerBinding(unittest.TestCase):
    def test_offline_check_reuses_startup_validation_without_connections_or_writes(self):
        root = ROOT/"build"/f"offline-config-{os.getpid()}"
        root.mkdir(mode=0o700)
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(0.05)
        try:
            for role in ("relay", "room", "bot", "observer"):
                seed = root/(role+".seed")
                seed.write_bytes(bytes(range(32)))
                seed.chmod(0o600)
            pending = root/"observer.initialized.pending"
            pending.write_bytes(b"retained unfinished marker")
            pending.chmod(0o600)
            base = (f"address=127.0.0.1\nport={listener.getsockname()[1]}\nprofile={PROFILE}\n"
                    f"worker=-\npassword={SECRET}\nobserver.enabled=1\n"
                    f"observer.url=tcp://127.0.0.1:{listener.getsockname()[1]}\n")
            cases = (
                (base, None),
                (base.replace("127.0.0.1", "localhost", 1), "numeric IPv4"),
                (base.replace(f"port={listener.getsockname()[1]}", "port=70000"), "port must be 1..65535"),
                (base+"width=4\n", "width must be 1..3"),
                (base.replace(PROFILE, "00"*18), "invalid shared modem profile"),
                (base+"relay.bogus=1\n", "invalid relay settings"),
                (base+"room.preference_profile=bogus\n", "invalid room settings"),
                (base+"observer.queue_size=0\n", "observer"),
            )
            for text, error in cases:
                with self.subTest(error=error):
                    config = root/"config"
                    config.write_text(text)
                    config.chmod(0o600)
                    before = {path.name: (path.read_bytes(), path.stat().st_mode, path.stat().st_mtime_ns)
                              for path in root.iterdir()}
                    commands = ([sys.executable, "-B", str(ROOT/"willow.py"), "check", "--state", str(root)],
                                [host(), "check-config", str(root)])
                    for command in commands:
                        completed = subprocess.run(command, capture_output=True, text=True, timeout=5)
                        output = completed.stdout+completed.stderr
                        self.assertEqual(completed.returncode == 0, error is None, output)
                        if error:
                            self.assertIn(error, output)
                        self.assertNotIn(SECRET, output)
                        self.assertNotIn("ONLINE", output)
                        self.assertEqual(before, {path.name: (path.read_bytes(), path.stat().st_mode, path.stat().st_mtime_ns)
                                                 for path in root.iterdir()})
                        with self.assertRaises(socket.timeout):
                            listener.accept()
        finally:
            listener.close()
            shutil.rmtree(root)

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
            text = f"address=127.0.0.1\nport=1\nprofile={PROFILE}\nworker=-\nworker=/bin/true\n"
            self.assertEqual(settings(text.encode())["worker"],"/bin/true")
            config.write_text(text)
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

    def test_configuration_errors_name_line_and_key_without_values(self):
        base = f"address=127.0.0.1\nport=1\nprofile={PROFILE}\npassword={SECRET}\n"
        cases = (
            (base+SECRET+"\n", "configuration line 5: expected key=value"),
            (base+"pasword=x\n", "configuration line 5: unknown key pasword"),
            (base.replace("port=1","port=one"), "configuration line 2: port must be an integer"),
            (base.replace("port=1","port=70000"), "invalid configuration: port must be 1..65535"),
            (base+"admin="+SECRET*5+"\n", "invalid configuration: admin must be at most 63 bytes"),
            (base+"imported=yes\n", "line 5: imported must be 0 or 1"),
        )
        root = ROOT/"build"/f"config-errors-{os.getpid()}"
        root.mkdir(mode=0o700)
        try:
            config = root/"config"
            for text, message in cases:
                with self.subTest(message=message):
                    config.write_text(text)
                    config.chmod(0o600)
                    completed = subprocess.run([os.environ.get("MESHCORE_HEW_HOST",str(ROOT/"build/hew-host")),str(root)],
                                               capture_output=True,text=True,timeout=5)
                    output = completed.stderr+completed.stdout
                    self.assertNotEqual(completed.returncode,0)
                    self.assertIn(message,output)
                    self.assertNotIn(SECRET,output)
                    self.assertNotIn("ONLINE",completed.stdout)
            self.assertFalse(list(root.glob("*.seed")))
        finally:
            shutil.rmtree(root)

    def test_check_configuration_errors_name_line_and_key_without_values(self):
        base = f"address=127.0.0.1\npassword={SECRET}\n".encode()
        for text, message in (
            (base+SECRET.encode()+b"\n", "Willow config line 3: expected key=value"),
            (base+b"pasword=x\n", "Willow config line 3: unknown key pasword"),
            (base+b"relay.bogus=1\n", "Willow config line 3: unknown key relay.bogus"),
            (base+b"room.preference_profile="+SECRET.encode()+b"\n", "Willow config line 3: room.preference_profile must be"),
        ):
            with self.subTest(message=message):
                with self.assertRaises(ValueError) as caught:
                    settings(text)
                self.assertIn(message,str(caught.exception))
                self.assertNotIn(SECRET,str(caught.exception))
        self.assertEqual(settings(base)["password"],SECRET)
        duplicate = base+b"password=later\n"
        with self.assertRaises(ValueError) as caught:
            settings(duplicate, strict=True)
        self.assertIn("Willow config line 3: duplicate key password",str(caught.exception))
        self.assertNotIn(SECRET,str(caught.exception))
        self.assertEqual(settings(base+b"password=later\n")["password"],"later")
        root = ROOT/"build"/f"migration-config-{os.getpid()}"
        root.mkdir(mode=0o700)
        try:
            path = root/"config"
            for text, message in (
                (base+SECRET.encode()+b"\n", "configuration line 3: expected key=value"),
                (base+b"pasword=x\n", "configuration line 3: unknown key pasword"),
            ):
                with self.subTest(source="migration", message=message):
                    path.write_bytes(text)
                    path.chmod(0o600)
                    with self.assertRaises(ValueError) as caught:
                        config_file(path)
                    self.assertIn(message,str(caught.exception))
                    self.assertNotIn(SECRET,str(caught.exception))
        finally:
            shutil.rmtree(root)

    def test_failed_build_does_not_leave_a_stale_binding(self):
        root = ROOT/"build"/f"worker-retry-{os.getpid()}"
        root.mkdir(mode=0o700)
        try:
            native = root/"native"
            native.mkdir()
            cached = native/"wamr/host/cached.o"
            cached.parent.mkdir(parents=True)
            cached.write_bytes(b"previous object")
            spec = root/"native-worker.json"
            spec.write_text(json.dumps({"worker": str(root/"native-worker"), "local_inputs": {},
                                        "inputs": {str(cached): hashlib.sha256(b"previous object").hexdigest()}}))
            def failed_build(command, *args, **kwargs):
                cached.write_bytes(b"partially rebuilt object")
                raise subprocess.CalledProcessError(2, command)
            with patch.multiple(build_worker, BUILD=root, NATIVE=native, SPEC=spec,
                                WORKER=root/"native-worker", run=failed_build):
                for attempt in range(2):
                    with self.subTest(attempt=attempt):
                        with self.assertRaises(subprocess.CalledProcessError):
                            build_worker.ensure()
                        self.assertFalse(spec.exists())
                with self.assertRaisesRegex(ValueError, "build record missing"):
                    build_worker.verify()
        finally:
            shutil.rmtree(root)


    def test_init_creates_private_state_without_printing_credentials(self):
        root = ROOT/"build"/f"init-{os.getpid()}"
        root.mkdir(mode=0o700)
        try:
            state = root/"state"
            completed = init(state)
            self.assertEqual(completed.returncode,0,completed.stderr)
            self.assertFalse((root/"state.init").exists())
            for path in (state,state/"native",state/"native/nvs",state/"native/spiffs"):
                self.assertEqual(path.stat().st_mode & 0o777,0o700)
            for name in ("config","relay.seed","room.seed","bot.seed"):
                self.assertEqual((state/name).stat().st_mode & 0o777,0o600)
            config = settings((state/"config").read_bytes(),strict=True)
            self.assertEqual(set(config),{"address","port","profile","worker","admin","room.password"})
            self.assertEqual(config["worker"],str(ROOT/"build/native-worker"))
            self.assertEqual(len(config["admin"]),15)
            self.assertNotEqual(config["admin"],config["room.password"])
            for secret in (config["admin"],config["room.password"]):
                self.assertNotIn(secret,completed.stdout+completed.stderr)
            self.assertIn("INIT_OK",completed.stdout)
            self.assertIn("next: python3 -B willow.py check",completed.stdout)
            check(state)
            again = init(state)
            self.assertNotEqual(again.returncode,0)
            self.assertIn("already exists",again.stderr+again.stdout)
            self.assertEqual(settings((state/"config").read_bytes()),config)

            admin, room = root/"admin", root/"room"
            admin.write_text(SECRET[:15]+"\n")
            room.write_text("room-"+SECRET[:6]+"\n")
            admin.chmod(0o600)
            room.chmod(0o600)
            imported = init(root/"imported","--admin-file",str(admin),"--room-password-file",str(room))
            self.assertEqual(imported.returncode,0,imported.stderr)
            self.assertIn("admin=imported room_password=imported",imported.stdout)
            self.assertNotIn(SECRET[:15],imported.stdout+imported.stderr)
            config = settings((root/"imported/config").read_bytes())
            self.assertEqual((config["admin"],config["room.password"]),(SECRET[:15],"room-"+SECRET[:6]))
        finally:
            shutil.rmtree(root)

    def test_initialized_willow_upgrade_keeps_identity_and_native_data(self):
        root = ROOT/"build"/f"native-upgrade-{os.getpid()}"
        root.mkdir(mode=0o700)
        try:
            source, candidate = root/"source", root/"candidate"
            completed = init(source)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            config = source/"config"
            config.write_text(config.read_text().replace(str(ROOT/"build/native-worker"),
                                                        "/previous/willow/build/native-worker"))
            retained = source/"native/spiffs/retained-note"
            retained.write_bytes(b"retained program data")
            retained.chmod(0o600)
            (source/"service.lock").touch(mode=0o600)
            before = {str(p.relative_to(source)): p.read_bytes() for p in source.rglob("*") if p.is_file()}
            result = rebind(source, candidate)
            self.assertEqual(result["mode"], "rebound")
            self.assertEqual(check(candidate)["worker"], str(ROOT/"build/native-worker"))
            self.assertFalse((candidate/"migration.json").exists())
            for name, data in before.items():
                self.assertEqual((source/name).read_bytes(), data)
                if name != "config":
                    self.assertEqual((candidate/name).read_bytes(), data)
            with self.assertRaisesRegex(ValueError, "NEW and disjoint"):
                rebind(source, candidate)
            with (source/"service.lock").open("rb") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX|fcntl.LOCK_NB)
                with self.assertRaisesRegex(ValueError, "locked; stop the writer"):
                    rebind(source, root/"locked")
            self.assertFalse((root/"locked").exists())
            import willow
            original_write = willow.write_private
            def interrupted(path, data):
                if path.name == "retained-note":
                    raise OSError("synthetic copy failure")
                original_write(path, data)
            failed = root/"interrupted"
            with patch("willow.write_private", side_effect=interrupted):
                with self.assertRaisesRegex(OSError, "synthetic copy failure"):
                    rebind(source, failed)
            self.assertFalse(failed.exists())
            original_check = willow.check
            def invalid_candidate(path, **kwargs):
                if path != source.resolve():
                    raise ValueError("synthetic candidate validation failure")
                return original_check(path, **kwargs)
            with patch("willow.check", side_effect=invalid_candidate):
                with self.assertRaisesRegex(ValueError, "synthetic candidate validation failure"):
                    rebind(source, failed)
            self.assertFalse(failed.exists())
            with patch("willow.os.rename", side_effect=OSError("synthetic rename failure")):
                with self.assertRaisesRegex(OSError, "synthetic rename failure"):
                    rebind(source, failed)
            self.assertFalse(failed.exists())
            self.assertFalse(list(root.glob(".*.rebind-*")))
            for name, data in before.items():
                self.assertEqual((source/name).read_bytes(), data)
            rebind(source, failed)
            self.assertEqual(check(failed)["worker"], str(ROOT/"build/native-worker"))
        finally:
            shutil.rmtree(root)

    def test_init_rejects_invalid_input_before_creating_state(self):
        root = ROOT/"build"/f"init-reject-{os.getpid()}"
        root.mkdir(mode=0o700)
        try:
            exposed, long, same = root/"exposed", root/"long", root/"same"
            exposed.write_text(SECRET)
            exposed.chmod(0o644)
            long.write_text(SECRET*2)
            long.chmod(0o600)
            same.write_text(SECRET[:15])
            same.chmod(0o600)
            cases = (
                (["--profile","00"*18], "invalid shared modem profile"),
                (["--profile","zz"], "--profile must be"),
                (["--port","0"], "--port must be 1..65535"),
                (["--address",""], "--address is required"),
                (["--bogus","1"], "unknown option --bogus"),
                (["--admin-file",str(exposed)], "mode 0600"),
                (["--room-password-file",str(long)], "1..15 bytes"),
                (["--admin-file",str(same),"--room-password-file",str(same)], "must differ"),
            )
            for extra, message in cases:
                with self.subTest(message=message):
                    completed = init(root/"state",*extra)
                    output = completed.stdout+completed.stderr
                    self.assertNotEqual(completed.returncode,0)
                    self.assertIn(message,output)
                    self.assertNotIn(SECRET[:15],output)
                    self.assertFalse((root/"state").exists())
                    self.assertFalse((root/"state.init").exists())
        finally:
            shutil.rmtree(root)


if __name__ == "__main__":
    unittest.main(verbosity=2)
