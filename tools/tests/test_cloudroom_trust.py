# SPDX-License-Identifier: Apache-2.0
"""The generated root list and the chain rule the device applies to it; no network."""
from datetime import datetime, timedelta, timezone
from pathlib import Path
import re
import sys
import tempfile
import unittest
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa
from cryptography.x509.oid import NameOID

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import cloudroom_trust as trust


def name(common):
    return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, common)])


def issue(subject, issuer, key, signer, authority=True):
    now = datetime.now(timezone.utc)
    return (x509.CertificateBuilder().subject_name(name(subject)).issuer_name(name(issuer))
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - timedelta(days=1)).not_valid_after(now + timedelta(days=30))
            .add_extension(x509.BasicConstraints(ca=authority, path_length=None), critical=True)
            .sign(signer, hashes.SHA256()))


class CloudRoomTrust(unittest.TestCase):
    def committed(self):
        text = (ROOT / "firmware/esp32/CloudRoomRoots.h").read_text()
        body = text[text.index("cloudRoomRootBundle[] = {"):]
        return bytes(int(byte, 16) for byte in re.findall(r"0x([0-9a-f]{2})", body))

    def test_committed_list_holds_every_pinned_root_sorted_by_subject(self):
        table = trust.parse_bundle(self.committed())
        self.assertEqual(len(table), len(trust.ROOTS))
        subjects = list(table)
        self.assertEqual(subjects, sorted(subjects))
        for common in trust.ROOTS:
            self.assertEqual(sum(common.encode() in subject for subject in subjects), 1, common)
        for key in table.values():
            serialization.load_der_public_key(key)
        self.assertLess(len(self.committed()), 6 * 1024)

    def test_chain_rule_matches_the_sdk_verifier(self):
        root_key, middle_key, leaf_key = (ec.generate_private_key(ec.SECP256R1()) for _ in range(3))
        root = issue("Test Root", "Test Root", root_key, root_key)
        middle = issue("Test Intermediate", "Test Root", middle_key, root_key)
        leaf = issue("room.example", "Test Intermediate", leaf_key, middle_key, authority=False)
        table = trust.parse_bundle(trust.bundle({"Test Root": root})[0])
        self.assertIs(trust.verify_chain([leaf, middle], table), middle)
        # A server that also sends a cross-signed copy of the root: the cross certificate's issuer
        # must be listed too, which is why GlobalSign Root CA is in the list.
        other_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        other = issue("Other Root", "Other Root", other_key, other_key)
        cross = issue("Test Root", "Other Root", root_key, other_key)
        with self.assertRaises(ValueError):
            trust.verify_chain([leaf, middle, cross], table)
        both = trust.parse_bundle(trust.bundle({"Test Root": root, "Other Root": other})[0])
        self.assertEqual(trust.verify_chain([leaf, middle, cross], both).subject, cross.subject)
        # An unlisted root, a wrong key and a reordered chain are refused.
        with self.assertRaises(ValueError):
            trust.verify_chain([leaf, middle], trust.parse_bundle(trust.bundle({"Other Root": other})[0]))
        forged = issue("Test Root", "Test Root", leaf_key, leaf_key)
        with self.assertRaises(Exception):
            trust.verify_chain([leaf, middle], trust.parse_bundle(trust.bundle({"Test Root": forged})[0]))

    def test_generator_refuses_a_root_with_another_fingerprint(self):
        key = ec.generate_private_key(ec.SECP256R1())
        impostor = issue("ISRG Root X1", "ISRG Root X1", key, key)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "roots.pem"
            path.write_bytes(impostor.public_bytes(serialization.Encoding.PEM))
            with self.assertRaises(SystemExit):
                trust.load([path])


if __name__ == "__main__":
    unittest.main()
