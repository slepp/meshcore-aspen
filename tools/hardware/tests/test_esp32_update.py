# SPDX-License-Identifier: Apache-2.0
import os
import tempfile
import unittest
from unittest.mock import MagicMock, patch

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.hardware import esp32_update as field_release

from tools.hardware.inventory import value as inventory_value


from test_support.operator_inventory import configure_inventory

class TelemetryDeploymentTests(unittest.TestCase):
    def setUp(self):
        configure_inventory(self)

    def test_only_authenticated_aspen_is_enabled_after_endpoint_commit(self):
        with tempfile.TemporaryDirectory(dir=field_release.field.ROOT / ".tmp") as directory:
            ca = Path(directory) / "ca.pem"
            ca.write_bytes(b"-----BEGIN CERTIFICATE-----\n" + b"x" * 150 + b"\n-----END CERTIFICATE-----")
            client = MagicMock()
            commands = []
            identity = field_release.telemetry_policy()["device"]

            def checked(_, command):
                commands.append(command)
                if command == "telemetry identity":
                    return identity
                if command == "telemetry endpoint status":
                    return "Telemetry endpoint configured=1 staged=0 ca-bytes=207 auth=0"
                if command == "telemetry status":
                    return "Telemetry on=1 interval=60 pending=0"
                if command == "telemetry endpoint host":
                    return "metrics.example.invalid"
                if command == "telemetry endpoint path":
                    return "/write"
                return "Saved"

            with patch.dict(os.environ, {"TELEMETRY_CA_FILE": str(ca),
                                         "TELEMETRY_MAST_KEY": "ab" * 32}, clear=True), \
                    patch.object(field_release.field, "mast_host", return_value=inventory_value("mast_host")), \
                    patch.object(field_release.field, "NativeClient", return_value=client) as native, \
                    patch.object(field_release.field, "connect") as web, \
                    patch.object(field_release, "private_file", return_value=b"test-password"), \
                    patch.object(field_release, "checked", side_effect=checked), \
                    patch.object(field_release.socket, "gethostbyname", return_value="192.0.2.10"), \
                    patch.object(field_release.socket, "create_connection"), \
                    patch.object(field_release.ssl, "create_default_context") as context:
                field_release.telemetry()
                native.assert_called_once()
                self.assertEqual(native.call_args.args[3], "ab" * 32)
                web.assert_not_called()
                context.assert_called_once_with(cadata=ca.read_text())
                self.assertEqual(commands[0], "telemetry identity")
                self.assertEqual(commands[1], "telemetry off")
                self.assertLess(commands.index("telemetry endpoint commit"), commands.index("telemetry on"))
                self.assertTrue(all(len(command) <= 145 for command in commands))
                chunks = [command.split()[-1] for command in commands
                          if command.startswith("telemetry endpoint ca ") and not command.endswith("clear")]
                self.assertEqual(bytes.fromhex("".join(chunks)), ca.read_bytes())
                client.close.assert_called_once()
                commands.clear()
                identity = "esp32-another-device"
                with self.assertRaisesRegex(ValueError, "not Aspen"):
                    field_release.telemetry()
                self.assertEqual(commands, ["telemetry identity"])

    def test_different_deployment_host_cannot_connect(self):
        with patch.object(field_release.field, "mast_host", return_value="192.0.2.99"), \
                patch.object(field_release.field, "NativeClient") as connect:
            with self.assertRaisesRegex(ValueError, "authorized only"):
                field_release.telemetry()
            connect.assert_not_called()

    def test_ca_is_explicit_never_an_insecure_fallback(self):
        with patch.dict(os.environ, {"TELEMETRY_MAST_KEY": "ab" * 32}, clear=True):
            with self.assertRaisesRegex(ValueError, "TELEMETRY_CA_FILE"):
                field_release.telemetry()

    def test_rf_target_key_is_owner_verified_not_http_discovered(self):
        with patch.dict(os.environ, {}, clear=True), \
                patch.object(field_release.field, "NativeClient") as native:
            with self.assertRaisesRegex(ValueError, "TELEMETRY_MAST_KEY"):
                field_release.telemetry()
            native.assert_not_called()


if __name__ == "__main__":
    unittest.main()
