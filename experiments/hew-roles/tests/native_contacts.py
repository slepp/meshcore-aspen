#!/usr/bin/env python3
"""Saved Base contacts, actual native worker, and a loopback-only shared modem."""
import base64
import copy
import hashlib
from itertools import count
import json
import os
from pathlib import Path
import shutil
import struct
import time
import unittest

from cryptography.hazmat.primitives.asymmetric import ed25519
from cryptography.hazmat.primitives import serialization
from base_service import saved_state
from parity import ROOT, public, packet, parse
from service_demo import RunningService, advert, body, text
from willow import owner_rpc


SEQUENCE = count()
TEMPLATE = saved_state()


def contact(key, raw, arrays=False):
    encode = list if arrays else lambda value: base64.b64encode(value).decode()
    return {"PublicKey": encode(key), "Advert": encode(raw), "Type": 1,
            "Flags": 0, "OutPath": [0] * 64, "OutPathLen": 128,
            "AdvertName": "Retained caller", "LastAdvert": 1}


def document(records):
    state = copy.deepcopy(TEMPLATE)
    state["Contacts"] = records
    return state


def matching_contacts(total):
    caller = public(2)
    records = [contact(caller, advert(2))]
    index = 0
    while len(records) < total:
        seed = hashlib.sha256(b"Willow saved contact fixture" + struct.pack("<I", index)).digest()
        index += 1
        signer = ed25519.Ed25519PrivateKey.from_private_bytes(seed)
        key = signer.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        if key[:1] != caller[:1]:
            continue
        prefix = key + struct.pack("<I", 1700000000)
        app = b"\x81Retained collision"
        records.append(contact(key, packet(4, prefix + signer.sign(prefix + app) + app,
                                           route=1, width=3)))
    return records


class NativeContacts(unittest.TestCase):
    def setUp(self):
        self.common = ROOT.parents[1] / f".c-{os.getpid()}-{next(SEQUENCE)}"
        self.common.mkdir(mode=0o700)
        self.service = None
        self.stamp = 1700000100

    def tearDown(self):
        try:
            if self.service:
                self.service.close()
        finally:
            shutil.rmtree(self.common)

    def save(self, authority, records, envelope=False):
        directory = self.common / authority
        directory.mkdir(mode=0o700, exist_ok=True)
        value = document(records)
        if envelope:
            identity = bytearray(hashlib.sha512(bytes([1]) * 32).digest())
            identity[0] &= 248
            identity[31] = (identity[31] & 63) | 64
            value = {"version": 1, "document": "companion.json",
                     "identity": list(identity), "state": value}
        path = directory / ("identity-state.json" if envelope else "companion.json")
        candidate = path.with_suffix(".candidate")
        candidate.write_text(json.dumps(value, separators=(",", ":")))
        candidate.chmod(0o600)
        candidate.replace(path)
        return path

    def start(self):
        self.service = RunningService(root=self.common / "willow")
        self.assertEqual(self.admin("bot key"), "KEY " + public(5).hex())
        self.assertIn("lookup=companion", self.admin("bot contacts"))
        return self.service.emulator

    def admin(self, command):
        return owner_rpc(self.service.root, b"\x01\x04" + command.encode()).decode()

    def send(self, command="!ping"):
        self.stamp += 1
        raw = text(2, 5, self.stamp, command)
        self.service.emulator.send(2, 0, raw)
        return raw

    def reply(self):
        item = self.service.emulator.receive(lambda job: job["port"] == 2 and parse(job["raw"])[0] == 2)
        return body(item["raw"], 2, 5)[5:].rstrip(b"\0")

    def reject(self, raw=None):
        modem = self.service.emulator
        first = len(modem.all_submissions)
        modem.send(2, 0, raw) if raw else self.send()
        time.sleep(.7)
        self.assertFalse(any(job["port"] == 2 and parse(job["raw"])[0] in (2, 3, 8)
                             for job in modem.all_submissions[first:]))
        self.assertIn("Contacts=0/16", self.admin("bot contacts"))

    def authority_hashes(self):
        return {str(path.relative_to(self.common)): hashlib.sha256(path.read_bytes()).hexdigest()
                for name in ("base", "secondary") for path in (self.common / name).glob("*")
                if path.is_file()}

    def worker(self):
        children = set()
        for task in (Path("/proc") / str(self.service.proc.pid) / "task").iterdir():
            children.update(map(int, (task / "children").read_text().split()))
        self.assertEqual(len(children), 1)
        return children.pop()

    def test_primary_restart_preserves_data_and_identity_without_advert(self):
        self.save("base", [contact(public(2), advert(2))])
        before = self.authority_hashes()
        modem = self.start()
        original_worker = self.worker()
        self.send("!remember camp retained-recovery")
        self.assertIn(b"committed", self.reply())
        self.service.stop_process()
        self.assertFalse((Path("/proc") / str(original_worker)).exists())
        self.service.start_process(native=True)
        self.assertNotEqual(self.worker(), original_worker)
        self.assertEqual(self.admin("bot key"), "KEY " + public(5).hex())
        self.send("!recall camp")
        self.assertIn(b"retained-recovery", self.reply())
        self.assertIn("observed=0", self.admin("bot contacts"))
        self.assertIn("recovered=1", self.admin("bot contacts"))
        self.assertEqual(self.authority_hashes(), before)
        self.assertTrue(all(raw[3] == "0100" for raw in modem.controls if raw[2] == 34))

    def test_secondary_envelope_overrides_stale_companion_and_deduplicates(self):
        primary = self.save("base", [])
        state = document([])
        state["Contacts"] = None
        primary.write_text(json.dumps(state))
        self.save("secondary", [contact(public(6), advert(6))])
        self.save("secondary", [contact(public(2), advert(2), arrays=True),
                                contact(public(2), advert(2))], envelope=True)
        before = self.authority_hashes()
        self.start()
        self.send()
        self.assertIn(b"Pong", self.reply())
        self.assertIn("Contacts=1/16", self.admin("bot contacts"))
        self.assertEqual(self.authority_hashes(), before)

    def test_empty_active_envelope_does_not_use_stale_companion(self):
        self.save("secondary", [contact(public(2), advert(2))])
        self.save("secondary", [], envelope=True)
        self.start()
        self.reject()

    def test_hash_collisions_keep_full_keys_and_fixed_sixteen_slots(self):
        records = matching_contacts(16)
        self.save("base", records)
        self.save("secondary", records, envelope=True)
        before = self.authority_hashes()
        self.start()
        worker = self.worker()
        descriptors = len(list((Path("/proc") / str(worker) / "fd").iterdir()))
        raw = self.send()
        self.assertIn(b"Pong", self.reply())
        status = self.admin("bot contacts")
        self.assertIn("Contacts=16/16 observed=0", status)
        self.assertIn("recovered=16", status)
        first = len(self.service.emulator.all_submissions)
        self.service.emulator.send(2, 0, raw)
        time.sleep(.5)
        self.assertFalse(any(job["port"] == 2 and parse(job["raw"])[0] == 2
                             for job in self.service.emulator.all_submissions[first:]))
        self.assertEqual(len(list((Path("/proc") / str(worker) / "fd").iterdir())), descriptors)
        self.assertEqual(self.authority_hashes(), before)

    def test_seventeenth_matching_full_key_rejects_lookup(self):
        self.save("base", matching_contacts(17))
        self.start()
        self.reject()
        self.assertTrue(any("more than16 signed full keys" in line for line in self.service.errors))

    def test_invalid_signature_and_wrong_full_key_are_not_imported(self):
        invalid = bytearray(advert(2))
        invalid[-1] ^= 1
        self.save("base", [contact(public(2), bytes(invalid)), contact(public(2), advert(6))])
        self.start()
        self.reject()
        self.save("secondary", [contact(public(2), advert(2))], envelope=True)
        time.sleep(.4)
        self.send()
        self.assertIn(b"Pong", self.reply())

    def test_unsafe_and_malformed_active_authorities_fail_closed(self):
        primary = self.save("base", [contact(public(2), advert(2))])
        envelope = self.save("secondary", [contact(public(2), advert(2))], envelope=True)
        self.start()
        for kind in ("permissions", "symlink", "malformed", "wrong_format"):
            with self.subTest(kind=kind):
                if kind == "permissions":
                    primary.chmod(0o644)
                elif kind == "symlink":
                    primary.rename(primary.with_suffix(".safe"))
                    primary.symlink_to(primary.with_suffix(".safe").name)
                elif kind == "malformed":
                    envelope.write_text("{")
                else:
                    envelope.write_text(json.dumps(document([contact(public(2), advert(2))])))
                time.sleep(1.05)
                self.reject()
                if kind == "permissions":
                    primary.chmod(0o600)
                elif kind == "symlink":
                    primary.unlink()
                    primary.with_suffix(".safe").rename(primary)
                else:
                    self.save("secondary", [contact(public(2), advert(2))], envelope=True)

    def test_saved_contact_does_not_bypass_message_authentication(self):
        self.save("base", [contact(public(2), advert(2))])
        self.start()
        worker = self.worker()
        descriptors = len(list((Path("/proc") / str(worker) / "fd").iterdir()))
        raw = bytearray(text(2, 5, 1700000200, "!ping"))
        raw[-1] ^= 1
        first = len(self.service.emulator.all_submissions)
        self.service.emulator.send(2, 0, bytes(raw))
        time.sleep(.7)
        self.assertFalse(any(job["port"] == 2 and parse(job["raw"])[0] in (2, 3, 8)
                             for job in self.service.emulator.all_submissions[first:]))
        self.assertIn("recovered=1", self.admin("bot contacts"))
        self.send()
        self.assertIn(b"Pong", self.reply())
        self.assertEqual(len(list((Path("/proc") / str(worker) / "fd").iterdir())), descriptors)


if __name__ == "__main__":
    unittest.main()
