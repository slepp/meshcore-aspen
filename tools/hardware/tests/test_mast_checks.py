# SPDX-License-Identifier: Apache-2.0
import hashlib
import struct
import time
import json
import tempfile
import unittest
from unittest.mock import MagicMock, patch

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.hardware import mast_checks as owner_field
from tools.hardware import lua_checks as vm_lab
from tools.hardware import companion_checks as field_stock_test
from tools.hardware import esp32_update as field_release

from tools.hardware.inventory import value as inventory_value


from test_support.operator_inventory import configure_inventory

class Socket:
    def __init__(self, data):
        self.data = bytearray(data)
        self.sent = b""

    def settimeout(self, value):
        self.timeout = value

    def recv(self, size):
        part = bytes(self.data[:min(size, 2)])
        del self.data[:len(part)]
        return part

    def sendall(self, data):
        self.sent += data


class OwnerFieldTests(unittest.TestCase):
    def setUp(self):
        configure_inventory(self)

    def test_stock_management_requires_endpoint_receipt_not_local_ack(self):
        key = "12" * 32
        received = {"type": "PRIV", "pubkey_prefix": key[:12],
                    "text": "ab|PHY=912525000,250000,8,5,1 temp=1", "SNR": 7.25}
        self.assertEqual(field_stock_test.management_reply([received], key, "ab|", "temp=1"), received)
        for changed in (dict(received, type="SENT"), dict(received, pubkey_prefix="34" * 6),
                        dict(received, SNR=None), dict(received, text="ab|temp=0")):
            with self.assertRaisesRegex(ValueError, "stock RF management receipt"):
                field_stock_test.management_reply([changed], key, "ab|", "temp=1")

    def test_stock_management_mode_never_uses_disposable_peer_workflow(self):
        with patch.object(field_stock_test.field, "admin_status", return_value={
                "status": "PHY=912525000,250000,7,5,2 temp=0"}), \
                patch.object(field_stock_test, "management_exercise", return_value={}) as exercise, \
                patch.object(field_stock_test.field, "save"), \
                patch.object(field_stock_test.stock, "verified_backup") as backup, \
                patch.object(field_stock_test.stock, "flash") as flash, \
                patch.object(field_stock_test.stock, "restore") as restore:
            field_stock_test.main(management_only=True, keep_peer=True)
            exercise.assert_called_once_with()
            backup.assert_not_called()
            flash.assert_not_called()
            restore.assert_not_called()
        with self.assertRaisesRegex(ValueError, "--management-only requires"):
            field_stock_test.main(management_only=True)

    def test_rf_management_without_wifi_preserves_authority_and_restores_on_failure(self):
        from tools.hardware import wifi_checks as wifi_lab
        profile = dict(zip(("frequency_hz", "bandwidth_hz", "sf", "cr", "tx_power_dbm"),
                           owner_field.active_profile()), committed=True, fault=False)
        roles = [{"role": "management", "public_key": "12" * 32},
                 {"role": "companion", "public_key": "34" * 32}]
        restored_ssid = b"generated-fixture-network"
        for fail_save in (False, True):
            state = {"wifi": True, "restore": False}
            commands = []

            def checked(_, command):
                commands.append(command)
                if command == "status":
                    return "PHY=912525000,250000,7,5,2 temp=0"
                if command == "wifi status":
                    return f"wifi saved=1 connected={int(state['wifi'])}"
                if command == "wifi apply":
                    state["wifi"] = state["restore"]
                    return "Accepted WiFi reconnect after this reply"
                if command.startswith("wifi ssid "):
                    state["restore"] = command == "wifi ssid " + restored_ssid.hex()
                    return "Saved WiFi field"
                if command.startswith("radio "):
                    if fail_save:
                        raise ValueError("generated radio failure")
                    return "Accepted radio change"
                return {"source hash": "SHA256 " + "ab" * 32 + " gen=79",
                        "role-path": "Role path bytes repeater=3 room=3 companion=3 management=3",
                        "bot status": "ready=1",
                        "job": "Radio applied and saved; effective generation advanced",
                        "reboot": "Accepted reboot"}[command]

            with patch.object(wifi_lab, "environment_file", return_value=owner_field.ROOT / ".env.dev.local"), \
                    patch.object(wifi_lab, "lan_credentials", return_value=(restored_ssid, b"fixture-pass")), \
                    patch.object(owner_field, "dashboard", return_value={"profile": profile, "roles": roles}), \
                    patch.object(owner_field, "NativeClient") as native, \
                    patch.object(owner_field, "connect") as web, \
                    patch.object(owner_field, "checked", side_effect=checked), \
                    patch.object(owner_field, "save") as save, \
                    patch.object(owner_field.time, "sleep"):
                if fail_save:
                    with self.assertRaisesRegex(ValueError, "generated radio failure"):
                        owner_field.rf_management_check()
                else:
                    owner_field.rf_management_check()
                self.assertTrue(state["wifi"])
                self.assertTrue(save.call_args.args[1]["wifi_restored_over_rf"])
                self.assertFalse(save.call_args.args[1]["role_authority_changed"])
                self.assertNotIn(restored_ssid.hex(), json.dumps(save.call_args.args[1]))
                self.assertTrue(all(call.args[3] == roles[0]["public_key"]
                                    for call in native.call_args_list))
                self.assertFalse(any(command.startswith(("trust ", "roles ", "role-path ", "wifi password "))
                                     for command in commands))
                web.assert_not_called()
        with patch.object(wifi_lab, "environment_file", return_value=owner_field.ROOT / ".env"), \
                patch.object(wifi_lab, "lan_credentials") as credentials:
            with self.assertRaisesRegex(ValueError, "authorized"):
                owner_field.rf_management_check()
            credentials.assert_not_called()

    def test_stock_configuration_defaults_to_lab_without_a_profile_flag(self):
        bot, peer = "12" * 32, "34" * 32
        channel = {"channel_idx": 1, "channel_name": owner_field.channel_tag(),
                   "channel_secret": owner_field.hashlib.sha256(owner_field.channel_tag().encode()).digest()[:16].hex()}
        with patch.object(field_stock_test.field, "admin_status", return_value={
                "status": "PHY=912525000,250000,7,5,2 temp=0"}), \
                patch.object(field_stock_test.field, "dashboard", return_value={
                    "roles": [{"role": "command-bot", "public_key": bot}]}), \
                patch.object(field_stock_test.stock_cli, "configure",
                             return_value=("unused", bot, peer)) as configure, \
                patch.object(field_stock_test.stock_cli, "run_cli", return_value=[channel]):
            field_stock_test.configure("fixture", preserve_peer=True)
            configure.assert_called_once_with(public=True, field=False, preserve_identity=True,
                                              name="Cedar-Base")

    def test_explicit_route_probe_requires_physical_three_byte_request(self):
        relay, bot, caller = "ab" * 32, "cd" * 32, "e9" * 32
        reply = {"type": "PRIV", "pubkey_prefix": bot[:12], "txt_type": 0,
                 "path_hash_mode": 2, "path_len": 0, "text": "local; RSSI/SNR unavailable"}
        for direction in ("rx", "tx"):
            event = {"direction": direction, "at_ms": 101, "preview_hex": "0a81ababab",
                     "rssi_dbm": -30}
            with patch.object(field_stock_test, "configure", return_value=(
                    {"repeater": {"public_key": relay}}, bot, caller)), \
                    patch.object(field_stock_test.field, "dashboard", side_effect=[
                        {"uptime_ms": 100}, {"history": {"events": [event]}}]), \
                    patch.object(field_stock_test.stock_cli, "run_cli", return_value=[reply]) as cli:
                if direction == "rx":
                    result = field_stock_test.path_exercise()
                    self.assertEqual(result["route"], "ababab:2")
                    self.assertIn(f"change_path {bot} ababab:2", cli.call_args.args[1])
                else:
                    with self.assertRaisesRegex(ValueError, "No physical RX"):
                        field_stock_test.path_exercise()

    def test_companion_name_uses_native_saved_control_without_key_or_radio_change(self):
        key = "12" * 32
        before = bytearray(58)
        before[0], before[2] = 5, 2
        before[4:36] = bytes.fromhex(key)
        before[48:58] = struct.pack("<IIBB", 912525, 250000, 7, 5)
        after = bytes(before) + b"Aspen-Base"
        with patch.object(owner_field, "Companion") as companion:
            peer = companion.return_value.__enter__.return_value
            peer.public_key, peer.width = key, 3
            peer.command.side_effect = [bytes(before) + b"old", b"\0", after]
            self.assertEqual(owner_field.rename_companion("unused", 5000, "Aspen-Base", key)["name"],
                             "Aspen-Base")
            self.assertEqual(peer.command.call_args_list[1].args[0], b"\x08Aspen-Base")
            peer.command.reset_mock()
            peer.command.side_effect = [after]
            owner_field.rename_companion("unused", 5000, "Aspen-Base", key, apply=False)
            peer.command.assert_called_once()
            self.assertEqual(peer.command.call_args.args[0][0], 1)
            peer.public_key = "34" * 32
            peer.command.reset_mock()
            with self.assertRaisesRegex(ValueError, "before naming"):
                owner_field.rename_companion("unused", 5000, "Aspen-Base", key)
            peer.command.assert_not_called()

    def test_multihop_requires_durable_rollback_to_original_selection(self):
        from tools.hardware import admin as mast_cli
        source = b"function retained() return 'existing' end"
        digest = owner_field.hashlib.sha256(source).hexdigest()
        identities = ("01" * 32, "03" * 32)
        first = {"get radio": "> 912.5250244,250,7,5", "get tx": "> 2",
                 "get repeat": "> on", "get path.hash.mode": "> 2", "stats-packets": "before"}
        for rollback_error in (False, True):
            selected = {"generation": 1, "active": 3, "outcome": "source durably saved and active"}
            client = MagicMock(routed_responses=8, incomplete_routes=9, route_repairs=0)
            nrf = MagicMock()
            nrf.snapshot.side_effect = [
                (first, {}, identities), ({"stats-packets": "after"}, {}, identities)]
            nrf.packet_stats.side_effect = [({"direct_tx": 1}, []), ({"direct_tx": 9}, [])]

            def checked(_, command):
                if command == "source hash":
                    return f"SHA256 {digest} gen={selected['generation']}"
                if command == "source status":
                    return (f"gen={selected['generation']} active={selected['active']} prev=0; " +
                            selected["outcome"])
                self.assertEqual(command, "source rollback")
                selected.update(generation=3, active=3,
                                outcome="Error: activation failed" if rollback_error else
                                "source durably saved and active")
                return "Accepted verification"

            def install(*_):
                selected.update(generation=2, active=0)
                return "source durably saved and active"

            with patch.dict(sys.modules, {"hardware": nrf}), \
                    patch.object(owner_field, "dashboard", side_effect=[
                        {"roles": [{"role": "management", "public_key": "02" * 32}]},
                        {"profile": {"frequency_hz": 912525000, "bandwidth_hz": 250000},
                         "kiss": {"connected": 0}}]), \
                    patch.object(owner_field, "admin_status", return_value={
                        "status": "PHY=912525000,250000,7,5,2 temp=0"}), \
                    patch.object(owner_field, "NativeClient", side_effect=[TimeoutError(), client]), \
                    patch.object(owner_field, "checked", side_effect=checked), \
                    patch.object(owner_field.time, "sleep"), \
                    patch.object(mast_cli, "download", return_value=source), \
                    patch.object(mast_cli, "install", side_effect=install):
                if rollback_error:
                    with self.assertRaisesRegex(ValueError, "rollback failed"):
                        owner_field.multihop_install()
                else:
                    owner_field.multihop_install()
                    self.assertEqual(selected["active"], 3)
                    self.assertTrue(nrf.save.call_args.args[2]["original_source_restored"])
                client.close.assert_called_once()

    def test_lab_admission_retains_peer_without_backup_flash_or_restore(self):
        with patch.object(field_stock_test.field, "admin_status", return_value={
                "status": "PHY=912525000,250000,7,5,2 temp=0"}), \
                patch.object(field_stock_test, "admission_exercise", return_value={}) as exercise, \
                patch.object(field_stock_test.field, "save"), \
                patch.object(field_stock_test.stock, "verified_backup") as backup, \
                patch.object(field_stock_test.stock, "flash") as flash, \
                patch.object(field_stock_test.stock, "restore") as restore:
            field_stock_test.main(admission_only=True, keep_peer=True, lab=True)
            exercise.assert_called_once_with(preserve_peer=True)
            exercise.reset_mock()
            field_stock_test.main(admission_only=True, keep_peer=True)
            exercise.assert_called_once_with(preserve_peer=True)
            backup.assert_not_called()
            flash.assert_not_called()
            restore.assert_not_called()
        with patch.object(field_stock_test.field, "admin_status") as status:
            with self.assertRaisesRegex(ValueError, "--lab requires"):
                field_stock_test.main(lab=True)
            with self.assertRaisesRegex(ValueError, "--path-only requires"):
                field_stock_test.main(path_only=True)
            status.assert_not_called()

    def test_conversation_probe_requires_fresh_replies_and_uses_one_slot(self):
        before = {"uptime_ms": 120000}
        after = {"roles": [{"role": "command-bot", "name": "Birch-Lua", "source_slot": 7, "ready": True, "fault": None}],
                 "history": {"events": [{"direction": "tx", "source_slot": 7,
                                         "state": 2, "at_ms": 120001 + i} for i in range(2)]}}
        rows = [{"type": "CHAN", "channel_idx": 1, "path_hash_mode": 2, "txt_type": 0,
                 "sender_timestamp": 1800000000, "text": "Birch-Lua: " + text}
                for text in ("Pong", "= 42")]
        stats = ["Replies=2 vm-fail=1 notices=1 suppressed=0",
                 "Replies=4 vm-fail=1 notices=1 suppressed=0",
                 "Last=none wait-ms=0 active=0"]
        with patch.object(owner_field, "admin_status", return_value={
                "status": "PHY=912525000,250000,7,5,2 temp=0"}), \
                patch.object(owner_field, "dashboard", side_effect=[before, after]) as dashboard, \
                patch.object(owner_field, "private_file", return_value=b'{"index":1}'), \
                patch.object(owner_field, "Companion") as companion, \
                patch.object(owner_field, "connect"), \
                patch.object(owner_field, "checked", side_effect=stats) as checked, \
                patch.object(owner_field, "cli", return_value=rows) as cli, \
                patch.object(owner_field, "save") as save, \
                patch.object(owner_field.time, "time", return_value=1800000000), \
                patch.object(owner_field.time, "sleep"):
            connection = companion.return_value.__enter__.return_value
            connection.width, connection.channels = 3, 2
            connection.command.return_value = b"\x12\x01" + (inventory_value("channel_tag")).encode("ascii").ljust(32, b"\0") + bytes(16)
            owner_field.conversation_check()
            companion.assert_called_once_with()
            self.assertFalse(save.call_args.args[1]["physical_peer_receipt"])
            self.assertEqual([item for item in cli.call_args.args[1] if item.startswith("chan ")],
                             ["chan 1 '!ping'", "chan 1 '!calc 6*7'"])
            rows[-1]["sender_timestamp"] -= 1
            dashboard.side_effect, checked.side_effect = [before, after], stats
            with self.assertRaisesRegex(ValueError, "Consecutive channel replies missing"):
                owner_field.conversation_check()

    def test_release_verification_uses_only_one_companion_slot(self):
        state = {"keys": {"companion": "12" * 32},
                 "admin": {"source hash": "SHA256 " + "34" * 32 + " gen=1",
                           "bot policy": "unchanged", "room access": "protected"}}
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory, \
                patch.object(field_release, "DIRECTORY", Path(directory)), \
                patch.object(field_release, "private_file", return_value=json.dumps(state).encode()), \
                patch.object(field_release, "current", return_value=state), \
                patch.object(field_release, "save") as save, \
                patch.object(owner_field, "Companion") as companion:
            connection = companion.return_value.__enter__.return_value
            connection.width = 3
            connection.public_key = state["keys"]["companion"]
            field_release.verify()
            companion.assert_called_once_with()
            save.assert_called_once()
            connection.public_key = "56" * 32
            with self.assertRaisesRegex(ValueError, "identity mismatch"):
                field_release.verify()
            self.assertEqual(save.call_count, 1)

    def test_historical_fixture_comparison_is_byte_exact(self):
        cpp = b'const char BotDefaultSource[] = R"lua(\nfunction ping() return "Pong" end\n)lua";'
        expected = b'\nfunction ping() return "Pong" end\n'
        self.assertEqual(field_release.diagnostic_fixture(cpp), expected)
        self.assertNotEqual(field_release.diagnostic_fixture(cpp), expected + b"function custom() end")
        for invalid in (b"", cpp + cpp, cpp.replace(b')lua";', b"truncated")):
            with self.assertRaises(ValueError):
                field_release.diagnostic_fixture(invalid)

    def test_release_allows_only_the_candidate_bundled_source_change(self):
        before = {"keys": {"companion": "12" * 32},
                  "admin": {"source hash": "SHA256 " + "34" * 32,
                            "bot policy": "unchanged", "room access": "protected"}}
        after = {"keys": dict(before["keys"]),
                 "admin": dict(before["admin"], **{"source hash": "SHA256 " + "56" * 32})}
        with patch.object(field_release, "current", return_value=after), \
                patch.object(field_release, "save"), \
                patch.object(owner_field, "Companion") as companion:
            connection = companion.return_value.__enter__.return_value
            connection.width, connection.public_key = 3, before["keys"]["companion"]
            field_release.verify(before, "56" * 32)
            with self.assertRaisesRegex(ValueError, "source hash changed"):
                field_release.verify(before, "34" * 32)
            after["keys"]["companion"] = "78" * 32
            with self.assertRaisesRegex(ValueError, "identities changed"):
                field_release.verify(before, "56" * 32)

    def test_release_deployment_snapshot_wins_over_stale_fixed_artifacts(self):
        before = {"keys": {"companion": "12" * 32},
                  "admin": {"source hash": "SHA256 " + "34" * 32,
                            "bot policy": "unchanged", "room access": "protected"}}
        for kind, expected in (("custom", "34" * 32), ("bundled", "56" * 32)):
            with self.subTest(source=kind), \
                    tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
                directory = Path(directory)
                after = {"keys": dict(before["keys"]),
                         "admin": dict(before["admin"], **{"source hash": "SHA256 " + expected})}
                artifacts = {
                    "before.json": {"keys": {"companion": "ff" * 32}},
                    "bundled.json": {"sha256": "78" * 32},
                    "custom-source.json": {"sha256": "90" * 32},
                    "deployment-1.json": {"before": {"keys": {}}, "expected_source": "aa" * 32},
                    "deployment-2.json": {"before": before, "expected_source": expected},
                }
                for name, value in artifacts.items():
                    path = directory / name
                    path.write_text(json.dumps(value))
                    path.chmod(0o600)
                with patch.object(field_release, "DIRECTORY", directory), \
                        patch.object(field_release, "current", return_value=after), \
                        patch.object(field_release, "save") as save, \
                        patch.object(owner_field, "Companion") as companion:
                    connection = companion.return_value.__enter__.return_value
                    connection.width, connection.public_key = 3, before["keys"]["companion"]
                    field_release.verify()
                    save.assert_called_once()
                    after["admin"]["source hash"] = "SHA256 " + "ab" * 32
                    with self.assertRaisesRegex(ValueError, "source hash changed"):
                        field_release.verify()

    def test_flash_uses_fresh_state_and_distinguishes_custom_from_bundled_source(self):
        before = {"keys": {"companion": "12" * 32},
                  "admin": {"source hash": "SHA256 " + "34" * 32}}
        image = b"application"
        candidate = {"image": "image-1.bin", "partitions": "image-1-partitions.bin",
                     "sha256": hashlib.sha256(image).hexdigest(), "bundled_sha256": "56" * 32}
        for status, expected in (("gen=4; bundled source", "56" * 32),
                                 ("gen=4; custom source", "34" * 32)):
            with self.subTest(source=status), \
                    tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory, \
                    patch.object(field_release, "DIRECTORY", Path(directory)), \
                    patch.object(Path, "glob", return_value=[Path(directory) / "image-1.json"]), \
                    patch.object(field_release, "private_file",
                                 side_effect=[json.dumps(candidate).encode(), image]), \
                    patch.object(field_release, "current", return_value=before), \
                    patch.object(owner_field, "connect"), \
                    patch.object(field_release, "checked", return_value=status), \
                    patch.object(field_release, "save") as save, \
                    patch.object(field_release, "flash_image") as flash, \
                    patch.object(field_release, "verify") as verify:
                field_release.flash()
                flash.assert_called_once_with(Path(directory) / "image-1.bin", "flash")
                verify.assert_called_once_with(before, expected)
                self.assertEqual(save.call_args.args[1]["before"], before)
                self.assertEqual(save.call_args.args[1]["expected_source"], expected)

    def test_extended_stock_reply_requires_current_transport_and_plaintext(self):
        bot = "12" * 32
        reply = {"type": "PRIV", "pubkey_prefix": bot[:12], "text": "Home health: ok",
                 "path_hash_mode": 2, "path_len": 0, "txt_type": 0}
        self.assertEqual(field_stock_test.expected_reply([reply], bot, "Home health: ok"), [reply])
        for changed in (dict(reply, type="CHAN"), dict(reply, pubkey_prefix="34" * 6),
                        dict(reply, path_hash_mode=3), dict(reply, txt_type=1)):
            with self.assertRaises(ValueError):
                field_stock_test.expected_reply([changed], bot, "Home health: ok")
        channel = dict(reply, type="CHAN", channel_idx=1)
        self.assertEqual(field_stock_test.expected_reply([channel], bot, "Home health: ok", True),
                         [channel])
        direct = dict(reply, path_hash_mode=-1, path_len=255)
        self.assertEqual(field_stock_test.expected_reply([direct], bot, "Home health: ok"), [direct])
        learned = dict(reply, path_hash_mode=0, path_len=0)
        self.assertEqual(field_stock_test.expected_reply([learned], bot, "Home health: ok"), [learned])
        for invalid in (dict(direct, path_len=0), dict(direct, type="CHAN", channel_idx=1)):
            with self.assertRaises(ValueError):
                field_stock_test.expected_reply([invalid], bot, "Home health: ok",
                                                channel=invalid["type"] == "CHAN")

    def test_release_rejects_implicit_or_excluded_config_before_access(self):
        with patch.object(field_release, "environment_file", return_value=Path("/not/approved/.env")), \
                patch.object(field_release, "lan_credentials") as credentials:
            with self.assertRaisesRegex(ValueError, "independently authorized"):
                field_release.build()
            credentials.assert_not_called()

    def test_physical_bare_fixture_is_exact_requested_source(self):
        self.assertEqual(vm_lab.fixture(True), b"function hello(name) reply('Hello '..name) end")
        self.assertNotIn(b"command(", vm_lab.fixture(True))
        self.assertEqual(len(vm_lab.fixture()), 4096)

    def test_exact_phy_and_no_temporary_move(self):
        status = {"status": "PHY=912525000,250000,7,5,2 temp=0"}
        owner_field.require_phy(status, owner_field.active_profile())
        for invalid in ("PHY=910525000,250000,7,5,2 temp=0",
                        "PHY=912525000,250000,7,5,2 temp=1"):
            with self.assertRaises(ValueError):
                owner_field.require_phy({"status": invalid}, owner_field.active_profile())

    def test_fragmented_native_frames_and_pushes(self):
        client = owner_field.Companion()
        client.socket = Socket(b">\x01\x00\x80>\x02\x00\x0d\x0d")
        self.assertEqual(client.command(b"\x16\x0d"), b"\x0d\x0d")
        self.assertEqual(client.socket.sent, b"<\x02\x00\x16\x0d")

    def test_bad_native_frame_and_error(self):
        for data in (b"?\x01\x00\0", b">\0\0", b">" + struct.pack("<H", 513),
                     b">\x02\x00\x01\x04"):
            client = owner_field.Companion()
            client.socket = Socket(data)
            with self.assertRaises(ValueError):
                client.command(b"\x16\x0d")

    def test_strict_query_deadline(self):
        client = owner_field.Companion()
        client.socket = Socket(b"data")
        with self.assertRaises(TimeoutError):
            client.read(1, time.monotonic() - 1)

    def test_no_backup_no_real_deployment(self):
        with patch.object(owner_field, "private_file", side_effect=FileNotFoundError), \
                patch.object(owner_field, "connect") as connection:
            with self.assertRaises(FileNotFoundError):
                owner_field.deploy()
            connection.assert_not_called()

    def test_backup_can_precede_checks_but_not_allow_deployment(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            directory = Path(directory)

            def copy_flash(*args, **kwargs):
                with (directory / "pre-real.bin").open("xb") as image:
                    image.truncate(0x800000)

            status = {"status": "PHY=912525000,250000,7,5,2 temp=0"}
            with patch.object(owner_field, "DIRECTORY", directory), \
                    patch.object(owner_field, "admin_status", return_value=status), \
                    patch.object(owner_field, "check") as check, \
                    patch.object(owner_field.subprocess, "run", side_effect=copy_flash), \
                    patch.object(owner_field.time, "sleep"), \
                    patch.object(owner_field, "connect") as connection:
                owner_field.backup()
                saved = json.loads((directory / "pre-real.json").read_text())
                self.assertEqual(saved["mac"], owner_field.device_mac())
                self.assertEqual((directory / "pre-real.bin").stat().st_size, 0x800000)
                with self.assertRaisesRegex(ValueError, "before real deployment"):
                    owner_field.deploy()
                with self.assertRaisesRegex(ValueError, "refusing overwrite"):
                    owner_field.backup()
                check.assert_called_once()
                connection.assert_not_called()

    def test_group_path_width_rejected_before_rf(self):
        with patch.object(owner_field.socket, "create_connection") as connection:
            with self.assertRaisesRegex(ValueError, "path width"):
                owner_field.group_exchange("!ping", "Pong", path_width=4)
            connection.assert_not_called()

    def test_protected_launch_requires_explicit_choice_and_matching_network_evidence(self):
        names = ("repeater", "room", "companion", "management", "command-bot")
        current = {"room access": "Room guest access password-protected",
                   "source hash": "SHA256 " + "a" * 64 + " gen=71"}
        roles = {name: f"{index:064x}" for index, name in enumerate(names)}
        state = {"roles": [{"role": name, "public_key": key, "ready": True}
                           for name, key in roles.items()]}
        evidence = {"roles": roles, "admin": current,
                    "ping": {"reply": "MeshCore command bot: Pong", "repeater_forwarded": True},
                    "signal": {"reply": "RSSI=-40 dBm SNR=10 dB"},
                    "stock_tcp_receive": True, "companion_clients": 2}
        with patch.object(Path, "is_file", return_value=False), \
                patch.object(owner_field, "private_file", return_value=json.dumps(evidence).encode()):
            with self.assertRaisesRegex(ValueError, "before real deployment"):
                owner_field.deployment_evidence(False, current, state)
            acceptance = owner_field.deployment_evidence(True, current, state)
            self.assertFalse(acceptance["room_guest_login"])
            self.assertFalse(acceptance["mobile_app_send_receive"])
            self.assertFalse(acceptance["real_phy_stock_peer"])
            state["roles"][0]["public_key"] = "f" * 64
            with self.assertRaisesRegex(ValueError, "does not match"):
                owner_field.deployment_evidence(True, current, state)
            with self.assertRaisesRegex(ValueError, "before real deployment"):
                owner_field.deployment_evidence(True, dict(current, **{"room access": "Room guest access open"}), state)
        with patch.object(Path, "is_file", return_value=False), \
                patch.object(owner_field, "private_file", side_effect=FileNotFoundError):
            with self.assertRaises(FileNotFoundError):
                owner_field.deployment_evidence(True, current, state)

    def test_field_stock_always_restores_after_flash_or_rf_failure(self):
        for flash_failed in (False, True):
            with patch.object(field_stock_test.field, "admin_status", return_value={}), \
                    patch.object(field_stock_test.field, "require_phy"), \
                    patch.object(field_stock_test.field, "dashboard",
                                 return_value={"kiss": {"connected": 0}, "scheduler": {"queued": 0}}), \
                    patch.object(field_stock_test.stock, "verified_backup"), \
                    patch.object(field_stock_test.stock, "verify",
                                 return_value={"kiss": {"connected": 0}, "scheduler": {"queued": 0}}), \
                    patch.object(field_stock_test.stock, "flash",
                                 side_effect=ValueError("flash") if flash_failed else None), \
                    patch.object(field_stock_test.stock, "restore") as restore, \
                    patch.object(field_stock_test, "exercise", side_effect=ValueError("RF")) as exercise, \
                    patch.object(field_stock_test.time, "sleep"), \
                    patch.object(field_stock_test.field, "save") as save:
                with self.assertRaisesRegex(ValueError, "flash" if flash_failed else "RF"):
                    field_stock_test.main()
                restore.assert_called_once()
                save.assert_not_called()
                if flash_failed:
                    exercise.assert_not_called()

    def test_field_channel_results_do_not_mix_dm_or_other_channels(self):
        good = {"type": "CHAN", "channel_idx": 1, "text": "MeshCore command bot: Pong"}
        self.assertEqual(field_stock_test.channel_messages([
            good, dict(good, type="PRIV"), dict(good, channel_idx=0)]), [good])

    def test_field_tcp_reply_must_match_actual_rf_not_old_retained_history(self):
        pong = {"type": "CHAN", "channel_idx": 1, "text": "MeshCore command bot: Pong",
                "sender_timestamp": 100, "txt_hash": 123, "path_hash_mode": 2, "txt_type": 0, "SNR": 12}
        diagnostic = dict(pong, text="MeshCore command bot: Connected; RSSI=-36 dBm SNR=12 dB")
        self.assertEqual(field_stock_test.verify_channel_messages([pong, diagnostic]), [pong, diagnostic])
        self.assertTrue(field_stock_test.matched_tcp_reply([pong], [pong]))
        for changed in (dict(pong, sender_timestamp=99), dict(pong, txt_hash=124),
                        dict(pong, channel_idx=0), dict(pong, type="PRIV")):
            self.assertFalse(field_stock_test.matched_tcp_reply([changed], [pong]))
        for key, value in (("path_hash_mode", 0), ("txt_type", 1), ("SNR", None)):
            with self.assertRaisesRegex(ValueError, "lack three-byte"):
                field_stock_test.verify_channel_messages([dict(pong, **{key: value}), diagnostic])

    def test_room_admin_uses_only_existing_fixture_and_does_not_claim_guest_login(self):
        status = {"status": "PHY=912525000,250000,7,5,2 temp=0",
                  "room access": "Room guest access password-protected"}
        room = {"role": "room", "ready": True, "public_key": "42" * 32}
        gateway = {"profile": {"frequency_hz": owner_field.active_profile()[0]}, "kiss": {"connected": 0}}
        for failure in (False, True):
            peer = MagicMock()
            with patch.object(owner_field, "admin_status", return_value=status), \
                    patch.object(owner_field, "dashboard", side_effect=[{"roles": [room]}, gateway]), \
                    patch.object(owner_field, "private_file", return_value=b"fixture") as private, \
                    patch.object(owner_field, "NativeClient", return_value=peer,
                                 side_effect=TimeoutError if failure else None) as native, \
                    patch.object(owner_field, "save") as save:
                if failure:
                    with self.assertRaisesRegex(ValueError, "access remains unverified"):
                        owner_field.room_admin_check()
                    save.assert_not_called()
                else:
                    owner_field.room_admin_check()
                    peer.close.assert_called_once()
                    evidence = save.call_args.args[1]
                    self.assertTrue(evidence["independent_rf"])
                    self.assertTrue(evidence["admin_login"])
                    self.assertNotIn("guest_login", evidence)
                private.assert_called_once_with(owner_field.LAB / "role-password", 15)
                native.assert_called_once_with(owner_field.gateway_host(), 8001,
                                               owner_field.LAB / "companion.seed",
                                               room["public_key"], "fixture", room=True)


if __name__ == "__main__":
    unittest.main()
