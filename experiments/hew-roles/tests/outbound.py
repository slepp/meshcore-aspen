"""Isolated TCP/TLS, deadlines and supervisor-owned native resource checks."""
import os
from pathlib import Path
import re
import shutil
import socket
import ssl
import struct
import subprocess
import threading
import time
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("MESHCORE_HEW_OUTBOUND_BIN", ROOT / "build/outbound-checks"))
STD_TLS = Path(os.environ.get("MESHCORE_HEW_STD_TLS_BIN", ROOT / "build/std-tls-deadline-probe"))


class TLSFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = ROOT / "build" / ("outbound-fixture-" + uuid.uuid4().hex)
        cls.directory.mkdir(mode=0o700)
        cls.addClassCleanup(shutil.rmtree, cls.directory)
        cls.cert, cls.key = cls.directory / "cert.pem", cls.directory / "key.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(cls.key), "-out", str(cls.cert),
            "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost", "-days", "2",
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        cls.tls.load_cert_chain(cls.cert, cls.key)


class Outbound(TLSFixture):
    def run_probe(self, mode, url, ca=""):
        result = subprocess.run([str(BINARY), mode, url, str(ca)],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("OUTBOUND_CHECKS_OK", result.stdout)
        self.assertIn("OUTBOUND_HEALTHY_DURING_CONNECT", result.stdout)
        return result.stdout

    def exercise(self, *, tls=False, reject=False, trusted=True, hostname="localhost",
                 panic=False, stall=False, reset=False, backpressure=False):
        errors = []
        done = threading.Event()
        with socket.socket() as listener:
            if backpressure:
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            listener.settimeout(8)

            def peer():
                try:
                    for index in range(2 if panic else 1):
                        connection, _ = listener.accept()
                        connection.settimeout(6)
                        try:
                            if stall:
                                done.wait(7)
                                continue
                            if tls:
                                try:
                                    connection = self.tls.wrap_socket(connection, server_side=True)
                                except ssl.SSLError:
                                    if reject:
                                        continue
                                    raise
                            if reject:
                                try:
                                    rejected_data = connection.recv(12)
                                except (ssl.SSLError, ConnectionResetError, BrokenPipeError):
                                    rejected_data = b""
                                self.assertEqual(rejected_data, b"")
                                continue
                            message = b""
                            while len(message) < 12:
                                part = connection.recv(12 - len(message))
                                if not part:
                                    break
                                message += part
                            self.assertEqual(message, b"HEW_OUTBOUND")
                            for at in range(0, 12, 3):
                                connection.sendall(message[at:at + 3])
                            if reset:
                                time.sleep(0.1)
                                connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                                      struct.pack("ii", 1, 0))
                                continue
                            if backpressure:
                                done.wait(7)
                                continue
                            try:
                                closed_data = connection.recv(1)
                            except (ssl.SSLEOFError, ConnectionResetError, BrokenPipeError):
                                if not (tls and panic and index == 0):
                                    raise
                                closed_data = b""
                            self.assertEqual(closed_data, b"")
                        finally:
                            connection.close()
                except BaseException as error:
                    errors.append(error)

            thread = threading.Thread(target=peer)
            thread.start()
            try:
                mode = "reject" if reject else ("panic" if panic else "echo")
                if reset:
                    mode = "reset"
                if backpressure:
                    mode = "backpressure"
                scheme = "https" if tls else "http"
                url = f"{scheme}://{hostname}:{listener.getsockname()[1]}"
                output = self.run_probe(mode, url, self.cert if tls and trusted else "")
            finally:
                done.set()
                thread.join(timeout=8)
            self.assertFalse(thread.is_alive())
            if errors:
                raise errors[0]
            if panic:
                self.assertIn("OUTBOUND_OWNER_REPLACED", output)
            if reject:
                self.assertIn("OUTBOUND_CONNECT_FAILED", output)
            return output

    def test_plaintext_connect_only_and_echo(self):
        self.exercise()

    def test_verified_tls_and_echo(self):
        self.exercise(tls=True)

    def test_untrusted_certificate_is_rejected(self):
        output = self.exercise(tls=True, trusted=False, reject=True)
        self.assertIn("curl_status=60", output)

    def test_certificate_hostname_is_verified(self):
        output = self.exercise(tls=True, hostname="127.0.0.1", reject=True)
        self.assertIn("curl_status=60", output)

    def test_tls_handshake_deadline_keeps_other_actor_live(self):
        output = self.exercise(tls=True, stall=True, reject=True)
        elapsed = int(re.search(r"OUTBOUND_CONNECT_FAILED elapsed_ms=(\d+)", output)[1])
        self.assertGreaterEqual(elapsed, 2900)
        self.assertLess(elapsed, 3600)
        self.assertIn("curl_status=28", output)

    def test_plaintext_owner_panic_closes_and_replaces(self):
        self.exercise(panic=True)

    def test_tls_owner_panic_closes_and_replaces(self):
        self.exercise(tls=True, panic=True)

    def test_reset_during_plaintext_write_is_a_value(self):
        self.assertIn("OUTBOUND_WRITE_FAILED", self.exercise(reset=True))

    def test_reset_during_tls_write_is_a_value(self):
        self.assertIn("OUTBOUND_WRITE_FAILED", self.exercise(tls=True, reset=True))

    def test_plaintext_backpressure_reports_prefix_without_actor_failure(self):
        output = self.exercise(backpressure=True)
        self.assertIn("deadline expired; committed_prefix=", output)

    def test_tls_backpressure_reports_prefix_without_actor_failure(self):
        output = self.exercise(tls=True, backpressure=True)
        self.assertIn("deadline expired; committed_prefix=", output)

    def test_endpoint_policy_rejects_before_dial(self):
        for url in (
            "http://invalid.example:1883", "http://localhost.invalid.example:1883",
            "http://user@127.0.0.1:1883", "http://127.0.0.1:1883?secret=value",
            "http://127.0.0.1:1883#fragment", "mqtt://127.0.0.1:1883",
        ):
            with self.subTest(url=url):
                output = self.run_probe("reject", url)
                self.assertIn("requires an allowed endpoint", output)

    def test_standard_tls_write_has_no_three_second_handshake_deadline(self):
        accepted, release = threading.Event(), threading.Event()
        errors = []
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            listener.settimeout(5)

            def peer():
                try:
                    with listener.accept()[0] as connection:
                        accepted.set()
                        release.wait(6)
                        connection.shutdown(socket.SHUT_RDWR)
                except BaseException as error:
                    errors.append(error)

            thread = threading.Thread(target=peer)
            thread.start()
            process = subprocess.Popen([str(STD_TLS), str(listener.getsockname()[1])],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                self.assertTrue(accepted.wait(3))
                self.assertEqual(process.stdout.readline().strip(), "STD_TLS_CONNECT_RETURNED")
                time.sleep(3.3)
                self.assertIsNone(process.poll(), "write unexpectedly has a shorter deadline")
                release.set()
                stdout, stderr = process.communicate(timeout=3)
                self.assertEqual(process.returncode, 0, stdout + stderr)
                self.assertIn("STD_TLS_WRITE_RETURNED_ERROR", stdout)
            finally:
                release.set()
                if process.poll() is None:
                    process.terminate()
                    process.communicate(timeout=3)
                thread.join(timeout=7)
            self.assertFalse(thread.is_alive())
            if errors:
                raise errors[0]


if __name__ == "__main__":
    unittest.main(verbosity=2)
