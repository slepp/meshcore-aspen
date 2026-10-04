# SPDX-License-Identifier: Apache-2.0
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import patch

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "runtime"))
from tools.hardware import admin as mast_cli


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]


class PackageFetchTests(unittest.TestCase):
    def test_streaming_fetch_rejects_invalid_alias_and_hash_before_device_access(self):
        class Client:
            def command(self, command):
                raise AssertionError(f"Device must not be contacted for invalid input: {command}")

        for alias, digest, message in (
            ("../package", "ab" * 32, "fixed 'package' endpoint alias"),
            ("packages", "ab" * 32, "fixed 'package' endpoint alias"),
            ("package", "AB" * 32, "lowercase expected SHA256"),
        ):
            with self.subTest(alias=alias, digest=digest), self.assertRaisesRegex(ValueError, message):
                mast_cli.fetch_package(Client(), alias, digest)

    def test_streaming_package_fetch_requires_native_contract_and_verifies_hash(self):
        expected = "ab" * 32
        calls = []
        fetch_command = "source fetch package " + expected

        class Client:
            def command(self, command):
                calls.append(command)
                if command == "source api fetch":
                    return mast_cli.PACKAGE_FETCH_API
                if command == "source api package":
                    return ("Package runtime=lua-5.5.1 api=named-commands-v1 "
                            "caps=events,kv,kv.atomic,mesh,modules,reminders,timers,utilities")
                if command == "source hash":
                    return f"SHA256 {expected} gen=2" if fetch_command in calls else "SHA256 " + "00" * 32 + " gen=1"
                if command.startswith("source fetch "):
                    return "Accepted streaming package fetch"
                if command == "source status":
                    return "gen=2 active=1 prev=3 durable; durably saved and active"
                if command == "source metadata":
                    return "META name=notes ver=1.0.0 schema=none rollback=none"
                return "OK"

        with patch("tools.hardware.admin.time.sleep"), patch("builtins.print"):
            result = mast_cli.fetch_package(Client(), "package", expected)
        self.assertIn("SHA256 " + expected + " gen=2", result)
        self.assertIn(fetch_command, calls)

    def test_streaming_package_fetch_rejects_missing_native_contract(self):
        class Client:
            def command(self, command):
                if command == "source api fetch":
                    return "Owner fetch unavailable"
                raise AssertionError(f"Device must not receive package fetch commands: {command}")

        with self.assertRaisesRegex(ValueError, "native streaming package fetch"):
            mast_cli.fetch_package(Client(), "package", "ab" * 32)

    def test_unknown_fetch_outcome_is_reported_without_claiming_installation(self):
        calls = []

        class Client:
            def command(self, command):
                calls.append(command)
                if command == "source api fetch":
                    return mast_cli.PACKAGE_FETCH_API
                if command == "source hash":
                    return "SHA256 " + "00" * 32 + " gen=1"
                if command.startswith("source fetch "):
                    return "Accepted streaming package fetch"
                if command == "source status":
                    return "gen=1 active=2 prev=3; Error: package GET outcome unknown after submission"
                raise AssertionError(f"Unexpected command after failed fetch: {command}")

        with patch("tools.hardware.admin.time.sleep"), patch("builtins.print"):
            with self.assertRaisesRegex(ValueError, "outcome unknown"):
                mast_cli.fetch_package(Client(), "package", "ab" * 32)
        self.assertNotIn("source metadata", calls)

    def test_real_cli_webclient_drives_authenticated_package_fetch_lifecycle(self):
        expected = "42" * 32
        password = b"test-password"
        session = "0123456789abcdef0123456789abcdef"

        class AdminFixture(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def reply(self, value, status=200):
                body = value.encode("ascii")
                self.send_response(status)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_POST(self):
                size = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(size)
                state = self.server.state
                if self.path == "/admin/login":
                    if body != password or self.headers.get("X-Mast-Session"):
                        state["errors"].append("invalid login request")
                        self.reply("Error: invalid fixture login", 401)
                    else:
                        self.reply(session)
                    return
                if self.headers.get("X-Mast-Session") != session:
                    state["errors"].append("missing authenticated session")
                    self.reply("Error: fixture session required", 401)
                    return
                if self.path == "/admin/logout":
                    state["logged_out"] = True
                    self.reply("Logged out")
                    return
                if self.path != "/admin/command":
                    state["errors"].append("unexpected fixture route")
                    self.reply("Error: unknown fixture route", 404)
                    return
                command = body.decode("ascii")
                state["commands"].append(command)
                if command == "source api fetch":
                    self.reply(mast_cli.PACKAGE_FETCH_API)
                elif command == "source hash":
                    digest = expected if state["activated"] else "00" * 32
                    generation = 5 if state["activated"] else 4
                    self.reply(f"SHA256 {digest} gen={generation}")
                elif command == f"source fetch package {expected}" and not state["fetch_started"]:
                    state["fetch_started"] = True
                    self.reply("Accepted package fetch; inspect source status")
                elif command == "source status" and state["fetch_started"]:
                    state["status_polls"] += 1
                    if state["status_polls"] == 1:
                        self.reply("gen=4 active=0 prev=3 durable; package fetch running")
                    else:
                        state["activated"] = True
                        self.reply("gen=5 active=1 prev=4 durable; durably saved and active")
                elif command == "source metadata" and state["activated"]:
                    self.reply("META name=fixture ver=1.0.0 schema=none rollback=none")
                else:
                    state["errors"].append("unexpected admin command")
                    self.reply("Error: unexpected fixture command", 400)

        temporary_root = REPOSITORY_ROOT / ".tmp"
        temporary_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="bot-package-cli-", dir=temporary_root) as directory:
            password_path = Path(directory) / "mast-password"
            descriptor = os.open(password_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(password)
            server = ThreadingHTTPServer(("127.0.0.1", 0), AdminFixture)
            server.daemon_threads = True
            server.state = {
                "activated": False,
                "commands": [],
                "errors": [],
                "fetch_started": False,
                "logged_out": False,
                "status_polls": 0,
            }
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                environment = os.environ.copy()
                environment["PYTHONDONTWRITEBYTECODE"] = "1"
                environment["NO_PROXY"] = ",".join(filter(None, (
                    environment.get("NO_PROXY"), "127.0.0.1", "localhost")))
                environment["no_proxy"] = environment["NO_PROXY"]
                result = subprocess.run(
                    [sys.executable, str(REPOSITORY_ROOT / "tools/hardware/admin.py"),
                     "--web", f"http://127.0.0.1:{server.server_port}",
                     "--password-file", str(password_path), "package-fetch", "package", expected],
                    cwd=REPOSITORY_ROOT, env=environment, capture_output=True, text=True, timeout=10,
                )
            finally:
                server.shutdown()
                thread.join()
                server.server_close()

        self.assertEqual(result.returncode, 0, f"stdout={result.stdout}\nstderr={result.stderr}")
        self.assertIn("gen=5 active=1 prev=4 durable; durably saved and active", result.stdout)
        self.assertIn("META name=fixture ver=1.0.0 schema=none rollback=none", result.stdout)
        self.assertNotIn(password.decode("ascii"), result.stdout + result.stderr)
        self.assertEqual(server.state["errors"], [])
        self.assertTrue(server.state["logged_out"])
        self.assertEqual(server.state["commands"], [
            "source api fetch",
            "source hash",
            f"source fetch package {expected}",
            "source status",
            "source hash",
            "source status",
            "source hash",
            "source metadata",
        ])


if __name__ == "__main__":
    unittest.main()
