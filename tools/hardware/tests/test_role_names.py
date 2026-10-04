# SPDX-License-Identifier: Apache-2.0
import struct
import unittest
from datetime import datetime, timezone
from unittest.mock import Mock, patch

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.hardware import mast_checks as owner_field
from tools.hardware import companion_cli as stock_cli_test


from test_support.operator_inventory import configure_inventory

class FleetNamesTests(unittest.TestCase):
    def setUp(self):
        configure_inventory(self)

    def aspen_fixture(self):
        wanted = {"repeater": "Aspen-Relay", "room": "Aspen-Room", "companion": "Aspen-Base",
                  "bot": "Aspen-Bot", "management": "Aspen-Admin", "kiss": "Aspen"}
        names = {role: {"name": name, "public_key": f"{index:02x}" * 32, "advertised": role != "kiss"}
                 for index, (role, name) in enumerate(wanted.items(), 1)}
        roles = [{"role": "command-bot" if role == "bot" else "bot" if role == "kiss" else role,
                  "name": row["name"], "public_key": row["public_key"], "ready": True}
                 for role, row in names.items()]
        return names, {"roles": roles}

    def test_aspen_all_labels_use_owner_and_only_requested_reboot(self):
        names, state = self.aspen_fixture()
        values = {role: row["name"] for role, row in names.items()}
        admin_state = {"status": "PHY=912525000,250000,7,5,2 temp=0",
                       "bot policy": "saved", "role-path": "all=3", "room access": "protected"}

        def command(client, text):
            if text in admin_state:
                return admin_state[text]
            if text == "source hash":
                return "SHA256 unchanged gen=1"
            if text == "reboot":
                return "Reboot queued"
            words = text.split(" ", 3)
            if words[:2] != ["role", "name"]:
                raise AssertionError("Unexpected owner command: " + text)
            if len(words) == 4:
                values[words[2]] = words[3]
                return "Saved"
            return "Name: " + values[words[2]]

        for restart in (False, True):
            with self.subTest(restart=restart), \
                    patch.object(owner_field, "dashboard", return_value=state), \
                    patch.object(owner_field, "connect") as connect, \
                    patch.object(owner_field, "checked", side_effect=command) as checked, \
                    patch.object(owner_field, "Companion") as companion, \
                    patch.object(owner_field.time, "sleep"), \
                    patch.object(owner_field, "private_file", side_effect=AssertionError("role password read")), \
                    patch.object(owner_field, "NativeClient", side_effect=AssertionError("separate role login")):
                peer = companion.return_value.__enter__.return_value
                peer.width, peer.public_key = 3, names["companion"]["public_key"]
                peer.command.return_value = b"\0"
                actual = owner_field.configure_aspen_names(restart=restart)
            self.assertEqual(actual, names)
            commands = [call.args[1] for call in checked.call_args_list]
            setters = [text for text in commands if len(text.split(" ")) == 4]
            self.assertEqual(setters, [f"role name {role} {row['name']}" for role, row in names.items()])
            self.assertEqual(commands.count("reboot"), int(restart))
            self.assertEqual(connect.call_count, 1 + int(restart))
            self.assertTrue(all(not call.args for call in connect.call_args_list))

    def test_aspen_only_notifies_five_roles_and_never_restarts_go(self):
        names, _ = self.aspen_fixture()
        profile = dict(zip(("frequency_hz", "bandwidth_hz", "sf", "cr", "tx_power_dbm"),
                           owner_field.active_profile()), committed=True, fault=False)

        def receive(connection, wanted, since, seconds, observed):
            observed.update({key: {"name": name, "hops": 0, "flood": False} for key, name in wanted.items()})
            return observed

        with patch.object(owner_field, "dashboard", return_value={"profile": profile}), \
                patch.object(owner_field, "configure_aspen_names", return_value=names) as configure, \
                patch.object(owner_field.socket, "create_connection"), \
                patch.object(owner_field, "wait_named_adverts", side_effect=receive) as wait, \
                patch.object(owner_field, "connect"), \
                patch.object(owner_field, "checked", return_value="Queued zero-hop") as checked, \
                patch.object(owner_field, "save") as save, patch.object(owner_field.time, "sleep"), \
                patch.object(owner_field.subprocess, "run", side_effect=AssertionError("service mutation")), \
                patch.object(stock_cli_test, "run_cli", side_effect=AssertionError("Cedar mutation")):
            owner_field.aspen_names()
        configure.assert_called_once_with(restart=False)
        self.assertEqual(len(wait.call_args.args[1]), 5)
        self.assertNotIn(names["kiss"]["public_key"], wait.call_args.args[1])
        self.assertEqual([call.args[1] for call in checked.call_args_list],
                         [f"role advert {role} zerohop" for role, row in names.items() if row["advertised"]])
        self.assertTrue(save.call_args.args[1]["complete"])
        self.assertEqual(len(save.call_args.args[1]["adverts"]), 5)

    def test_aspen_admission_without_reception_is_failure(self):
        names, _ = self.aspen_fixture()
        profile = dict(zip(("frequency_hz", "bandwidth_hz", "sf", "cr", "tx_power_dbm"),
                           owner_field.active_profile()), committed=True, fault=False)
        with patch.object(owner_field, "dashboard", return_value={"profile": profile}), \
                patch.object(owner_field, "configure_aspen_names", return_value=names), \
                patch.object(owner_field.socket, "create_connection"), \
                patch.object(owner_field, "wait_named_adverts", side_effect=ValueError("No RF advert")), \
                patch.object(owner_field, "connect"), \
                patch.object(owner_field, "checked", return_value="Queued zero-hop"), \
                patch.object(owner_field, "save") as save, patch.object(owner_field.time, "sleep"):
            with self.assertRaisesRegex(ValueError, "No RF advert"):
                owner_field.aspen_names()
        self.assertFalse(save.call_args.args[1]["complete"])

    def packet(self, name="Pine-Bot", timestamp=1800000000, route=2, path=b"\0", seed=0):
        key = Ed25519PrivateKey.from_private_bytes(bytes((seed,)) * 32)
        public = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        header = public + struct.pack("<I", timestamp)
        app = b"\x81" + name.encode()
        wire = bytes((0x10 | route,)) + path + header + key.sign(header + app) + app
        escaped = wire.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc")
        return public.hex(), b"\xc0\0" + escaped + b"\xc0"

    def test_only_fresh_exact_remote_zero_hop_name_counts(self):
        key, valid = self.packet()
        frames = [
            self.packet("old-name")[1],
            self.packet(timestamp=1799999999)[1],
            self.packet(route=1, path=b"\x80")[1],
            self.packet(path=b"\x81abc")[1],
            self.packet(seed=1)[1],
            valid,
        ]
        connection = Mock()
        connection.recv.side_effect = frames
        result = owner_field.wait_named_adverts(connection, {key: "Pine-Bot"}, 1800000000)
        self.assertEqual(list(result), [key])
        self.assertEqual(result[key]["name"], "Pine-Bot")
        self.assertEqual(connection.recv.call_count, len(frames))

    def test_all_remote_roles_must_be_received(self):
        key, frame = self.packet()
        other, other_frame = self.packet("Pine-Relay", seed=1)
        connection = Mock()
        connection.recv.side_effect = [frame, frame, other_frame]
        result = owner_field.wait_named_adverts(
            connection, {key: "Pine-Bot", other: "Pine-Relay"}, 1800000000)
        self.assertEqual(set(result), {key, other})
        self.assertEqual(connection.recv.call_count, 3)

    def test_tx_done_and_wrong_advert_do_not_prove_delivery(self):
        key, _ = self.packet()
        connection = Mock()
        connection.recv.side_effect = [b"\xc0\x06\xf8\x01\xc0", self.packet("old-name")[1], b""]
        with self.assertRaisesRegex(ValueError, "Missing independent zero-hop adverts: Pine-Bot"):
            owner_field.wait_named_adverts(connection, {key: "Pine-Bot"}, 1800000000)

    def test_empty_remote_set_is_not_acceptance(self):
        with self.assertRaisesRegex(ValueError, "at least one remote"):
            owner_field.wait_named_adverts(Mock(), {}, 1800000000)

    def test_empty_stock_handshake_has_one_read_only_retry(self):
        info = {"public_key": "stock", "name": "Cedar-Base"}
        with patch("tools.hardware.companion_cli.run_cli", side_effect=[
                stock_cli_test.NoResponseError("empty"), [{"path_hash_mode": 2}, info]]) as run, \
                patch("tools.hardware.companion_device.check") as reset, patch("tools.hardware.mast_checks.time.sleep"):
            self.assertEqual(owner_field.fleet_stock_info("names"), info)
        reset.assert_called_once()
        self.assertEqual(run.call_count, 2)
        self.assertTrue(all(call.args[1] == ["ver", "infos"] for call in run.call_args_list))

    def test_stock_retry_is_bounded_and_other_errors_are_not_replayed(self):
        for error, count in ((stock_cli_test.NoResponseError("empty"), 2), (ValueError("bad reply"), 1)):
            with self.subTest(error=type(error)), patch("tools.hardware.companion_cli.run_cli", side_effect=error) as run, \
                    patch("tools.hardware.companion_device.check"), patch("tools.hardware.mast_checks.time.sleep"):
                with self.assertRaises(type(error)):
                    owner_field.fleet_stock_info("names")
                self.assertEqual(run.call_count, count)

    def test_stock_requires_actual_three_byte_native_mode(self):
        for rows in ([{"public_key": "stock"}],
                     [{"path_hash_mode": 1}, {"public_key": "stock"}]):
            with self.subTest(rows=rows), patch("tools.hardware.companion_cli.run_cli", return_value=rows), \
                    patch("tools.hardware.companion_device.check") as reset:
                with self.assertRaisesRegex(ValueError, "three-byte"):
                    owner_field.fleet_stock_info("names")
                reset.assert_not_called()

    def test_forward_only_clock_already_current_is_read_back(self):
        now = 1800000010
        clock = datetime.fromtimestamp(now, timezone.utc).strftime("%H:%M - %d/%m/%Y UTC")
        nrf = Mock()
        nrf.serial_command.side_effect = ["(ERR: clock cannot go backwards)", clock]
        with patch("tools.hardware.mast_checks.time.time", return_value=now):
            owner_field.sync_pine_clock(nrf, "link")
        self.assertEqual(nrf.serial_command.call_count, 2)
        self.assertEqual(nrf.serial_command.call_args.args, ("link", "clock"))

    def test_clock_rejection_with_wrong_date_or_unrelated_error_is_failure(self):
        for replies in (["(ERR: clock cannot go backwards)", "00:00 - 1/1/2030 UTC"], ["Error: unavailable"]):
            with self.subTest(replies=replies), patch("tools.hardware.mast_checks.time.time", return_value=1800000010):
                nrf = Mock()
                nrf.serial_command.side_effect = replies
                with self.assertRaisesRegex(ValueError, "synchronization failed"):
                    owner_field.sync_pine_clock(nrf, "link")


if __name__ == "__main__":
    unittest.main()
