# SPDX-License-Identifier: Apache-2.0
import struct
import json
import tempfile
import unittest
from unittest.mock import Mock, patch

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.hardware import radio_checks as operator_radio


from test_support.operator_inventory import configure_inventory

class OperatorRadioTests(unittest.TestCase):
    def setUp(self):
        configure_inventory(self)

    def test_native_signed_flood_advert(self):
        key = Ed25519PrivateKey.from_private_bytes(bytes(range(32)))
        public = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        header = public + struct.pack("<I", 1800000000)
        app = b"\x81test"
        wire = b"\x11\x80" + header + key.sign(header + app) + app
        advert = operator_radio.signed_advert(wire)
        self.assertEqual(operator_radio.signed_advert(bytearray(wire)), advert)
        self.assertTrue(advert["flood"])
        self.assertEqual(advert["public_key"], public.hex())
        self.assertEqual(advert["path_bytes"], 3)
        self.assertEqual(advert["hops"], 0)
        self.assertEqual(advert["name"], "test")
        self.assertIsNone(operator_radio.signed_advert(wire[:-1] + b"X"))
        self.assertIsNone(operator_radio.signed_advert(b"\x51" + wire[1:]))
        self.assertIsNone(operator_radio.signed_advert(b"\x11\xc0" + wire[2:]))
        self.assertFalse(operator_radio.signed_advert(b"\x12" + wire[1:])["flood"])

    def test_advert_names_follow_native_optional_fields(self):
        key = Ed25519PrivateKey.from_private_bytes(bytes(range(32)))
        public = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        header = public + struct.pack("<I", 1800000000)
        for flags in range(8):
            extra = bytes((8 if flags & 1 else 0) + (2 if flags & 2 else 0) + (2 if flags & 4 else 0))
            app = bytes((0x81 | flags << 4,)) + extra + b"Pine-Bot"
            wire = b"\x12\0" + header + key.sign(header + app) + app
            self.assertEqual(operator_radio.signed_advert(wire)["name"], "Pine-Bot")
        for app in (b"\x91short", b"\x81\xff"):
            wire = b"\x12\0" + header + key.sign(header + app) + app
            self.assertIsNone(operator_radio.signed_advert(wire))

    def test_operator_contact_missing_is_not_delivery(self):
        class Client:
            def command(self, frame, allow_error=False):
                if frame != b"\x1e" + operator_radio.dm_target() or not allow_error:
                    raise ValueError("Unexpected native contact query")
                return b"\x01\x02"
        self.assertIsNone(operator_radio.operator_contact(Client()))

    def test_discovered_route_bounds_and_principal(self):
        prefix = b"\x8d\0" + operator_radio.dm_target()[:6]
        self.assertEqual(operator_radio.discovered_path(prefix + b"\x81abc\x80"),
                         (0x81, b"abc"))
        self.assertEqual(operator_radio.discovered_path(prefix + b"\x80\x80"), (0x80, b""))
        for frame in (prefix, prefix + b"\xc0\x80", prefix + b"\x81ab",
                      prefix + b"\x81abc", prefix + b"\x80\x80x",
                      prefix + b"\x96" + bytes(66) + b"\x80",
                      b"\x8d\0" + bytes(6) + b"\x80\x80"):
            with self.assertRaises(ValueError):
                operator_radio.discovered_path(frame)

    def test_push_timeout_is_not_ack(self):
        client = Mock(pushes=[b"\x82abcd" + bytes(4)])
        client.frame.side_effect = TimeoutError
        self.assertIsNone(operator_radio.wait_push(client, lambda frame: frame[1:5] == b"efgh", 1))
        client.pushes = [b"\x82efgh" + bytes(4)]
        self.assertEqual(operator_radio.wait_push(client, lambda frame: frame[1:5] == b"efgh", 1),
                         b"\x82efgh" + bytes(4))

    def test_send_admission_is_not_delivery(self):
        receipt = operator_radio.sent_reply(b"\x06\0abcd" + struct.pack("<I", 1000))
        self.assertEqual(receipt, {"flood": False, "tag": "61626364", "timeout_ms": 1000})
        for frame in (b"\0", b"\x01\x02", b"\x06\x02abcd" + bytes(4)):
            with self.assertRaises(ValueError):
                operator_radio.sent_reply(frame)

    def test_dm_requires_route_and_never_replays(self):
        for route_found, acknowledged in ((False, False), (True, False), (True, True)):
            with self.subTest(route_found=route_found, acknowledged=acknowledged), \
                    tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
                contact = bytearray(148)
                contact[0], contact[33], contact[35] = 3, 1, 255
                contact[1:33] = operator_radio.dm_target()
                learned = bytearray(contact)
                learned[35] = 0x80
                client = Mock(public_key="sender", pushes=[])
                client.__enter__ = Mock(return_value=client)
                client.__exit__ = Mock(return_value=False)
                client.command.side_effect = [
                    bytes(contact), b"\x0bexport",
                    b"\x06\x01abcd" + struct.pack("<I", 1000),
                    bytes(contact), b"\0", bytes(learned),
                    b"\x06\0efgh" + struct.pack("<I", 1000)]
                pushes = [b"\x8d\0" + operator_radio.dm_target()[:6] + b"\x80\x80" if route_found else None,
                          b"\x82efgh" + struct.pack("<I", 900) if acknowledged else None]
                with patch.object(operator_radio, "DIRECTORY", Path(directory)), \
                        patch("tools.hardware.mast_checks.DIRECTORY", Path(directory)), \
                        patch.object(operator_radio, "admin_status"), \
                        patch.object(operator_radio, "require_phy"), \
                        patch.object(operator_radio, "host_status",
                                     return_value={"companion": {"public_key": "sender"}}), \
                        patch.object(operator_radio, "Companion", return_value=client), \
                        patch.object(operator_radio, "signed_advert",
                                     return_value={"public_key": operator_radio.dm_target().hex()}), \
                        patch.object(operator_radio, "wait_push", side_effect=pushes), \
                        patch.object(operator_radio.time, "sleep"):
                    if route_found:
                        operator_radio.dm()
                    else:
                        with self.assertRaisesRegex(ValueError, "no DM sent"):
                            operator_radio.dm()
                    evidence = json.loads((Path(directory) / "operator-dm.json").read_text())
                    self.assertEqual(evidence["dm_requested"], route_found)
                    self.assertEqual(evidence.get("acknowledged", False), acknowledged)
                    sent = [call.args[0] for call in client.command.call_args_list]
                    self.assertEqual(sum(frame[:1] == b"\x02" for frame in sent), int(route_found))
                    client.command.side_effect = [bytes(contact), b"\x0bexport"]
                    with self.assertRaisesRegex(ValueError, "Refusing to replace|already requested"):
                        operator_radio.dm()
                    self.assertEqual(client.command.call_count, len(sent) + (0 if route_found else 2))

    def test_isolated_burst_never_touches_real_host_or_replays(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            previous = {"esp-command-bot": {"observed": {"timestamp": 1000}}}
            (Path(directory) / "operator-advert-state.json").write_text(json.dumps(previous))
            role = {"role": "companion", "public_key": "esp", "state": "running", "ready": True}
            companion = Mock(public_key="esp")
            companion.__enter__ = Mock(return_value=companion)
            companion.__exit__ = Mock(return_value=False)
            companion.command.return_value = b"\0"
            observer = Mock()
            observer.__enter__ = Mock(return_value=observer)
            observer.__exit__ = Mock(return_value=False)
            with patch.object(operator_radio, "DIRECTORY", Path(directory)), \
                    patch("tools.hardware.mast_checks.DIRECTORY", Path(directory)), \
                    patch.object(operator_radio, "preflight"), \
                    patch.object(operator_radio, "host_status") as host, \
                    patch.object(operator_radio, "dashboard", return_value={"roles": [role]}), \
                    patch.object(operator_radio, "Companion", return_value=companion), \
                    patch.object(operator_radio.socket, "create_connection", return_value=observer), \
                    patch.object(operator_radio, "wait_advert", return_value={"public_key": "esp"}), \
                    patch.object(operator_radio.time, "time", return_value=1100):
                operator_radio.isolated_adverts()
                with self.assertRaisesRegex(ValueError, "Refusing to replace"):
                    operator_radio.isolated_adverts()
                companion.command.assert_called_once_with(b"\x07\x01")
                host.assert_not_called()
                evidence = json.loads((Path(directory) / "operator-isolated-adverts.json").read_text())
                self.assertTrue(evidence["native_accepted"])
                self.assertTrue(any("15-minute" in reason for reason in evidence["unsupported"]))

    def test_isolated_missing_contact_does_not_use_real_host(self):
        role = {"role": "companion", "public_key": "esp", "state": "running", "ready": True}
        client = Mock(public_key="esp")
        client.__enter__ = Mock(return_value=client)
        client.__exit__ = Mock(return_value=False)
        client.command.return_value = b"\x01\x02"
        with patch.object(operator_radio, "admin_status"), \
                patch.object(operator_radio, "require_phy"), \
                patch.object(operator_radio, "guard_operator_dm"), \
                patch.object(operator_radio, "dashboard", return_value={"roles": [role]}), \
                patch.object(operator_radio, "Companion", return_value=client) as connect, \
                patch.object(operator_radio, "host_status") as host:
            with self.assertRaisesRegex(ValueError, "not learned; no DM sent"):
                operator_radio.dm(isolated=True)
            connect.assert_called_once_with(operator_radio.mast_host(), 5000)
            client.command.assert_called_once_with(b"\x1e" + operator_radio.dm_target(), allow_error=True)
            host.assert_not_called()

    def test_other_lane_dm_prevents_all_further_attempts(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            with patch.object(operator_radio, "DIRECTORY", Path(directory)), \
                    patch.object(operator_radio, "Companion") as companion, \
                    patch.object(operator_radio, "host_status") as host, \
                    patch.object(operator_radio, "admin_status") as mast:
                for state in ({"dm_requested": True}, {"dm_requested": None}, {}):
                    (Path(directory) / "operator-dm-external.json").write_text(json.dumps(state))
                    for isolated in (False, True):
                        with self.assertRaisesRegex(ValueError, "no duplicate DM"):
                            operator_radio.dm(isolated=isolated, retry_discovery=True)
                companion.assert_not_called()
                host.assert_not_called()
                mast.assert_not_called()

    def test_signed_contact_copy_cannot_request_host_rf(self):
        for valid in (False, True):
            with self.subTest(valid=valid):
                source, destination = Mock(), Mock()
                for client in (source, destination):
                    client.__enter__ = Mock(return_value=client)
                    client.__exit__ = Mock(return_value=False)
                source.command.return_value = b"\x0braw-advert"
                destination.command.return_value = b"\0"
                verified = {"public_key": operator_radio.dm_target().hex(), "timestamp": 1000, "digest": "digest"}
                with patch.object(operator_radio, "admin_status"), \
                        patch.object(operator_radio, "require_phy"), \
                        patch.object(operator_radio, "Companion", side_effect=[destination, source]), \
                        patch.object(operator_radio, "operator_contact", side_effect=[None, b"contact"]), \
                        patch.object(operator_radio, "save") as save, \
                        patch.object(operator_radio, "signed_advert", return_value=verified if valid else None):
                    if valid:
                        operator_radio.learn_contact()
                        destination.command.assert_called_once_with(b"\x12raw-advert")
                        save.assert_called_once()
                    else:
                        with self.assertRaisesRegex(ValueError, "no import or RF"):
                            operator_radio.learn_contact()
                        destination.command.assert_not_called()
                        save.assert_not_called()
                    source.command.assert_called_once_with(b"\x11" + operator_radio.dm_target(), allow_error=True)

    def test_explicit_discovery_retry_requires_definite_no_dm_and_keeps_history(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            directory = Path(directory)
            previous = {"sender": "sender", "target": operator_radio.dm_target().hex(),
                        "dm_requested": False, "outcome": "No authenticated discovery response; no DM sent"}
            next_attempt = dict(previous, outcome="Unknown")
            name = "operator-dm.json"
            with patch.object(operator_radio, "DIRECTORY", directory), \
                    patch("tools.hardware.mast_checks.DIRECTORY", directory):
                for field, value in (("dm_requested", True), ("dm_requested", None),
                                     ("sender", "another"), ("target", "another"),
                                     ("outcome", "Unknown")):
                    (directory / name).write_text(json.dumps(dict(previous, **{field: value})))
                    with self.assertRaisesRegex(ValueError, "Cannot retry"):
                        operator_radio.admit_dm_attempt(name, next_attempt, True)
                    self.assertEqual(list(directory.glob("*-history-*")), [])
                (directory / name).write_text(json.dumps(previous))
                operator_radio.admit_dm_attempt(name, next_attempt, True)
                history = list(directory.glob("*-history-*"))
                self.assertEqual(len(history), 1)
                self.assertEqual(json.loads(history[0].read_text()), previous)
                self.assertEqual(json.loads((directory / name).read_text()), next_attempt)
                with self.assertRaisesRegex(ValueError, "Cannot retry"):
                    operator_radio.admit_dm_attempt(name, next_attempt, True)


if __name__ == "__main__":
    unittest.main()
