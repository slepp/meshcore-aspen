import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "pine_time_refresh", Path(__file__).resolve().parents[1] / "time_refresh.py")
refresh_time = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(refresh_time)


class Client:
    def __init__(self, reply):
        self.reply = reply
        self.commands = []

    def command(self, text):
        self.commands.append(text)
        return self.reply if text == "bot time" else "OK"


class TimeRefreshTests(unittest.TestCase):
    def test_sets_explicit_utc_and_reads_trust(self):
        client = Client("UTC trusted=1 bounds=1790890000..1790890001")
        self.assertEqual(refresh_time.refresh(client, 1790890000), client.reply)
        self.assertEqual(client.commands, ["bot time 1790890000", "bot time"])

    def test_missing_or_false_trust_fails(self):
        for reply in ("OK", "UTC trusted=0 bounds=0..0", "UTC trusted=10"):
            with self.subTest(reply=reply), self.assertRaises(ValueError):
                refresh_time.refresh(Client(reply), 1790890000)

    def test_remote_error_is_not_success(self):
        with self.assertRaisesRegex(ValueError, "storage unavailable"):
            refresh_time.refresh(Client("Error: storage unavailable"), 1790890000)

    def test_invalid_utc_never_sends(self):
        for now in (0, 1715770350, 4102444801):
            client = Client("UTC trusted=1")
            with self.subTest(now=now), self.assertRaises(ValueError):
                refresh_time.refresh(client, now)
            self.assertEqual(client.commands, [])

    def test_unsynchronized_host_fails(self):
        with patch.object(refresh_time.subprocess, "run") as run:
            run.return_value.stdout = "no\n"
            with self.assertRaisesRegex(ValueError, "not NTP-synchronized"):
                refresh_time.synchronized()

    def test_synchronized_host_passes(self):
        with patch.object(refresh_time.subprocess, "run") as run:
            run.return_value.stdout = "yes\n"
            refresh_time.synchronized()
