# SPDX-License-Identifier: Apache-2.0
import hashlib
from collections import deque
import io
import socket
import struct
import tempfile
import threading
import unittest
from unittest.mock import patch
from unittest.mock import MagicMock

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.hardware import admin as mast_cli
from tools.hardware import management_rf_checks as beta_radio_test
from tools.hardware import companion_cli as stock_cli_test
from tools.hardware import wifi_checks as wifi_lab
from tools.hardware import bot_checks as runtime_lab

from tools.hardware.inventory import value as inventory_value
from test_support.operator_inventory import configure_inventory


class MastClientTests(unittest.TestCase):
    def setUp(self):
        configure_inventory(self)

    def test_named_source_export_is_read_only_and_preserves_exact_bytes(self):
        from tools.hardware import lua_sources
        source = lua_sources.encode({"main": None, "config": b"settings={interval=300}\n",
                                     "monitor": b"function poll() return settings.interval end\n"})
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            destination = Path(directory) / "monitor.lua"
            client = MagicMock()
            client.command.return_value = "Sources format=meshcore-sources/1 commit-base=sha256 atomic=1"
            arguments = ["mast_cli.py", "--unix-socket", str(Path(directory) / "owner.sock"),
                         "source-export", "monitor", str(destination)]
            with patch.object(sys, "argv", arguments), patch.object(mast_cli, "UnixClient", return_value=client), \
                    patch.object(mast_cli, "download", return_value=source), \
                    patch.object(mast_cli, "install") as install, patch("sys.stdout", new_callable=io.StringIO):
                mast_cli.main()
            self.assertEqual(destination.read_bytes(), lua_sources.decode(source)["monitor"])
            self.assertEqual(destination.stat().st_mode & 0o777, 0o600)
            client.command.assert_called_once_with("source api sources")
            client.close.assert_called_once()
            install.assert_not_called()
            for name, message in (("main", "bundled commands"), ("absent", "not installed"),
                                  ("../file", "identifier")):
                with patch.object(sys, "argv", arguments[:-2] + [name, str(destination)]), \
                        patch.object(mast_cli, "UnixClient", return_value=MagicMock(command=MagicMock(
                            return_value="Sources format=meshcore-sources/1 commit-base=sha256 atomic=1"))), \
                        patch.object(mast_cli, "download", return_value=source), \
                        patch.object(mast_cli, "install") as install, \
                        patch("sys.stderr", new_callable=io.StringIO) as error:
                    with self.assertRaises(SystemExit) as exited:
                        mast_cli.main()
                    self.assertEqual(exited.exception.code, 2)
                    self.assertIn(message, error.getvalue())
                    install.assert_not_called()
                self.assertEqual(destination.read_bytes(), lua_sources.decode(source)["monitor"])

    def test_native_single_send_leaves_timeout_unknown_without_replay(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            client = mast_cli.NativeClient.__new__(mast_cli.NativeClient)
            client.timestamp = 1
            client.clock_file = Path(directory) / "clock"
            client.path = None
            client.public = bytes([1]) * 32
            client.target = bytes([2]) * 32
            client.secret = bytes([3]) * 32
            client.tagged = False
            client.received_cli = deque(maxlen=32)
            client.tag = 1
            client.timeout = 1
            client.retry_commands = False
            client.connection = MagicMock()
            client.connection.recv.side_effect = socket.timeout
            with self.assertRaisesRegex(TimeoutError, "outcome unknown"):
                client.command("source rollback")
            self.assertEqual(client.connection.sendall.call_count, 1)

    def test_owner_unix_socket_framing_permissions_and_input(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            path = Path(directory) / "admin.sock"
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
                server.bind(str(path))
                server.listen(1)
                path.chmod(0o600)
                client = mast_cli.UnixClient(path, 2)
                for text in ("", "a" * 161, "name one\nreboot", "non-ascii \u00e9"):
                    with self.assertRaises(ValueError):
                        client.command(text)
                for response in (b"Name: Birch-Lua\n", b"x" * 257 + b"\n", b"truncated"):
                    received = []
                    def answer():
                        with server.accept()[0] as connection:
                            received.append(connection.recv(161))
                            connection.sendall(response)
                    thread = threading.Thread(target=answer)
                    thread.start()
                    try:
                        if response.startswith(b"Name:"):
                            self.assertEqual(client.command("name"), "Name: Birch-Lua")
                        else:
                            with self.assertRaisesRegex(ValueError, "Incomplete or oversized"):
                                client.command("name")
                    finally:
                        thread.join(3)
                    self.assertFalse(thread.is_alive())
                    self.assertEqual(received, [b"name\n"])
                path.chmod(0o666)
                with self.assertRaisesRegex(ValueError, "private socket"):
                    mast_cli.UnixClient(path)

    @staticmethod
    def identity_fixture():
        seed = bytes(range(32))
        native = stock_cli_test.native_private_key(seed)
        public = mast_cli.ed25519.Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(
            mast_cli.serialization.Encoding.Raw, mast_cli.serialization.PublicFormat.Raw).hex()
        return native, public

    def test_identity_import_checks_native_format_and_expected_public_before_sending(self):
        native, public = self.identity_fixture()
        client = mast_cli.NativeClient.__new__(mast_cli.NativeClient)
        with patch.object(client, "command") as command:
            for invalid in (b"", native[:32], native[:63], native + b"\0",
                            native.hex().encode(), bytes(64),
                            bytes((native[0] | 1,)) + native[1:]):
                with self.assertRaisesRegex(ValueError, "raw 64-byte"):
                    client.import_identity("bot", invalid, public)
            for expected in ("ab", "zz" * 32, "ab" * 32):
                with self.assertRaises(ValueError):
                    client.import_identity("bot", native, expected)
            for role in ("kiss", "observer", "modem", "bot\nreboot"):
                with self.assertRaisesRegex(ValueError, "role"):
                    client.import_identity(role, native, public)
            command.assert_not_called()

    def test_identity_import_roles_verify_pending_or_active_without_reboot(self):
        native, public = self.identity_fixture()
        pending = f"Pending {public}; reboot required; peers must learn new key"
        active = f"KEY {public}; already active; no reboot required"
        client = mast_cli.NativeClient.__new__(mast_cli.NativeClient)
        for role in mast_cli.IDENTITY_ROLES:
            wire_role = "bot" if role == "command-bot" else role
            for response, readback in ((pending, pending), (active, f"KEY {public}")):
                with patch.object(client, "command", side_effect=[response, readback]) as command:
                    self.assertEqual(client.import_identity(role, native, public.upper()), response)
                    self.assertEqual(command.call_args_list[0].args, (f"key {wire_role} {native.hex()}",))
                    self.assertLessEqual(len(command.call_args_list[0].args[0]) + 17, 160)
                    self.assertEqual(command.call_args_list[1].args,
                                     (f"key {wire_role}" + (" pending" if response == pending else ""),))

    def test_identity_import_sanitizes_failures_and_reports_unknown_readback(self):
        native, public = self.identity_fixture()
        pending = f"Pending {public}; reboot required; peers must learn new key"
        client = mast_cli.NativeClient.__new__(mast_cli.NativeClient)
        for response in ("Error: echo " + native.hex(), pending.replace(public, "ab" * 32),
                         "Pending", "OK", TimeoutError(native.hex()), ValueError(native.hex())):
            with patch.object(client, "command", side_effect=[response]) as command:
                with self.assertRaisesRegex(ValueError, "outcome unknown") as error:
                    client.import_identity("management", native, public)
                self.assertNotIn(native.hex(), str(error.exception))
                self.assertEqual(command.call_count, 1)
        for response in ("Error: identity commit/readback unknown; inspect pending key before reboot",
                         "Error: public identity already active or pending for another role; no change",
                         "Error: different pending identity or role reset; inspect pending key before cancelling"):
            with patch.object(client, "command", return_value=response):
                with self.assertRaises(ValueError) as error:
                    client.import_identity("bot", native, public)
                self.assertEqual(str(error.exception), response)
        for readback in (pending.replace(public, "ab" * 32), "Error: " + native.hex(),
                         TimeoutError(native.hex())):
            with patch.object(client, "command", side_effect=[pending, readback]) as command:
                with self.assertRaisesRegex(ValueError, "outcome unknown") as error:
                    client.import_identity("bot", native, public)
                self.assertNotIn(native.hex(), str(error.exception))
                self.assertEqual(command.call_count, 2)

    def test_file_identity_import_refuses_web_and_private_command_arguments(self):
        native, public = self.identity_fixture()
        cases = [
            ["--web", "http://mast.test", "key-import", "bot", "--native-key-file", "/not/read",
             "--expected-public-key", public],
            ["command", "key management " + native.hex()],
            ["command", "role key bot " + native.hex()],
        ]
        for arguments in cases:
            with patch.object(sys, "argv", ["mast_cli.py"] + arguments), \
                    patch.object(mast_cli, "private_file") as private, \
                    patch.object(mast_cli, "NativeClient") as rf, \
                    patch.object(mast_cli, "WebClient") as web, \
                    patch.object(mast_cli, "UnixClient") as unix, \
                    patch("sys.stderr", new_callable=io.StringIO) as error:
                with self.assertRaises(SystemExit) as exit:
                    mast_cli.main()
                self.assertEqual(exit.exception.code, 2)
                self.assertNotIn(native.hex(), error.getvalue())
                private.assert_not_called(); rf.assert_not_called(); web.assert_not_called(); unix.assert_not_called()

    def test_role_password_file_transport_validation_and_secret_sanitization(self):
        password = b"recovery-pass15"
        client = mast_cli.NativeClient.__new__(mast_cli.NativeClient)
        expected = "Saved and applied repeater administrator password; ACL/sessions unchanged"
        with patch.object(client, "command", return_value=expected) as command:
            self.assertEqual(client.set_role_password("repeater", password), expected)
            command.assert_called_once_with("role password repeater " + password.hex())
        for result in (password.decode(), password.hex(), "OK",
                       TimeoutError(password.hex()), ValueError(password.decode())):
            with patch.object(client, "command", side_effect=[result]) as command:
                with self.assertRaisesRegex(ValueError, "outcome unknown") as error:
                    client.set_role_password("room", password)
                self.assertNotIn(password.hex(), str(error.exception))
                self.assertNotIn(password.decode(), str(error.exception))
                self.assertEqual(command.call_count, 1)
        unknown = "Error: role password persistence unknown; live unchanged; saved may differ; verify before retry"
        with patch.object(client, "command", return_value=unknown):
            with self.assertRaisesRegex(ValueError, "live unchanged"):
                client.set_role_password("room", password)
        for value in (b"", b"a" * 16, b"line\n", b"\x00", b"\x1f", b"\x7f", b"\xff"):
            with patch.object(client, "command") as command:
                with self.assertRaises(ValueError):
                    client.set_role_password("room", value)
                command.assert_not_called()
        for arguments in (
            ["--web", "http://mast.test", "role-password", "room", "--new-password-file", "/not/read"],
            ["--unix-socket", "/not/read", "role-password", "room", "--new-password-file", "/not/read"],
            ["command", "role password repeater " + password.hex()],
            ["command", "role  password room " + password.hex()],
        ):
            with patch.object(sys, "argv", ["mast_cli.py"] + arguments), \
                    patch.object(mast_cli, "private_file") as private, \
                    patch.object(mast_cli, "NativeClient") as rf, \
                    patch.object(mast_cli, "WebClient") as web, \
                    patch.object(mast_cli, "UnixClient") as unix, \
                    patch("sys.stderr", new_callable=io.StringIO) as error:
                with self.assertRaises(SystemExit):
                    mast_cli.main()
                self.assertNotIn(password.hex(), error.getvalue())
                private.assert_not_called(); rf.assert_not_called(); web.assert_not_called(); unix.assert_not_called()

    def test_role_password_private_file_before_connect_and_separate_management_auth(self):
        password = b"new-role-pass15"
        expected = "Saved and applied room administrator password; ACL/sessions unchanged"
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            path = Path(directory) / "role-password"
            auth = Path(directory) / "management-password"
            mast_cli.write_new_file(path, password, 0o600)
            mast_cli.write_new_file(auth, b"management-auth", 0o600)
            arguments = ["mast_cli.py", "--gateway", "unused", "--target", "42" * 32,
                         "--seed-file", "/not/read", "--password-file", str(auth),
                         "role-password", "room", "--new-password-file", str(path)]
            client = mast_cli.NativeClient.__new__(mast_cli.NativeClient)
            client.connection = MagicMock()
            with patch.object(sys, "argv", arguments), \
                    patch.object(mast_cli, "NativeClient", return_value=client) as connect, \
                    patch.object(client, "command", return_value=expected), \
                    patch("sys.stdout", new_callable=io.StringIO) as output:
                mast_cli.main()
                self.assertEqual(output.getvalue(), expected + "\n")
                self.assertEqual(connect.call_args.args[4], "management-auth")
                self.assertNotIn(password.decode(), repr(connect.call_args))
                self.assertNotIn(password.hex(), repr(arguments))
                client.connection.close.assert_called_once()
            for value, mode in ((password, 0o644), (b"", 0o600), (b"x" * 16, 0o600),
                                (b"trailing\n", 0o600)):
                path.write_bytes(value); path.chmod(mode)
                with patch.object(sys, "argv", arguments), \
                        patch.object(mast_cli, "NativeClient") as connect, \
                        patch("sys.stderr", new_callable=io.StringIO):
                    with self.assertRaises(SystemExit):
                        mast_cli.main()
                    connect.assert_not_called()
            path.unlink(); path.symlink_to(auth)
            with patch.object(sys, "argv", arguments), \
                    patch.object(mast_cli, "NativeClient") as connect, \
                    patch("sys.stderr", new_callable=io.StringIO):
                with self.assertRaises(SystemExit):
                    mast_cli.main()
                connect.assert_not_called()

    def test_unix_identity_import_is_bot_only_and_sanitized(self):
        native, public = self.identity_fixture()
        client = mast_cli.UnixClient.__new__(mast_cli.UnixClient)
        pending = f"KEY {public} pending; apply required"
        with patch.object(client, "command", side_effect=[pending, pending]) as command:
            self.assertEqual(client.import_identity("bot", native, public), pending)
            self.assertEqual([call.args[0] for call in command.call_args_list],
                             ["key bot " + native.hex(), "key bot pending"])
        active = f"KEY {public}; already active; no apply required"
        with patch.object(client, "command", side_effect=[active, f"KEY {public}"]) as command:
            self.assertEqual(client.import_identity("bot", native, public), active)
            self.assertEqual(command.call_args_list[1].args, ("key bot",))
        with patch.object(client, "command") as command:
            for role in ("room", "management", "companion"):
                with self.assertRaisesRegex(ValueError, "only the native bot"):
                    client.import_identity(role, native, public)
            with self.assertRaises(ValueError):
                client.import_identity("bot", native[:32], public)
            command.assert_not_called()
        for result in ("Error: " + native.hex(), TimeoutError(native.hex())):
            with patch.object(client, "command", side_effect=[result]):
                with self.assertRaises(ValueError) as error:
                    client.import_identity("bot", native, public)
                self.assertNotIn(native.hex(), str(error.exception))

    def test_file_identity_import_validates_before_connecting_and_prints_only_public_result(self):
        native, public = self.identity_fixture()
        pending = f"Pending {public}; reboot required; peers must learn new key"
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            path = Path(directory) / "fixture.key"
            mast_cli.write_new_file(path, native, 0o600)
            arguments = ["mast_cli.py", "--gateway", "unused", "--target", "42" * 32,
                         "--seed-file", "/not/read", "key-import", "management",
                         "--native-key-file", str(path), "--expected-public-key", public]
            client = mast_cli.NativeClient.__new__(mast_cli.NativeClient)
            client.connection = MagicMock()
            with patch.object(sys, "argv", arguments), \
                    patch.object(mast_cli, "NativeClient", return_value=client) as connect, \
                    patch.object(client, "command", side_effect=[pending, pending]), \
                    patch("sys.stdout", new_callable=io.StringIO) as output:
                mast_cli.main()
                self.assertEqual(output.getvalue(), pending + "\n")
                self.assertNotIn(native.hex(), repr(connect.call_args))
                self.assertNotIn(native.hex(), repr(arguments))
                client.connection.close.assert_called_once()
            for mode, expected in ((0o644, public), (0o600, "ab" * 32)):
                path.chmod(mode)
                with patch.object(sys, "argv", arguments[:-1] + [expected]), \
                        patch.object(mast_cli, "NativeClient") as connect, \
                        patch("sys.stderr", new_callable=io.StringIO) as error:
                    with self.assertRaises(SystemExit):
                        mast_cli.main()
                    self.assertNotIn(native.hex(), error.getvalue())
                    connect.assert_not_called()
            link = Path(directory) / "link.key"
            link.symlink_to(path)
            with self.assertRaises(OSError):
                mast_cli.private_file(link, 64)

    def test_identity_import_uses_encrypted_tagged_native_request_at_sender_limit(self):
        native, public = self.identity_fixture()
        pending = f"Pending {public}; reboot required; peers must learn new key"
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            client = mast_cli.NativeClient.__new__(mast_cli.NativeClient)
            client.clock_file = Path(directory) / "clock"
            client.timestamp, client.tag, client.timeout = 0, 0x0123456789abcdef, 9
            client.tagged, client.path, client.fixed_path = True, None, None
            client.secret = bytes(range(32))
            client.public, client.target = bytes([21]) * 32, bytes([42]) * 32
            client.connection = MagicMock()
            replies = []
            for index in (0, 1):
                plain = (struct.pack("<I", 1800000010 + index) + b"\x04" +
                         f"{client.tag + index:016x}|{pending}".encode())
                replies.append(mast_cli.kiss(b"\x0a\x00" + bytes((21, 42)) +
                                            mast_cli.encrypt(client.secret, plain)))
            client.connection.recv.side_effect = replies
            with patch.object(mast_cli.time, "time", return_value=1800000000):
                self.assertEqual(client.import_identity("management", native, public), pending)
            wire = client.connection.sendall.call_args_list[0].args[0]
            self.assertNotIn(native.hex().encode(), wire)
            packet = wire[2:-1].replace(b"\xdb\xdc", b"\xc0").replace(b"\xdb\xdd", b"\xdb")
            self.assertEqual(packet[:2], b"\x09\x80")
            self.assertEqual(len(packet) - 2, 180)
            kind, plain = mast_cli.decode(client.secret, client.target, client.public, packet)
            self.assertEqual(kind, 2)
            text = plain[5:].rstrip(b"\0")
            self.assertEqual(text, ("0123456789abcdef|key management " + native.hex()).encode())
            self.assertEqual(len(text), 160)

    def test_failed_stock_login_is_retained_as_failure_only_when_requested(self):
        value = '{"login_success":false,"error":"login failed"}'
        with self.assertRaisesRegex(ValueError, "Stock CLI reported"):
            stock_cli_test.documents(value)
        rows = stock_cli_test.documents(value, allow_login_failure=True)
        self.assertIs(rows[0]["login_success"], False)
        with self.assertRaisesRegex(ValueError, "Stock CLI reported"):
            stock_cli_test.documents('{"error":"other failure"}', allow_login_failure=True)

    def test_private_stock_cli_uses_memory_only_script_and_withholds_error_output(self):
        import os
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            port = MagicMock()
            port.is_symlink.return_value = True
            port.resolve.return_value.name = "ttyACM2"
            captured = []

            def run(args, **kwargs):
                descriptor, = kwargs["pass_fds"]
                self.assertEqual(args[-1], f"/proc/self/fd/{descriptor}")
                self.assertEqual(os.pread(descriptor, 4096, 0), b"generated-private-fixture\n")
                captured.append(descriptor)
                return MagicMock(returncode=0, stdout='{"error":"generated-private-fixture"}',
                                 stderr="generated-private-fixture")

            with patch.object(stock_cli_test, "DIRECTORY", Path(directory)), \
                    patch.object(stock_cli_test, "device_port", return_value=port), \
                    patch.object(stock_cli_test.shutil, "which", return_value="/unused/meshcli"), \
                    patch.object(stock_cli_test.subprocess, "run", side_effect=run):
                with self.assertRaisesRegex(ValueError, "output withheld") as error:
                    stock_cli_test.run_cli("private", ["generated-private-fixture"], private_script=True)
                self.assertNotIn("generated-private-fixture", str(error.exception))
            self.assertFalse(any(path.is_file() for path in Path(directory).rglob("*")))
            with self.assertRaises(OSError):
                os.fstat(captured[0])

    def test_stock_status_reports_native_mode_and_bytes_without_provisioning(self):
        rows = [{"path_hash_mode": 2},
                {"name": "Cedar-Base", "public_key": "12" * 32, "radio_freq": 912.525,
                 "radio_bw": 250, "radio_sf": 7, "radio_cr": 5, "tx_power": 2}]
        with patch.object(stock_cli_test, "run_cli", return_value=rows) as cli, \
                patch.object(stock_cli_test, "configure") as configure, \
                patch.object(stock_cli_test, "verified_backup") as backup, \
                patch.object(sys, "argv", ["stock_cli_test.py", "--status"]), \
                patch("builtins.print") as output:
            stock_cli_test.main()
            self.assertIn('"path_hash_bytes": 3', output.call_args.args[0])
            self.assertEqual(cli.call_args.args[1], ["ver", "infos"])
            configure.assert_not_called()
            backup.assert_not_called()
            rows[0]["path_hash_mode"] = 3
            with self.assertRaisesRegex(ValueError, "Invalid native"):
                stock_cli_test.read_only_status()
    def test_explicit_stock_route_uses_three_bytes_and_native_mode(self):
        relay, target = "ab0102" + "00" * 29, "ab0103" + "00" * 29
        self.assertEqual(stock_cli_test.three_byte_relay_path(relay, target), "ab0102:2")
        with self.assertRaisesRegex(ValueError, "Ambiguous three-byte"):
            stock_cli_test.three_byte_relay_path(relay, relay.upper())
        with self.assertRaisesRegex(ValueError, "full native"):
            stock_cli_test.three_byte_relay_path("ab", target)

    def test_public_probe_originates_three_byte_advert_message_ack_and_reply(self):
        client = MagicMock()
        client.key = mast_cli.ed25519.Ed25519PrivateKey.from_private_bytes(bytes(range(32)))
        client.public = client.key.public_key().public_bytes(
            mast_cli.serialization.Encoding.Raw, mast_cli.serialization.PublicFormat.Raw)
        client.timestamp = 1
        target_key = mast_cli.ed25519.Ed25519PrivateKey.from_private_bytes(bytes(reversed(range(32))))
        target = target_key.public_key().public_bytes(
            mast_cli.serialization.Encoding.Raw, mast_cli.serialization.PublicFormat.Raw)
        _, secret, advert, request, _ = beta_radio_test.public_request(client, target.hex(), "!ping")
        self.assertEqual(advert[:2], b"\x11\x80")
        self.assertEqual(request[:2], b"\x09\x80")

        def receive(connection, context, timeout, verify, label):
            for text in (b"question", b"answer:field"):
                packet = (b"\x0a\x00" + bytes((client.public[0], target[0])) +
                          mast_cli.encrypt(secret, struct.pack("<I", 1800000010) + b"\0" + text))
                result = verify(packet, None)
            return result

        with patch.object(beta_radio_test.socket, "create_connection") as connection, \
                patch.object(beta_radio_test.time, "sleep"), \
                patch.object(beta_radio_test, "wait_receipt", side_effect=receive):
            self.assertEqual(beta_radio_test.public_exchange(client, target.hex()), "answer:field")
            sent = connection.return_value.__enter__.return_value.sendall.call_args.args[0]
            frames = [frame for frame in sent.split(b"\xc0") if frame]
            self.assertEqual([frame[1:3] for frame in frames], [b"\x0e\x80", b"\x09\x80"])

    def test_stock_cli_empty_success_is_reported_as_failure(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            port = MagicMock()
            port.is_symlink.return_value = True
            port.resolve.return_value.name = "ttyACM2"
            completed = MagicMock(returncode=0, stdout="", stderr="ERROR:meshcore:No response")
            with patch.object(stock_cli_test, "DIRECTORY", Path(directory)), \
                    patch.object(stock_cli_test, "device_port", return_value=port), \
                    patch.object(stock_cli_test.shutil, "which", return_value="/unused/meshcli"), \
                    patch.object(stock_cli_test.subprocess, "run", return_value=completed):
                with self.assertRaisesRegex(ValueError, "returned no response"):
                    stock_cli_test.run_cli("missing-response", ["ver"])

    def test_download_requires_exact_source_and_stable_generation(self):
        source = b"function hello() return 'useful existing source' end\n"
        manifest = "SHA256 " + hashlib.sha256(source).hexdigest() + " gen=3"
        replies = [manifest, "DATA " + source[:48].hex(), "DATA " + source[48:].hex(),
                   "EOF", manifest]
        with patch.object(mast_cli, "checked", side_effect=replies):
            self.assertEqual(mast_cli.download(None), source)
        for last in (manifest.replace("gen=3", "gen=4"), "SHA256 " + "0" * 64 + " gen=3"):
            with patch.object(mast_cli, "checked", side_effect=replies[:-1] + [last]):
                with self.assertRaisesRegex(ValueError, "changed or failed"):
                    mast_cli.download(None)
        with patch.object(mast_cli, "checked", side_effect=[manifest, "DATA zz"]):
            with self.assertRaisesRegex(ValueError, "source read"):
                mast_cli.download(None)

    def test_scheduler_data_requires_explicit_non_rearming_policy(self):
        for kind in ("timers", "reminders"):
            data = bytearray(mast_cli.DATA_LIMIT)
            data[:4] = mast_cli.DATA_MAGIC[kind]
            data[37] = 7
            data[-32:] = hashlib.sha256(data[:-32]).digest()
            digest = hashlib.sha256(data).hexdigest()
            ident = digest[:16]
            with patch.object(mast_cli, "checked") as checked:
                with self.assertRaisesRegex(ValueError, "--no-rearm"):
                    mast_cli.data_restore(None, bytes(data))
                checked.assert_not_called()
            status = f"EXPORTED {digest} {ident}"
            commands = []

            def checked(client, text):
                nonlocal status
                commands.append(text)
                if text == "data status":
                    return status
                if text.startswith("data read "):
                    offset = int(text.split()[-1]) * 48
                    return "DATA " + data[offset:offset + 48].hex()
                if text.startswith("data begin "):
                    return f"UPLOADING {ident} bytes={mast_cli.DATA_LIMIT}"
                if text.startswith("data chunk "):
                    return f"RECEIVED {min((int(text.split()[3]) + 1) * 48, mast_cli.DATA_LIMIT)}"
                if text.startswith("data stage "):
                    status = f"STAGED {ident}"
                if text.startswith("data restore "):
                    status = f"COMMITTED  {ident}"
                return f"PENDING {ident}"

            with patch.object(mast_cli, "checked", side_effect=checked):
                self.assertEqual(mast_cli.data_export(None, "caller", "07" + "00" * 31, kind), data)
                self.assertTrue(mast_cli.data_restore(None, bytes(data), no_rearm=True).startswith("COMMITTED"))
                self.assertIn(f"data restore {ident} no-rearm", commands)
            with patch.object(mast_cli, "checked", side_effect=checked):
                status = f"EXPORTED {digest} {ident}"
                with self.assertRaisesRegex(ValueError, "version"):
                    mast_cli.data_export(None, "caller", "07" + "00" * 31, "kv")
        with self.assertRaisesRegex(ValueError, "scope"):
            mast_cli.data_export(None, "channel", "07" + "00" * 31, "reminders")

    def test_scoped_data_export_and_staged_restore(self):
        data = bytearray(mast_cli.DATA_LIMIT)
        data[:4] = b"BKD\x01"
        data[36] = 2
        data[-32:] = hashlib.sha256(data[:-32]).digest()
        digest = hashlib.sha256(data).hexdigest()
        ident = digest[:16]
        status = f"EXPORTED {digest} {ident}"
        commands = []

        def checked(client, text):
            nonlocal status
            commands.append(text)
            if text == "data status":
                return status
            if text.startswith("data read "):
                offset = int(text.split()[-1]) * 48
                return "DATA " + data[offset:offset + 48].hex()
            if text.startswith("data begin "):
                return f"UPLOADING {ident} bytes={mast_cli.DATA_LIMIT}"
            if text.startswith("data chunk "):
                return f"RECEIVED {min((int(text.split()[3]) + 1) * 48, mast_cli.DATA_LIMIT)}"
            if text.startswith("data stage "):
                status = f"STAGED {ident}"
            if text.startswith("data restore "):
                status = f"COMMITTED  {ident}"
            return f"PENDING {ident}"

        with patch.object(mast_cli, "checked", side_effect=checked):
            self.assertEqual(mast_cli.data_export(None, "bot", "0" * 64), data)
            self.assertTrue(mast_cli.data_restore(None, bytes(data)).startswith("COMMITTED"))
            self.assertLess(commands.index(f"data stage {ident}"), commands.index(f"data restore {ident}"))
            with self.assertRaisesRegex(ValueError, "SHA256"):
                mast_cli.data_restore(None, bytes(data[:-1]) + bytes((data[-1] ^ 1,)))
        with patch.object(mast_cli, "checked", return_value="UNKNOWN commit readback"):
            with self.assertRaisesRegex(ValueError, "UNKNOWN"):
                mast_cli.data_wait(None, "COMMITTED")
        with self.assertRaisesRegex(ValueError, "zero principal"):
            mast_cli.data_export(None, "caller", "0" * 64)
        with patch.object(mast_cli, "checked", return_value="BUSY bot-data"):
            with self.assertRaisesRegex(ValueError, "not admitted"):
                mast_cli.data_export(None, "bot", "0" * 64)
            with self.assertRaisesRegex(ValueError, "not admitted"):
                mast_cli.data_restore(None, bytes(data))

    def test_retained_field_cleanup_restores_source_even_when_delete_receipt_fails(self):
        state = {"installed": False}
        original = "a" * 64

        def checked(client, command):
            if command == "source hash":
                return "SHA256 " + ("b" * 64 if state["installed"] else original)
            if command == "source api":
                return "API runtime=shared-v1 mesh=dm,wait"
            if command == "source rollback":
                state["installed"] = False
            if command == "bot key":
                return "KEY " + "1" * 64
            if command == "bot status":
                return "ready=1"
            if command == "wifi status":
                return "connected=1"
            return "OK"

        def install(*args):
            state["installed"] = True

        def command(peer, bot, argument, name, **kwargs):
            if name == "cleanup":
                raise ValueError("injected delete receipt loss")
            return kwargs["expected"]

        with patch.object(runtime_lab, "status", return_value={"kiss": {"connected": 0},
                                                              "scheduler": {"queued": 0}}), \
                patch.object(runtime_lab, "connect", return_value=MagicMock()), \
                patch.object(runtime_lab, "native", return_value=MagicMock()), \
                patch.object(runtime_lab, "checked", side_effect=checked), \
                patch.object(runtime_lab, "install", side_effect=install), \
                patch.object(runtime_lab, "active"), \
                patch.object(runtime_lab, "public_command", side_effect=command), \
                patch.object(runtime_lab, "public_exchange", return_value="answer:field"), \
                patch.object(runtime_lab.time, "sleep"):
            with self.assertRaisesRegex(ValueError, "delete receipt loss"):
                runtime_lab.exercise()
        self.assertFalse(state["installed"])

    def test_retained_field_refuses_serial_access_when_gateway_busy(self):
        with patch.object(runtime_lab, "status", return_value={"kiss": {"connected": 1},
                                                              "scheduler": {"queued": 0}}), \
                patch.object(runtime_lab, "start_capture") as capture:
            with self.assertRaisesRegex(ValueError, "in use"):
                runtime_lab.main()
            capture.assert_not_called()

    def test_lan_credentials_selects_only_literal_wifi_settings(self):
        text = """UNRELATED_KEY=do-not-use
WIFI_AP='fixture lan'
WIFI_PASSWORD="fixture-password" # synthetic unit-test value
"""
        with patch.object(Path, "open", return_value=io.StringIO(text)):
            self.assertEqual(wifi_lab.lan_credentials(), (b"fixture lan", b"fixture-password"))
        with patch.object(Path, "open", return_value=io.StringIO("WIFI_AP='unterminated\n")):
            with self.assertRaisesRegex(ValueError, "value withheld"):
                wifi_lab.lan_credentials()

    def test_native_retry_preserves_timestamp_text_and_changes_attempt(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            client = mast_cli.NativeClient.__new__(mast_cli.NativeClient)
            client.tagged = True
            client.clock_file = Path(directory) / "clock"
            client.timestamp, client.tag, client.timeout, client.path = 0, 8, 9, None
            client.fixed_path = None
            client.secret = bytes(range(32))
            client.public, client.target = bytes([21]) * 32, bytes([42]) * 32
            client.connection = MagicMock()
            plain = struct.pack("<I", 1800000010) + b"\x04" + b"0000000000000008|Saved fixture"
            response = b"\x0a\x00" + bytes((21, 42)) + mast_cli.encrypt(client.secret, plain)
            client.connection.recv.side_effect = [socket.timeout(), mast_cli.kiss(response)]
            with patch("tools.hardware.admin.time.time", return_value=1800000000):
                self.assertEqual(client.command("roles 0"), "Saved fixture")
            self.assertEqual(client.connection.sendall.call_count, 2)
            decoded = []
            for call in client.connection.sendall.call_args_list:
                wire = call.args[0][2:-1].replace(b"\xdb\xdc", b"\xc0").replace(b"\xdb\xdd", b"\xdb")
                kind, body = mast_cli.decode(client.secret, client.target, client.public, wire)
                self.assertEqual(kind, 2)
                decoded.append(body)
            self.assertEqual(decoded[0][4], 4)
            self.assertEqual(decoded[1][4], 5)
            self.assertEqual(decoded[0][:4] + decoded[0][5:], decoded[1][:4] + decoded[1][5:])
            client.tagged = False
            client.received_cli = deque(maxlen=32)
            response = (b"\x0a\x80" + bytes((21, 42)) +
                        mast_cli.encrypt(client.secret, struct.pack("<I", 1800000020) + b"\x04OK"))
            client.connection.recv.side_effect = [mast_cli.kiss(response)]
            with patch("tools.hardware.admin.time.time", return_value=1800000000), \
                    patch("tools.hardware.admin.encrypt", wraps=mast_cli.encrypt) as encrypt:
                self.assertEqual(client.command("set name Aspen-Relay"), "OK")
                self.assertTrue(encrypt.call_args.args[1].endswith(b"\x04set name Aspen-Relay"))

    def test_native_untagged_commands_ignore_previous_reply_on_another_route(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            client = mast_cli.NativeClient.__new__(mast_cli.NativeClient)
            client.tagged = False
            client.received_cli = deque(maxlen=32)
            client.clock_file = Path(directory) / "clock"
            client.timestamp, client.tag, client.timeout, client.path = 0, 8, 9, None
            client.fixed_path = None
            client.secret = bytes(range(32))
            client.public, client.target = bytes([21]) * 32, bytes([42]) * 32
            client.connection = MagicMock()

            def reply(timestamp, text, route=b"\0"):
                body = struct.pack("<I", timestamp) + b"\x04" + text.encode()
                return mast_cli.kiss(b"\x0a" + route + bytes((21, 42)) + mast_cli.encrypt(client.secret, body))

            client.connection.recv.side_effect = [
                reply(1800000010, "> Relay"),
                reply(1800000010, "> Relay", b"\x80"),
                reply(1800000011, "meshcore version"),
                reply(1800000012, "> Relay"),
            ]
            with patch("tools.hardware.admin.time.time", return_value=1800000000):
                self.assertEqual(client.command("get name"), "> Relay")
                self.assertEqual(client.command("ver"), "meshcore version")
                self.assertEqual(client.command("get name"), "> Relay")

    def test_explicit_route_ignores_reply_before_physical_last_hop(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            client = mast_cli.NativeClient.__new__(mast_cli.NativeClient)
            client.tagged = True
            client.clock_file = Path(directory) / "clock"
            client.timestamp, client.tag, client.timeout = 0, 8, 9
            client.path = client.fixed_path = b"\x81\x12\x34\x56"
            client.routed_responses = client.incomplete_routes = client.route_repairs = 0
            client.secret = bytes(range(32))
            client.public, client.target = bytes([21]) * 32, bytes([42]) * 32
            client.connection = MagicMock()
            body = struct.pack("<I", 1800000010) + b"\x04" + b"0000000000000008|Routed"
            payload = bytes((21, 42)) + mast_cli.encrypt(client.secret, body)
            pending = b"\x0a" + client.path + payload
            complete = b"\x0a\x80" + payload
            client.connection.recv.side_effect = [mast_cli.kiss(pending), mast_cli.kiss(complete)]
            with patch("tools.hardware.admin.time.time", return_value=1800000000):
                self.assertEqual(client.command("source status"), "Routed")
            self.assertEqual((client.incomplete_routes, client.routed_responses), (1, 1))
            client.tag = 8
            flood = b"\x09\x80" + payload
            client.connection.recv.side_effect = [
                mast_cli.kiss(flood), mast_cli.kiss(flood), mast_cli.kiss(complete)]
            with patch("tools.hardware.admin.time.time", return_value=1800000000), \
                    patch.object(client, "send_fixed_path") as repair:
                self.assertEqual(client.command("source status"), "Routed")
                repair.assert_called_once()
            self.assertEqual(client.route_repairs, 1)
        with patch("tools.hardware.admin.private_file") as private:
            for path in (b"", b"\x80", b"\x81\x12", b"\xc1\x12\x34\x56\x78"):
                with self.assertRaisesRegex(ValueError, "encoded path"):
                    mast_cli.NativeClient("unused", 8001, None, "unused", "", path=path)
            private.assert_not_called()

    def test_room_login_adds_native_sync_since_without_changing_mast_login(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            for room in (False, True):
                client = mast_cli.NativeClient.__new__(mast_cli.NativeClient)
                client.tagged = True
                client.clock_file = Path(directory) / "clock"
                client.timestamp, client.tag, client.timeout, client.path = 0, 8, 9, None
                client.fixed_path = None
                client.secret = bytes(range(32))
                client.public, client.target = bytes([21]) * 32, bytes([42]) * 32
                client.connection = MagicMock()
                plain = struct.pack("<I", 1800000010) + b"\0\0\x01\x03" + b"test\x01"
                response = b"\x06\x00" + bytes((21, 42)) + mast_cli.encrypt(client.secret, plain)
                client.connection.recv.return_value = mast_cli.kiss(response)
                with patch("tools.hardware.admin.time.time", return_value=1800000000), \
                        patch("tools.hardware.admin.encrypt", wraps=mast_cli.encrypt) as encrypt:
                    self.assertEqual(client.exchange("fixture", login=True, room=room),
                                     "Authenticated native administrator")
                timestamp = struct.pack("<I", 1800000000)
                self.assertEqual(encrypt.call_args.args[1], timestamp * (2 if room else 1) + b"fixture\0")
                client.connection.sendall.assert_called_once()

    def test_room_member_login_does_not_grant_mast_administration(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3] / ".tmp") as directory:
            for room, permissions, accepted in (
                    (True, b"\x00\x02", True), (True, b"\x02\x00", True),
                    (False, b"\x00\x02", False), (True, b"\x01\x02", False)):
                with self.subTest(room=room, permissions=permissions):
                    client = mast_cli.NativeClient.__new__(mast_cli.NativeClient)
                    client.tagged = True
                    client.clock_file = Path(directory) / "clock"
                    client.timestamp, client.tag, client.timeout, client.path = 0, 8, 9, None
                    client.fixed_path = None
                    client.public, client.target = bytes([21]) * 32, bytes([42]) * 32
                    client.secret = bytes(range(32))
                    client.connection = MagicMock()
                    plain = struct.pack("<I", 1800000010) + b"\0\0" + permissions + b"test\x01"
                    response = b"\x06\x00" + bytes((21, 42)) + mast_cli.encrypt(client.secret, plain)
                    client.connection.recv.side_effect = [mast_cli.kiss(response), b""]
                    with patch("tools.hardware.admin.time.time", return_value=1800000000):
                        if accepted:
                            self.assertEqual(client.exchange("fixture", login=True, room=room),
                                             "Authenticated native room member")
                        else:
                            with self.assertRaisesRegex(ValueError, "Gateway disconnected"):
                                client.exchange("fixture", login=True, room=room)

    def test_isolated_wifi_config_get_is_exact_and_fails_closed(self):
        import struct
        from unittest.mock import MagicMock
        frame = bytearray(b"\x06\xa2\x01\x00") + struct.pack("<III", 0xc0, 912525000, 250000)
        frame += bytes((8, 5, 2)) + bytes(7)
        wire = b"\xc0" + frame.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc") + b"\xc0"
        connection = MagicMock()
        connection.__enter__.return_value = connection
        connection.recv.side_effect = [wire[:11], wire[11:]]
        with patch("tools.hardware.wifi_checks.mast_ip", return_value="10.77.77.2"), \
                patch("tools.hardware.wifi_checks.socket.create_connection", return_value=connection) as connect:
            self.assertEqual(wifi_lab.config_get(),
                             {"generation": 0xc0, "frequency_hz": 912525000, "sf": 8, "power": 2})
            connect.assert_called_once_with(("10.77.77.2", 8001), timeout=3)
            for invalid in (b"", b"\xc0\xdb\x00", b"x" * 257):
                connection.recv.side_effect = [invalid]
                with self.assertRaises(ValueError):
                    wifi_lab.config_get()

    def test_stock_public_user_needs_no_owner_protocol(self):
        lines = stock_cli_test.public_commands()
        verbs = {line.split()[0] for line in lines}
        self.assertEqual(verbs, {"sync_msgs", "msgs_subscribe", "advert", "sleep", "msg", "ver"})
        self.assertFalse(any("beta-mast" in line or "|" in line for line in lines))
        bot = "42" * 32
        replies = [{"type": "PRIV", "pubkey_prefix": bot[:12], "SNR": 8, "txt_type": 0,
                    "text": text} for text in ("Pong", "Connected; RSSI=-90 dBm SNR=5.25 dB")]
        self.assertEqual(stock_cli_test.public_replies(replies, bot), replies)
        for invalid in (replies[:1], replies[1:], replies + [{"login_success": True}],
                        [dict(entry, pubkey_prefix="00" * 6) for entry in replies],
                        [dict(entry, txt_type=1) for entry in replies],
                        [replies[0], dict(replies[1], text="Connected; RSSI=999 dBm SNR=5.25 dB")],
                        [{k: v for k, v in entry.items() if k != "SNR"} for entry in replies]):
            with self.assertRaises(ValueError):
                stock_cli_test.public_replies(invalid, bot)

    def test_stock_cli_json_is_fail_closed(self):
        self.assertEqual(stock_cli_test.documents('{"login_success":true}\n{"text":"Pong"}'),
                         [{"login_success": True}, {"text": "Pong"}])
        self.assertEqual(stock_cli_test.documents('[]\n[{"text":"Pong"}]'), [{"text": "Pong"}])
        for invalid in ('{"error":"timeout"}', 'No native reply', '[null]'):
            with self.assertRaises(ValueError):
                stock_cli_test.documents(invalid)
        private = stock_cli_test.native_private_key(bytes(range(32)))
        self.assertEqual(len(private), 64)
        self.assertEqual(private[0] & 7, 0)
        self.assertEqual(private[31] & 0xc0, 0x40)

    def test_stock_channel_literal_is_explicit_bounded_and_data_only(self):
        value = {"channel_idx": 1, "channel_name": inventory_value("channel_tag"), "channel_secret": "ab" * 16}
        self.assertEqual(stock_cli_test.channel_document(repr(value)), value)
        for invalid in ("__import__('os')", repr(value) + "\n" + repr(value),
                        repr(dict(value, channel_secret="ab")),
                        repr(dict(value, channel_idx=True)), " " * 4097, "None"):
            with self.assertRaises(ValueError):
                stock_cli_test.channel_document(invalid)

    def test_physical_config_readback_decoding(self):
        from unittest.mock import MagicMock
        import struct
        frame = bytearray(b"\x06\xa2\x01\x00") + struct.pack("<III", 0xc0, 912525000, 250000)
        frame += bytes((8, 5, 2)) + bytes(7)
        wire = b"\xc0" + frame.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc") + b"\xc0"
        connection = MagicMock()
        connection.recv.side_effect = [wire[:8], wire[8:]]
        connection.__enter__.return_value = connection
        with patch("tools.hardware.management_rf_checks.socket.create_connection", return_value=connection):
            self.assertEqual(beta_radio_test.config_get(),
                             {"generation": 0xc0, "frequency_hz": 912525000, "sf": 8, "power": 2})

    def test_native_encrypted_reply_and_corruption(self):
        secret = bytes(range(32))
        sender, target = bytes([21]) * 32, bytes([42]) * 32
        plaintext = b"\x01\x02\x03\x04\x04a1|status"
        packet = b"\x0a\x02\xab\xcd" + bytes([21, 42]) + mast_cli.encrypt(secret, plaintext)
        kind, decoded = mast_cli.decode(secret, sender, target, packet)
        self.assertEqual(kind, 2)
        self.assertEqual(decoded.rstrip(b"\0"), plaintext)
        for route in (0, 3):
            scoped = bytes((8 | route,)) + b"\x12\x34\x56\x78" + packet[1:]
            self.assertEqual(mast_cli.decode(secret, sender, target, scoped), (kind, decoded))
            self.assertIsNone(mast_cli.decode(secret, sender, target, scoped[:-1] + bytes((scoped[-1] ^ 1,))))
        self.assertIsNone(mast_cli.decode(secret, sender, target, packet[:-1] + bytes([packet[-1] ^ 1])))
        for malformed in (b"", b"\x0a", b"\x0a\xff", packet[:10], b"\xca" + packet[1:]):
            self.assertIsNone(mast_cli.decode(secret, sender, target, malformed))

    def test_native_reciprocal_path_preserves_hash_width_and_auth(self):
        secret = bytes(range(32))
        sender, target = bytes([21]) * 32, bytes([42]) * 32
        for width in (1, 2, 3):
            path = bytes(((width - 1) << 6 | 1,)) + bytes(range(width))
            returned = bytes(((width - 1) << 6 | 1,)) + bytes(range(8, 8 + width))
            received = b"\x21" + returned + bytes((21, 42)) + mast_cli.encrypt(secret, path + b"\x01" + bytes(13))
            with patch("tools.hardware.admin.secrets.token_bytes", return_value=b"test"):
                packet = mast_cli.reciprocal_path(secret, sender, target, received, path)
            self.assertEqual(packet[:len(path) + 1], b"\x22" + path)
            kind, plain = mast_cli.decode(secret, target, sender, packet)
            self.assertEqual(kind, 8)
            self.assertEqual(plain.rstrip(b"\0"), returned + b"\xfftest")
            corrupted = received[:-1] + bytes((received[-1] ^ 1,))
            with self.assertRaisesRegex(ValueError, "authenticated"):
                mast_cli.reciprocal_path(secret, sender, target, corrupted, path)

    def test_resumable_bounded_installer(self):
        source = b"function hello(name) reply('Hello '..name) end command('hello','name:string:32','Greet')" + b" " * 4000
        identifier = hashlib.sha256(source).hexdigest()[:16]
        commands = []

        class Client:
            def command(self, text):
                commands.append(text)
                self_test.assertLessEqual(len(text) + 17, 162)
                if text.startswith("source begin "):
                    return f"ACK {identifier} next=2"
                if text.startswith("source chunk "):
                    index = int(text.split()[3])
                    chunk = bytes.fromhex(text.split()[4])
                    self_test.assertEqual(chunk, source[index * 48:(index + 1) * 48])
                    return f"ACK {identifier} next={index + 1}"
                if text.startswith("source commit "):
                    return "Accepted verification"
                if text == "source hash":
                    return f"SHA256 {hashlib.sha256(source).hexdigest()} gen=1"
                return "gen=1 source durably saved and active"

        self_test = self
        with patch("tools.hardware.admin.time.sleep"):
            mast_cli.install(Client(), source, lambda _: None)
        self.assertTrue(commands[1].startswith(f"source chunk {identifier} 2 "))
        for invalid in (b"", b" " * 4097, b"\x1bLua", b"return\0"):
            with self.assertRaises(ValueError):
                mast_cli.install(Client(), invalid)

    def test_installer_never_treats_error_as_activation(self):
        class Client:
            def command(self, text):
                if text.startswith("source begin "):
                    return "ACK " + text.split()[2] + " next=0"
                if text.startswith("source chunk "):
                    return "ACK " + text.split()[2] + " next=1"
                if text.startswith("source commit "):
                    return "Accepted verification"
                return "gen=0; Error: compile failed"
        with patch("tools.hardware.admin.time.sleep"), self.assertRaisesRegex(ValueError, "compile failed"):
            mast_cli.install(Client(), b"not lua", lambda _: None)

    def test_installer_carries_source_base_and_refuses_lost_update(self):
        source = b"function hello() return 'ok' end"
        digest = hashlib.sha256(source).hexdigest()
        base = "a" * 64
        commands = []

        class Client:
            def command(self, text):
                commands.append(text)
                if text.startswith("source begin "):
                    return f"ACK {digest[:16]} next=0"
                if text.startswith("source chunk "):
                    return f"ACK {digest[:16]} next=1"
                if text.startswith("source commit "):
                    return "Error: active Lua sources changed; download and reconcile before installing"
                raise AssertionError("Rejected source must not be treated as activated")

        with self.assertRaisesRegex(ValueError, "sources changed"):
            mast_cli.install(Client(), source, lambda _: None, expected_base_hash=base)
        self.assertEqual(commands[-1], f"source commit {digest[:16]} {base}")
        commands.clear()
        with self.assertRaisesRegex(ValueError, "SHA256"):
            mast_cli.install(Client(), source, expected_base_hash="not-a-hash")
        self.assertEqual(commands, [])


if __name__ == "__main__":
    unittest.main()
