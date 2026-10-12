"""Stopped-state and service-switch regressions; no real systemd or radio calls."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import socket
import tarfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "experiments/hew-roles"))
import migrate_base
import willow
spec = importlib.util.spec_from_file_location("installer", ROOT / "tools/install_willow.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class Install(unittest.TestCase):
    def setUp(self):
        self.previous_mask = os.umask(0o077)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.addCleanup(os.umask, self.previous_mask)
        self.root = Path(self.temp.name)

    def base(self, path, sequence):
        path.mkdir(mode=0o700)
        key = bytes([7]) * 32
        (path / "identity.seed").write_bytes(key)
        doc = {"Version": 1, "PublicKey": list(migrate_base.public(key)), "Name": "Retained",
               "Contacts": [], "Channels": [None] * 40, "Sequence": sequence,
               "Messages": [{"Sequence": sequence, "Frame": [17] + [0] * 10, "Digest": [8] * 32}],
               "Preferences": {"version": 1, "path_hash_mode": 2, "rxdelay": 0,
                               "airtime_factor": 0, "txdelay": 0, "direct_txdelay": 0}}
        (path / "companion.json").write_text(json.dumps(doc))

    def test_actual_base_transfer_and_rollback_keep_new_traffic(self):
        old, candidate, recovered = [self.root / n for n in ("old", "candidate", "recovered")]
        self.base(old, 9)
        item = {"kind": "base", "unit": "base.service", "target_install": str(ROOT)}
        installer.stage_state(item, old, candidate, None)
        self.assertEqual((old / "companion.json").read_bytes(), (candidate / "companion.json").read_bytes())
        shutil = __import__("shutil")
        shutil.rmtree(candidate)
        self.base(candidate, 10)
        receipt = {"items": [{**item, "current_install": str(ROOT), "state": str(old),
                              "next_state": str(candidate)}]}
        rollback = installer.rollback_plan(receipt, self.root / "rollback")[0]
        installer.stage_state(rollback, Path(rollback["state"]), recovered, None)
        self.assertEqual(json.loads((recovered / "companion.json").read_text())["Sequence"], 10)
        self.assertEqual((candidate / "identity.seed").read_bytes(), (recovered / "identity.seed").read_bytes())

    def test_actual_host_rebind_keeps_guests_and_opaque_observer_checkpoint(self):
        source, destination = self.root / "host", self.root / "new-host"
        source.mkdir(mode=0o700)
        (source / "config").write_text("worker=/old/worker\n")
        for name in ("relay", "room", "bot"):
            (source / (name + ".seed")).write_bytes(bytes([3]) * 32)
        (source / "native").mkdir(mode=0o700)
        for name, raw in {"native/selected.lua": b"return 'retained'", "native/grants": b"restricted",
                          "native/rollback": b"old source", "observer.outbox": b"opaque checkpoint\x00payload"}.items():
            (source / name).write_bytes(raw)
        worker = {"worker": "/new/worker", "worker_sha256": "a" * 64}
        with patch.object(willow, "verify", return_value=worker):
            willow.rebind(source, destination)
        for name in ("native/selected.lua", "native/grants", "native/rollback", "observer.outbox"):
            self.assertEqual((source / name).read_bytes(), (destination / name).read_bytes())
        self.assertIn("worker=/new/worker", (destination / "config").read_text())
        (source / "observer.outbox.pending").write_bytes(b"unfinished")
        with patch.object(willow, "verify", return_value=worker), self.assertRaisesRegex(ValueError, "unfinished transaction"):
            willow.rebind(source, self.root / "refused")

    def item(self, kind="dashboard"):
        return {"unit": "dashboard.service", "kind": kind, "current_install": "/old-dashboard",
                "target_install": "/new-dashboard", "config": str(self.root / "dashboard.conf"),
                "next_config": str(self.root / "new.conf")}

    def test_override_keeps_environment_and_other_service_options(self):
        old = b"[Service]\nExecStart=\nExecStart=/old/dashboard /old/config\nEnvironmentFile=/private/references\nEnvironment=HEW_WORKERS=4\nMemoryMax=1G\nWorkingDirectory=/old\n"
        new = installer.override_bytes(self.item(), old).decode()
        self.assertIn('"/new-dashboard/experiments/hew-roles/build/hew-dashboard-release"', new)
        self.assertIn("EnvironmentFile=/private/references\nEnvironment=HEW_WORKERS=4\nMemoryMax=1G", new)
        self.assertEqual(new.count("ExecStart="), 2)

    def switch(self, fail=False, stop_failure=False):
        item = self.item()
        Path(item["config"]).write_text("state=/existing/host\npassword_env=PRIVATE_REFERENCE\n")
        override = self.root / ".config/systemd/user/dashboard.service.d/70-willow-current.conf"
        override.parent.mkdir(parents=True)
        original = b"[Service]\nExecStart=\nExecStart=/old/dashboard /old/config\nEnvironmentFile=/private/env\n"
        override.write_bytes(original)
        workspace = self.root / "operation"
        workspace.mkdir()
        guard = {"unit": "host.service", "kind": "host", "current_install": "/retained-host", "selected": False}
        plan = {"services": [guard], "ready_url": "http://fixture/readyz", "status_url": "http://fixture/status"}
        commands = []
        def run(args, **kwargs):
            commands.append(args)
            if stop_failure and args[-2:] == ["stop", "dashboard.service"]:
                raise OSError("uncertain stop")
        def owners(items, candidate=False):
            return {x["unit"]: os.getpid() for x in items}
        real_atomic = installer.atomic
        def atomic(path, data):
            if fail and path == override and b"new-dashboard" in data:
                real_atomic(path, data)
                raise OSError("synthetic override failure after replace")
            real_atomic(path, data)
        with patch.object(Path, "home", return_value=self.root), patch.object(installer, "owners", side_effect=owners), \
             patch.object(installer, "snapshot", return_value={"bot": "key"}), patch.object(installer, "run", side_effect=run), \
             patch.object(installer, "prop", side_effect=lambda u,k: "0" if k == "MainPID" else "inactive"), \
             patch.object(installer, "atomic", side_effect=atomic):
            if stop_failure:
                with self.assertRaisesRegex(OSError, "uncertain stop"):
                    installer.switch(plan, workspace, [item])
                self.assertEqual(json.loads((workspace / "receipt.json").read_text())["stage"], "stop-failed-inspect-current-owners")
                self.assertFalse(any("start" in args for args in commands))
                return
            if fail:
                with self.assertRaisesRegex(OSError, "synthetic"):
                    installer.switch(plan, workspace, [item])
                self.assertEqual(override.read_bytes(), original)
                self.assertEqual(json.loads((workspace / "receipt.json").read_text())["stage"], "staging-failed-original-owners-started")
            else:
                installer.switch(plan, workspace, [item])
                self.assertEqual(Path(item["next_config"]).read_bytes(), Path(item["config"]).read_bytes())
                with self.assertRaisesRegex(ValueError, "Receipt exists"):
                    installer.switch(plan, workspace, [item])
        self.assertTrue(any(args[-2:] == ["start", "dashboard.service"] for args in commands))
        self.assertFalse(any("host.service" in args for args in commands))

    def test_mixed_install_dashboard_switch_preserves_unselected_host(self):
        self.switch()

    def test_failed_staging_restores_original_owner_without_switch_replay(self):
        self.switch(fail=True)

    def test_observer_backlog_guard_distinguishes_old_and_durable_hosts(self):
        stats = {"observed": 8, "published": 7, "dropped": 0, "queued": 0}
        with self.assertRaisesRegex(ValueError, "broker to drain"):
            installer.observer_retention({"observer": {"observer_stats": stats}})
        stats["published"] = 8
        self.assertEqual(installer.observer_retention({"observer": {"observer_stats": stats}}), (False, 0))
        stats.update(published=7, pending=True, dirty=True, saving=True, save_error=0, restored=0)
        self.assertEqual(installer.observer_retention({"observer": {"observer_stats": stats}}), (True, 1))
        stats["save_error"] = -2
        with self.assertRaisesRegex(ValueError, "checkpoint is unavailable"):
            installer.observer_retention({"observer": {"observer_stats": stats}})

    def test_uncertain_stop_is_not_automatically_restarted(self):
        self.switch(stop_failure=True)

    def test_snapshot_retains_regular_sock_filename_and_skips_only_socket(self):
        source = self.root / "state"
        source.mkdir()
        (source / "history.sock").write_bytes(b"regular retained data")
        with socket.socket(socket.AF_UNIX) as connection:
            connection.bind(str(source / "admin.sock"))
            __import__("shutil").copytree(source, self.root / "copied", ignore=installer.socket_entries)
        self.assertEqual((self.root / "copied/history.sock").read_bytes(), b"regular retained data")
        self.assertFalse((self.root / "copied/admin.sock").exists())

    def test_selected_dashboard_build_does_not_build_guarded_host_or_harness(self):
        plan = {"source": str(ROOT), "install": str(self.root / "install"), "revision": "a" * 40,
                "services": [{"kind": "dashboard"}, {"kind": "host", "selected": False}]}
        commands = []
        def archive(args, **kwargs):
            with tarfile.open(fileobj=kwargs["stdout"], mode="w"):
                pass
        def build(args, **kwargs):
            commands.append(args)
            directory = installer.roles(plan["install"]) / "build"
            directory.mkdir(parents=True)
            (directory / "hew-dashboard-release").write_bytes(b"candidate")
        with patch.object(installer, "run", side_effect=archive), \
             patch.object(installer.subprocess, "run", side_effect=build), \
             patch.object(installer.subprocess, "check_output", return_value="hew current"), \
             patch.object(installer.shutil, "which", return_value="/usr/bin/true"):
            installer.build(plan, self.root)
        self.assertEqual(commands, [["make", "-j2", "build/hew-dashboard-release"]])

    def test_build_failure_cannot_stop_any_owner(self):
        plan = {"source": str(ROOT), "install": str(self.root / "install"), "revision": "a" * 40,
                "services": [{"kind": "dashboard"}]}
        calls = []
        def run(args, **kwargs):
            calls.append(args)
            raise OSError("source unavailable")
        with patch.object(installer, "run", side_effect=run), self.assertRaisesRegex(OSError, "source unavailable"):
            installer.build(plan, self.root)
        self.assertFalse(any("systemctl" in x for args in calls for x in args))


if __name__ == "__main__":
    unittest.main()
