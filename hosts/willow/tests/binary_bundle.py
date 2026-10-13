#!/usr/bin/env python3
"""Install relocated production binaries and retain current writes through upgrade and rollback."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from service_demo import RunningService, PROFILE, advert, text, body, login, parse
import service_demo
import combined_service

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
import install_willow as updater
from willow import owner_rpc


def run(args):
    return subprocess.run(
        list(map(str, args)), check=True, capture_output=True, text=True, timeout=30
    )


class BinaryBundle(unittest.TestCase):
    def test_relocated_init_current_writes_and_rollback(self):
        bundle = Path(os.environ["WILLOW_TEST_BUNDLE"]).resolve()
        with tempfile.TemporaryDirectory(prefix="willow-bin-", dir="/tmp") as name:
            root = Path(name)
            first, second = (root / p for p in ("install-a", "install-b"))
            extra = bundle / "unexpected-directory"
            extra.symlink_to(root, target_is_directory=True)
            try:
                refused = subprocess.run(
                    [sys.executable, bundle / "install-willow.py", "--prefix", first],
                    capture_output=True,
                    text=True,
                    timeout=15,
                )
                self.assertNotEqual(refused.returncode, 0)
                self.assertIn("symlink or special file", refused.stderr)
                self.assertFalse(first.exists())
            finally:
                extra.unlink()
            for prefix in (first,):
                run(
                    [
                        sys.executable,
                        bundle / "install-willow.py",
                        "--bundle",
                        bundle,
                        "--prefix",
                        prefix,
                    ]
                )
            workspace = root / "upgrade-workspace"
            workspace.mkdir(mode=0o700)
            plan = {
                "source": str(bundle),
                "install": str(second),
                "revision": json.loads((bundle / "manifest.json").read_text())[
                    "source"
                ]["commit"],
                "services": [{"kind": "host"}],
            }
            with patch.object(
                updater.shutil,
                "which",
                side_effect=AssertionError("Binary install must not select a compiler"),
            ):
                result = updater.build(plan, workspace)
            saved = workspace / "build.json"
            saved.write_text(json.dumps({"plan": plan, "build": result}))
            saved.chmod(0o600)
            self.assertEqual(updater.resume_build(plan, workspace), result)
            a, b = (p / "hosts/willow" for p in (first, second))
            new = root / "new-state"
            run(
                [
                    a / "build/hew-host-release",
                    "init",
                    new,
                    "--address",
                    "127.0.0.1",
                    "--port",
                    "8001",
                    "--profile",
                    PROFILE.hex(),
                ]
            )
            before = {
                str(p.relative_to(new)): p.read_bytes()
                for p in new.rglob("*")
                if p.is_file()
            }
            checked = run(
                [sys.executable, a / "tools/willow.py", "check", "--state", new]
            )
            self.assertIn("no modem connection opened", checked.stdout)
            self.assertEqual(
                before,
                {
                    str(p.relative_to(new)): p.read_bytes()
                    for p in new.rglob("*")
                    if p.is_file()
                },
            )
            # Even a rewritten mutable binding record cannot change the digest
            # embedded in the distributed host executable.
            tampered_state = root / "tampered-state"
            run(
                [
                    sys.executable,
                    b / "tools/willow.py",
                    "rebind",
                    "--source",
                    new,
                    "--state",
                    tampered_state,
                ]
            )
            worker = b / "build/native-worker"
            worker.write_bytes(worker.read_bytes() + b"tamper")
            inputs = b / "build/native-worker.inputs"
            inputs.write_text(
                hashlib.sha256(worker.read_bytes()).hexdigest() + "\tnative-worker\n"
            )
            spec = b / "build/native-worker.json"
            data = json.loads(spec.read_text())
            data["inputs_sha256"] = hashlib.sha256(inputs.read_bytes()).hexdigest()
            data["inputs"]["native-worker"] = hashlib.sha256(
                worker.read_bytes()
            ).hexdigest()
            spec.write_text(json.dumps(data))
            refused = subprocess.run(
                [
                    sys.executable,
                    b / "tools/willow.py",
                    "check",
                    "--state",
                    tampered_state,
                ],
                capture_output=True,
                text=True,
                timeout=15,
            )
            self.assertNotEqual(refused.returncode, 0)
            self.assertIn("native worker verification failed", refused.stderr)
            for leaf in ("native-worker", "native-worker.inputs", "native-worker.json"):
                shutil.copy2(bundle / "hosts/willow/build" / leaf, b / "build" / leaf)
            with (
                patch.dict(
                    os.environ,
                    {
                        "MESHCORE_HEW_HOST": str(a / "build/hew-host-release"),
                        "HEW_WORKERS": "4",
                    },
                ),
                patch.object(service_demo, "WORKER", a / "build/native-worker"),
            ):
                with RunningService(
                    root=root / "old-state",
                    extra_config="room.retention=durable-replay\n",
                ) as service:
                    modem = service.emulator
                    keys = {p.name: p.read_bytes() for p in service.root.glob("*.seed")}
                    stamp = 1700000800

                    def note(value, at):
                        modem.send(2, 0, advert(2))
                        deadline = time.monotonic() + 5
                        while True:
                            contacts = owner_rpc(
                                service.root, b"\x01\x04bot contacts"
                            ).decode()
                            if "Contacts=1/16" in contacts:
                                break
                            self.assertLess(time.monotonic(), deadline, contacts)
                            time.sleep(0.02)
                        modem.send(2, 0, text(2, 5, at, value))
                        try:
                            reply = modem.receive(
                                lambda item: item["port"] == 2
                                and parse(item["raw"])[0] == 2
                            )
                        except AssertionError as error:
                            self.fail(
                                f"{error}: {service.logs[-20:]} {service.errors[-20:]}"
                            )
                        return body(reply["raw"], 2, 5).lower()

                    self.assertIn(
                        b"committed", note("!remember bundle retained note", stamp)
                    )
                    modem.send(1, 0, login(stamp=stamp + 1))
                    modem.receive(
                        lambda item: item["port"] == 1 and parse(item["raw"])[0] == 1
                    )
                    modem.send(1, 0, text(2, 4, stamp + 2, "Bundle retained history"))
                    modem.receive(
                        lambda item: item["port"] == 1 and parse(item["raw"])[0] == 3
                    )
                    service.stop_process()
                    for target, destination in (
                        (b, root / "upgraded-state"),
                        (a, root / "rollback-state"),
                    ):
                        run(
                            [
                                sys.executable,
                                target / "tools/willow.py",
                                "rebind",
                                "--source",
                                service.root,
                                "--state",
                                destination,
                            ]
                        )
                        self.assertEqual(
                            keys,
                            {
                                p.name: p.read_bytes()
                                for p in destination.glob("*.seed")
                            },
                        )
                        self.assertIn(
                            b"Bundle retained history",
                            (destination / "room.state").read_bytes(),
                        )
                        service.root = destination
                        modem.identity_root = destination
                        with patch.dict(
                            os.environ,
                            {
                                "MESHCORE_HEW_HOST": str(
                                    target / "build/hew-host-release"
                                )
                            },
                        ):
                            service.start_process(True)
                        stamp += 10
                        expected = b"retained note" if target == b else b"latest note"
                        self.assertIn(expected, note("!recall bundle", stamp))
                        if target == b:
                            self.assertIn(
                                b"committed",
                                note("!remember bundle latest note", stamp + 1),
                            )
                            modem.send(1, 0, login(stamp=stamp + 2))
                            modem.receive(
                                lambda item: item["port"] == 1
                                and parse(item["raw"])[0] == 1
                            )
                            modem.send(
                                1, 0, text(2, 4, stamp + 3, "Bundle upgraded history")
                            )
                            modem.receive(
                                lambda item: item["port"] == 1
                                and parse(item["raw"])[0] == 3
                            )
                        else:
                            self.assertIn(
                                b"Bundle upgraded history",
                                (destination / "room.state").read_bytes(),
                            )
                        service.stop_process()
            # The existing combined scenario exercises all four installed
            # services, observer MQTT, companion state and dashboard controls.
            with (
                patch.object(combined_service, "ROOT", a),
                patch.object(service_demo, "WORKER", a / "build/native-worker"),
                patch.dict(
                    os.environ,
                    {
                        "MESHCORE_HEW_HOST": str(a / "build/hew-host-release"),
                        "HEW_WORKERS": "4",
                    },
                ),
            ):
                combined_service.CombinedService(
                    "test_native_endpoints_observations_restarts_and_latest_rollback"
                ).debug()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    args = parser.parse_args()
    os.environ["WILLOW_TEST_BUNDLE"] = str(args.bundle)
    unittest.main(argv=[sys.argv[0]], verbosity=2)
