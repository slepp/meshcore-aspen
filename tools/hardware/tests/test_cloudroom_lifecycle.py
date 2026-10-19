# SPDX-License-Identifier: Apache-2.0
"""Shared-room lifecycle subcommands of tools/admin.py against a fake native endpoint."""
import contextlib
import hashlib
import io
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from tools import admin as cli
from tools.hardware import admin

CODE = "ABCD-EFGH-JKMN-PQRS"
CANONICAL = "ABCDEFGHJKMNPQRS"
PASSWORD = "7QX2M9KDAB"
HOST = "rooms.example.com"


def namespace(action, **values):
    base = dict(cloudroom_action=action, code=None, name=None, yes=False, owner=False, number=None, id=None,
                host=None, password=None, open=False, new=False, alias=None, delete=False, local=False,
                force=False, state=None)
    return SimpleNamespace(**(base | values))


class FakeNode:
    """Scripted Management endpoint: records commands, answers by command or from a queue."""
    def __init__(self, replies):
        self.replies = replies
        self.commands = []
        self.closed = False

    def command(self, text):
        self.commands.append(text)
        reply = self.replies(text) if callable(self.replies) else self.replies.pop(0)
        if isinstance(reply, BaseException):
            raise reply
        return reply

    def close(self):
        self.closed = True


def run_cli(arguments, node, stdin=""):
    """Invoke main() against a fake RF endpoint; return (exit code, stdout, stderr)."""
    argv = ["admin.py", "--gateway", "192.0.2.1", "--target", "ab" * 32, "--seed-file", "unused", *arguments]
    out, err = io.StringIO(), io.StringIO()
    code = 0
    with patch.object(sys, "argv", argv), patch.object(admin, "NativeClient", return_value=node), \
            patch.object(admin.time, "sleep"), patch.object(sys, "stdin", io.StringIO(stdin)), \
            contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            cli.main()
        except SystemExit as exit_:
            code = exit_.code
    return code, out.getvalue(), err.getvalue()


class CommandBuilding(unittest.TestCase):
    def build(self, action, stdin="", **values):
        return admin.cloudroom_command(namespace(action, **values), io.StringIO(stdin))

    def test_codes_are_canonicalised_and_hosts_lowered(self):
        self.assertEqual(self.build("join", code=CODE), f"cloudroom join {CANONICAL}")
        self.assertEqual(self.build("join", code="abcd-efgh-jkmn-pqrs@Rooms.Example.COM"),
                         f"cloudroom join {CANONICAL}@{HOST}")
        self.assertEqual(self.build("join", code="OOOO-IIII-LLLL-0123"), "cloudroom join 0000111111110123")
        self.assertEqual(self.build("create", code="open@" + HOST, name="Fleet"), f"cloudroom create OPEN@{HOST} Fleet")

    def test_bad_codes_and_hosts_never_echo_the_secret(self):
        for code in ("ABCDEFGHJKMNPQRU", "ABCD-EFGH", "ABCDEFGHJKMNPQRS-TOOLONG", "ABCDEFGHJKMNPQR!",
                     CANONICAL + "@https://x.example", CANONICAL + "@x.example:443", CANONICAL + "@1.2.3.4",
                     CANONICAL + "@" + "a" * 37, CANONICAL + "@-x.example", "open"):
            with self.assertRaises(ValueError) as caught:
                self.build("join", code=code)
            self.assertNotIn(code.split("@")[0], str(caught.exception), code)
        with self.assertRaises(ValueError):
            self.build("create", code="open@bad_host.example", name="Fleet")

    def test_stdin_keeps_codes_and_passwords_out_of_argv(self):
        self.assertEqual(self.build("join", stdin=CODE + "\n", code="-"), f"cloudroom join {CANONICAL}")
        self.assertEqual(self.build("password", stdin=PASSWORD + "\n", password="-"), f"cloudroom password {PASSWORD}")
        with self.assertRaises(ValueError):
            self.build("join", stdin="", code="-")
        with self.assertRaises(ValueError):
            self.build("password", stdin="x" * 200, password="-")

    def test_names_are_checked_like_the_device(self):
        self.assertEqual(self.build("create", code=CODE, name="Back Yard"), f"cloudroom create {CANONICAL} Back Yard")
        for name in ("", "x" * 32, "a:b", "a[b", "a]b", "a\\b", "a,b", "a?b", "a*b", "café"):
            with self.assertRaises(ValueError, msg=name):
                self.build("create", code=CODE, name=name)

    def test_convert_preview_then_yes_retypes_a_non_default_host(self):
        self.assertEqual(self.build("convert", code=CODE), f"cloudroom convert {CANONICAL}")
        self.assertEqual(self.build("convert", code=CODE, yes=True), f"cloudroom convert {CANONICAL} yes")
        self.assertEqual(self.build("convert", code=f"{CODE}@{HOST}", yes=True),
                         f"cloudroom convert {CANONICAL}@{HOST} yes {HOST}")

    def test_smaller_verbs(self):
        self.assertEqual(self.build("invite"), "cloudroom invite")
        self.assertEqual(self.build("invite", owner=True), "cloudroom invite owner")
        self.assertEqual(self.build("list"), "cloudroom list")
        self.assertEqual(self.build("list", number=2), "cloudroom list 2")
        self.assertEqual(self.build("nodes", number=3), "cloudroom nodes 3")
        self.assertEqual(self.build("revoke", id="AB12cd34"), "cloudroom revoke ab12cd34")
        self.assertEqual(self.build("result"), "cloudroom result")
        self.assertEqual(self.build("apply"), "cloudroom apply")
        self.assertEqual(self.build("probe"), "cloudroom probe")
        self.assertEqual(self.build("probe", host="Rooms.Example.COM"), f"cloudroom probe {HOST}")
        self.assertEqual(self.build("password"), "cloudroom password")
        self.assertEqual(self.build("password", open=True), "cloudroom password open")
        self.assertEqual(self.build("password", new=True), "cloudroom password new")
        self.assertEqual(self.build("password", password="two words"), "cloudroom password two words")
        self.assertEqual(self.build("unlink"), "cloudroom unlink")
        self.assertEqual(self.build("unlink", alias="2", delete=True), "cloudroom unlink 2 delete")
        self.assertEqual(self.build("unlink", alias="TestA", local=True, force=True), "cloudroom unlink TestA local force")
        self.assertEqual(self.build("share", state="on"), "cloudroom share on")
        self.assertEqual(self.build("share", state="off", force=True), "cloudroom share off force")
        for action, values in (("revoke", {"id": "abc"}), ("revoke", {"id": "g" * 8}), ("revoke", {"id": "a" * 17}),
                               ("nodes", {"number": 0}), ("list", {"number": 1000}),
                               ("password", {"password": "x" * 16}), ("password", {"password": "café"}),
                               ("probe", {"host": "https://x.example"})):
            with self.assertRaises(ValueError, msg=(action, values)):
                self.build(action, **values)

    def test_every_request_fits_one_message(self):
        host = "a" * 30 + ".b.com"
        for text in (self.build("join", code=f"{CODE}@{host}"),
                     self.build("create", code=f"{CODE}@{host}", name="n" * 31),
                     self.build("convert", code=f"{CODE}@{host}", yes=True),
                     self.build("unlink", alias="a" * 64, delete=True, force=True)):
            self.assertLessEqual(len(text), 145, text)


class Polling(unittest.TestCase):
    def test_a_queued_job_is_polled_until_it_finishes(self):
        node = FakeNode([f"Contacting {HOST} (up to 20 s). Send \"cloudroom result\" for the outcome; the node restarts when linked.",
                         "Link working.", "Link working.",
                         "Linked. Restarts in 10 s. Ask the room owner for the password."])
        sleeps = []
        text, ok = admin.cloudroom_lifecycle(node, namespace("join", code=CODE), sleep=sleeps.append)
        self.assertTrue(ok)
        self.assertTrue(text.startswith("Linked."))
        self.assertEqual(node.commands, [f"cloudroom join {CANONICAL}", "cloudroom result", "cloudroom result",
                                         "cloudroom result"])
        self.assertEqual(sleeps, [2, 2, 2])

    def test_immediate_replies_are_final_and_not_polled(self):
        node = FakeNode(["Key goes to the default host; it can speak as the room. Admin logins stay. Add \"yes\"."])
        text, ok = admin.cloudroom_lifecycle(node, namespace("convert", code=CODE), sleep=lambda _: self.fail("polled"))
        self.assertTrue(ok)
        self.assertEqual(len(node.commands), 1)

    def test_failures_are_reported_as_unsuccessful(self):
        for failure in ("Link failed: code expired. Ask the owner for a new one (cloudroom invite).",
                        "Server limit reached (rooms, nodes or codes). Ask the server operator.",
                        "Only an owner node can do that. Ask the owner for \"cloudroom invite owner\".",
                        "Too many failed attempts. Send \"reboot\", then retry.",
                        "Usage: cloudroom join CODE[@HOST]",
                        "Linking needs encrypted Management RF; send this from the companion app's Management console."):
            node = FakeNode([f"Contacting {HOST} (up to 20 s).", failure])
            text, ok = admin.cloudroom_lifecycle(node, namespace("join", code=CODE), sleep=lambda _: None)
            self.assertFalse(ok, failure)
            self.assertEqual(text, failure)
        for success in ("Linked. Restarts in 10 s.", "Owner code ABCD-EFGH-JKMN-PQRS (120 s, once). Run: cloudroom join X",
                        "Nodes 1-5 of 2: ab12cd34 owner 2h; 77e1f0aa node 3d", "Room password 7QX2M9KDAB (open: x)",
                        "Unlinked. Restarting in 10 s.", f"Server {HOST} ok: aspen-shared-room 0.1.20 signup=code clock+1s",
                        "Sharing off. Restarting in 10 s; send \"cloudroom share on\" to resume."):
            self.assertTrue(admin.cloudroom_lifecycle(FakeNode([success]), namespace("result"))[1], success)

    def test_polling_gives_up_after_forty_seconds(self):
        node = FakeNode(lambda text: f"Contacting {HOST} (up to 20 s)." if text.startswith("cloudroom join")
                        else "Link working.")
        now = [0]
        def sleep(seconds): now[0] += seconds
        with self.assertRaisesRegex(TimeoutError, "40 s"):
            admin.cloudroom_lifecycle(node, namespace("join", code=CODE), sleep=sleep, clock=lambda: now[0])
        self.assertEqual(now[0], 40)
        self.assertEqual(node.commands.count("cloudroom result"), 20)

    def test_one_lost_poll_does_not_end_the_wait(self):
        node = FakeNode([f"Contacting {HOST} (up to 20 s).", TimeoutError("lost"), "Linked. Restarts in 10 s."])
        now = [0]
        text, ok = admin.cloudroom_lifecycle(node, namespace("join", code=CODE),
                                             sleep=lambda s: now.__setitem__(0, now[0] + s), clock=lambda: now[0])
        self.assertTrue(ok)

    def test_device_errors_raise(self):
        with self.assertRaisesRegex(ValueError, "link busy"):
            admin.cloudroom_lifecycle(FakeNode(['Error: link busy; send "cloudroom result"']), namespace("join", code=CODE))


class CommandLine(unittest.TestCase):
    def test_join_with_stdin_code_prints_the_outcome(self):
        node = FakeNode([f"Contacting {HOST} (up to 20 s).", "Linked. Restarts in 10 s. Ask the room owner for the password."])
        code, out, err = run_cli(["cloudroom", "join", "-"], node, stdin=f"{CODE}@{HOST}\n")
        self.assertEqual(code, 0, err)
        self.assertIn("Linked.", out)
        self.assertEqual(node.commands[0], f"cloudroom join {CANONICAL}@{HOST}")
        self.assertTrue(node.closed)
        self.assertNotIn(CANONICAL, out + err)

    def test_failed_link_exits_non_zero_after_printing_the_reason(self):
        node = FakeNode([f"Contacting {HOST} (up to 20 s).", "Link failed: code expired. Ask the owner for a new one (cloudroom invite)."])
        code, out, err = run_cli(["cloudroom", "join", CODE], node)
        self.assertEqual(code, 1)
        self.assertIn("Link failed: code expired", out)
        self.assertTrue(node.closed)

    def test_invalid_input_exits_before_any_command(self):
        for arguments in (["cloudroom", "join", "ABCDEFGHJKMNPQRU"], ["cloudroom", "create", CODE, "bad:name"],
                          ["cloudroom", "revoke", "xyz"]):
            node = FakeNode([])
            code, out, err = run_cli(arguments, node)
            self.assertEqual(code, 2, arguments)
            self.assertEqual(node.commands, [])
            self.assertNotIn("ABCDEFGHJKMNPQRU", out + err)
            self.assertNotIn(CODE, out + err)

    def test_convert_needs_yes_to_send_the_key(self):
        node = FakeNode(['Key goes to the default host; it can speak as the room. Admin logins stay. Add "yes".'])
        code, out, _ = run_cli(["cloudroom", "convert", CODE], node)
        self.assertEqual((code, node.commands), (0, [f"cloudroom convert {CANONICAL}"]))
        self.assertIn("Add \"yes\"", out)
        node = FakeNode([f"Contacting {HOST} (up to 20 s).", "Linked. Restarts in 10 s."])
        code, _, _ = run_cli(["cloudroom", "convert", f"{CODE}@{HOST}", "--yes"], node)
        self.assertEqual(node.commands[0], f"cloudroom convert {CANONICAL}@{HOST} yes {HOST}")

    def test_password_modes(self):
        for arguments, expected in ((["password"], "cloudroom password"), (["password", "--open"], "cloudroom password open"),
                                    (["password", "--new"], "cloudroom password new"), (["password", "hunter2"], "cloudroom password hunter2")):
            node = FakeNode([f"Room password {PASSWORD} (open: cloudroom password open; change: cloudroom password PW|new)"])
            code, out, _ = run_cli(["cloudroom", *arguments], node)
            self.assertEqual((code, node.commands), (0, [expected]))
        node = FakeNode([])
        code, _, _ = run_cli(["cloudroom", "password", "x", "--new"], node)
        self.assertEqual(code, 2)  # argparse: mutually exclusive

    def test_unlink_share_and_reads(self):
        cases = ((["unlink", "--delete"], "cloudroom unlink delete"), (["unlink", "1", "--local"], "cloudroom unlink 1 local"),
                 (["share", "off", "--force"], "cloudroom share off force"), (["share", "on"], "cloudroom share on"),
                 (["list", "2"], "cloudroom list 2"), (["nodes"], "cloudroom nodes"), (["result"], "cloudroom result"),
                 (["apply"], "cloudroom apply"), (["probe", HOST], f"cloudroom probe {HOST}"),
                 (["invite", "--owner"], "cloudroom invite owner"))
        for arguments, expected in cases:
            node = FakeNode(["Done."])
            code, _, err = run_cli(["cloudroom", *arguments], node)
            self.assertEqual((code, node.commands), (0, [expected]), (arguments, err))

    def test_other_transports_refuse_before_connecting(self):
        for transport in (["--web", "http://node"], ["--unix-socket", "/nonexistent"], ["--willow-socket", "/nonexistent"]):
            for verb in (["join", CODE], ["list"], ["share", "on"], ["password"], ["apply"]):
                with patch.object(sys, "argv", ["admin.py", *transport, "cloudroom", *verb]), \
                        patch.object(admin, "WebClient") as web, patch.object(admin, "UnixClient") as unix, \
                        patch.object(admin, "WillowClient") as willow, \
                        contextlib.redirect_stderr(io.StringIO()) as err, self.assertRaises(SystemExit) as caught:
                    cli.main()
                self.assertEqual(caught.exception.code, 2)
                for client in (web, unix, willow):
                    client.assert_not_called()
                self.assertIn("encrypted Management RF", err.getvalue())
                self.assertNotIn(CANONICAL, err.getvalue())

    def test_existing_cloudroom_subcommands_keep_their_commands(self):
        for arguments, expected in ((["status"], "cloudroom config status"), (["hash"], "cloudroom config hash"),
                                    (["enable", "off"], "cloudroom enable off")):
            node = FakeNode(["ok"])
            code, _, _ = run_cli(["cloudroom", *arguments], node)
            self.assertEqual((code, node.commands), (0, [expected]))


class ConfigureAbi(unittest.TestCase):
    def v2_record(self):
        body = b"CRC\2" + bytes(9304 - 4 - 32)
        return body + hashlib.sha256(body).digest()

    def test_schema_2_profile_asks_for_abi_2(self):
        record = self.v2_record()
        digest, identifier = record[-32:].hex(), record[-32:].hex()[:16]
        calls = []
        def reply(text):
            calls.append(text)
            if text == "cloudroom config api 2":
                return admin.CLOUDROOM_ABI2
            if text == "cloudroom config begin " + digest:
                return "Cloud room upload ready received=0"
            if text.startswith("cloudroom config chunk "):
                offset = int(text.split()[4]) * 48
                self.assertEqual(bytes.fromhex(text.split()[5]), record[offset:offset + 48])
                return f"Cloud room chunk saved received={offset + len(bytes.fromhex(text.split()[5]))}"
            if text == "cloudroom config commit " + identifier:
                return "Cloud room settings saved; restart to apply"
            if text == "cloudroom config hash":
                return digest
            self.fail(text)
        with patch("tools.hardware.cloudroom_config.encode", return_value=record):
            self.assertEqual(admin.configure_cloudroom(FakeNode(reply), "{}", progress=lambda _: None), digest)
        self.assertNotIn("cloudroom config api", calls)
        self.assertEqual(calls[0], "cloudroom config api 2")
        self.assertEqual(sum(text.startswith("cloudroom config chunk ") for text in calls), 194)

    def test_old_node_refuses_a_schema_2_profile_before_any_upload(self):
        for answer in ("Error: cloudroom status|error|advertise ALIAS|config|enable on|off", admin.CLOUDROOM_ABI1):
            node = FakeNode(lambda text, answer=answer: answer)
            with patch("tools.hardware.cloudroom_config.encode", return_value=self.v2_record()), \
                    self.assertRaisesRegex(ValueError, "update the node firmware"):
                admin.configure_cloudroom(node, "{}", progress=lambda _: None)
            self.assertEqual(node.commands, ["cloudroom config api 2"])

    def test_v1_profiles_keep_the_abi_1_exchange(self):
        record = b"CRC\1" + bytes(9296 - 4 - 32)
        record += hashlib.sha256(record).digest()
        node = FakeNode(lambda text: admin.CLOUDROOM_ABI1 if text == "cloudroom config api" else "stop")
        with patch("tools.hardware.cloudroom_config.encode", return_value=record), self.assertRaises(ValueError):
            admin.configure_cloudroom(node, "{}", progress=lambda _: None)
        self.assertEqual(node.commands[:2], ["cloudroom config api", "cloudroom config begin " + record[-32:].hex()])


if __name__ == "__main__":
    unittest.main()
