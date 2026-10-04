# SPDX-License-Identifier: Apache-2.0
import http.client
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.hardware import https_checks as peer
ROOT = Path(__file__).resolve().parents[3]


class HttpsChecks(unittest.TestCase):
    def test_peer_readiness_recovers_incomplete_http_within_deadline(self):
        state = {"kiss": {"capacity": 3},
                 "roles": [{"role": "command-bot", "name": "HTTPS peer bot"}]}
        with patch.object(peer, "dashboard", side_effect=[http.client.IncompleteRead(b"partial"), state]), \
                patch.object(peer.time, "sleep") as sleep, patch.object(peer, "save") as save, \
                patch("builtins.print") as log:
            self.assertEqual(peer.wait_dashboard(), state)
        sleep.assert_called_once_with(1)
        log.assert_called_once()
        save.assert_called_once_with("readiness.json", {"incomplete_responses": 1, "ready": True})

    def test_peer_readiness_persistent_incomplete_http_fails_visibly(self):
        with patch.object(peer, "dashboard", side_effect=http.client.IncompleteRead(b"partial")), \
                patch.object(peer.time, "monotonic", side_effect=[0, 0, 120]), \
                patch.object(peer.time, "sleep"), patch.object(peer, "save") as save, \
                patch("builtins.print") as log:
            with self.assertRaisesRegex(TimeoutError, "did not become ready"):
                peer.wait_dashboard()
        log.assert_called_once()
        save.assert_called_once_with("readiness-failed.json",
                                     {"dashboard_seen": None, "incomplete_responses": 1})

    def test_peer_archives_attempts_without_moving_live_fixture_state(self):
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp") as directory:
            root = Path(directory)
            (root / "serial.log").write_bytes(b"previous probe")
            (root / "restored.json").write_text('{"verified":true}')
            (root / "fixture-counts.json").write_text("[]")
            with patch.object(peer, "DIRECTORY", root):
                peer.archive_attempt()
            archives = list(root.glob("previous-*"))
            self.assertEqual(len(archives), 1)
            self.assertEqual(archives[0].stat().st_mode & 0o777, 0o700)
            self.assertEqual((archives[0] / "serial.log").read_bytes(), b"previous probe")
            self.assertFalse((root / "restored.json").exists())
            self.assertEqual((root / "fixture-counts.json").read_text(), "[]")

    def test_peer_records_restore_only_after_readback(self):
        with patch.object(peer.stock, "verify", side_effect=OSError("unavailable")), \
                patch.object(peer, "record_restore") as record:
            with self.assertRaises(OSError):
                peer.verify_restore()
        record.assert_not_called()
        with patch.object(peer.stock, "verify"), patch.object(peer, "record_restore") as record:
            peer.verify_restore()
        record.assert_called_once_with()

    def test_peer_report_rejects_previous_attempt_restore(self):
        with tempfile.TemporaryDirectory(dir=ROOT / ".tmp") as directory:
            root = Path(directory)
            with patch.object(peer, "DIRECTORY", root):
                peer.save("serial.log", peer.DONE.encode())
                peer.save("restored.json", {"verified": True})
                os.utime(root / "restored.json", (1, 1))
                os.utime(root / "serial.log", (2, 2))
                with self.assertRaisesRegex(ValueError, "predates this serial capture"):
                    peer.report()
            self.assertFalse((root / "physical-result.json").exists())

    def test_peer_identification_uses_role_not_generic_dashboard_title(self):
        state = {"device_name": "MeshCore Radio", "kiss": {"capacity": 3},
                 "roles": [{"role": "command-bot", "name": "HTTPS peer bot"}]}
        self.assertTrue(peer.is_candidate(state))
        self.assertFalse(peer.is_candidate(state | {"kiss": {"capacity": 8}}))
        self.assertFalse(peer.is_candidate(state | {"roles": []}))
        self.assertFalse(peer.is_candidate({"device_name": "meshcore-https-peer"}))
