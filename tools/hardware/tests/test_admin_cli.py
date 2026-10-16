# SPDX-License-Identifier: Apache-2.0
import io
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch

from tools import admin as cli
from tools.hardware import admin

ROOT = Path(__file__).resolve().parents[3]


class AdminCLITests(unittest.TestCase):
    def test_backup_inputs_reject_fifos_without_waiting_for_a_writer(self):
        with tempfile.TemporaryDirectory() as directory:
            seed = Path(directory) / "seed"
            seed.write_bytes(bytes(range(32)))
            seed.chmod(0o600)
            fifo = Path(directory) / "pipe"
            os.mkfifo(fifo, 0o600)
            for backup, key in ((fifo, seed), (seed, fifo)):
                result = subprocess.run([sys.executable, str(ROOT / "tools/admin.py"), "backup", "inspect",
                                         str(backup), "--key-file", str(key)],
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    def test_package_workflow_from_outside_the_checkout(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "greet.lua"
            source.write_text("function greet(name) reply('Hello '..name) end\n")
            package = Path(directory) / "greet.package.lua"
            command = [sys.executable, str(ROOT / "tools/admin.py"), "package"]
            subprocess.run(command + ["create", str(source), str(package), "--name", "greet",
                           "--version", "1.0.0"], cwd=directory, check=True, capture_output=True)
            result = subprocess.run(command + ["inspect", str(package)], cwd=directory,
                                    check=True, capture_output=True, text=True)
            self.assertIn("greet", result.stdout)
            self.assertTrue(package.read_bytes().endswith(source.read_bytes()))

    def test_hardware_monitor_cli_can_load_its_dependencies(self):
        result = subprocess.run([sys.executable, str(ROOT / "test_support/hardware/live_multiclient.py"), "--help"],
                                cwd=ROOT, capture_output=True, text=True, check=True)
        self.assertIn("--receive-only", result.stdout)

    def test_telemetry_credentials_commit_before_enabling(self):
        with tempfile.TemporaryDirectory() as directory:
            ca = Path(directory) / "ca.pem"
            ca.write_text(ssl.DER_cert_to_PEM_cert(ssl.create_default_context().get_ca_certs(binary_form=True)[0]))
            token = Path(directory) / "token"
            token.write_bytes(b"example-token")
            token.chmod(0o600)
            seed = Path(directory) / "seed"
            seed.write_bytes(bytes(range(32)))
            seed.chmod(0o600)
            client = MagicMock(command=MagicMock(return_value="Saved"))
            arguments = ["admin.py", "--gateway", "modem", "--seed-file", str(seed),
                         "--target", "ab" * 32, "telemetry", "configure", "--endpoint", "https://metrics.example/write",
                         "--address", "192.0.2.1", "--ca-file", str(ca), "--token-file", str(token)]
            with patch.object(sys, "argv", arguments), patch.object(admin, "NativeClient", return_value=client), \
                    patch("sys.stdout", new_callable=io.StringIO):
                cli.main()
            commands = [call.args[0] for call in client.command.call_args_list]
            self.assertLess(commands.index("telemetry endpoint commit"), commands.index("telemetry on"))
            for name, expected in (("ca", ca.read_bytes()), ("token", token.read_bytes())):
                chunks = [text.rsplit(" ", 1)[1] for text in commands
                          if text.startswith(f"telemetry endpoint {name} ") and not text.endswith("clear")]
                self.assertEqual(bytes.fromhex("".join(chunks)), expected)
            client.close.assert_called_once()
            empty_ca = Path(directory) / "empty-ca.pem"
            empty_ca.write_bytes(b"")
            for option, value in (("--endpoint", "http://metrics.example/write"), ("--interval", "0"),
                                  ("--endpoint", "https://metrics.example:0/write"), ("--ca-file", str(empty_ca))):
                invalid = arguments.copy()
                if option in invalid:
                    invalid[invalid.index(option) + 1] = value
                else:
                    invalid += [option, value]
                with patch.object(sys, "argv", invalid), patch.object(admin, "NativeClient") as connect, \
                        patch("sys.stderr", new_callable=io.StringIO), self.assertRaises(SystemExit):
                    cli.main()
                connect.assert_not_called()

    def test_telemetry_credentials_reject_web_before_connecting(self):
        with patch.object(sys, "argv", ["admin.py", "--web", "http://node", "telemetry", "configure",
                                       "--endpoint", "https://metrics.example/write", "--ca-file", "unused"]), \
                patch.object(admin, "WebClient") as connect, patch("sys.stderr", new_callable=io.StringIO), \
                self.assertRaises(SystemExit):
            cli.main()
        connect.assert_not_called()
