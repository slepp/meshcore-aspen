#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Apply an owner-generated key manifest through existing local native APIs.

Cedar/Pine use local native USB; Aspen uses encrypted Management RF.
Aspen-Base has a separate final operation for the chat sender transition.
Private keys and channel secrets never enter argv or logs.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import hmac
import json
import os
import re
import socket
import struct
import time

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware import mast_checks as field
from tools.hardware.admin import checked, private_file
from tools.hardware.radio_checks import host_status
from tools.hardware.rf import public_hex
from tools.hardware import companion_cli as stock
from tools.hardware import companion_device as stock_lab

sys.path.insert(0, str(field.ROOT / "firmware/nrf52840"))
import hardware as nrf

from tools.hardware.inventory import value as inventory_value

def nrf_port():
    return inventory_value("nrf_port")
def peer_roles():
    return {key: tuple(row) for key, row in inventory_value("peer_roles").items()}
def mast_roles():
    return {key: tuple(row) for key, row in inventory_value("mast_roles").items()}
PUBLIC_KEY = re.compile("[0-9a-f]{64}")


def load_manifest(path, targets=None):
    if targets is None:
        targets = peer_roles()
    path = Path(path).absolute()
    if path.parent.stat().st_mode & 0o077:
        raise ValueError("Replacement-key directory must be private (0700)")
    document = json.loads(private_file(path, 65536))
    if document.get("format") != "meshcore-fleet-keys-v1" or document.get("path_hash_bytes") != 3:
        raise ValueError("Expected the owner-generated three-byte fleet key manifest")
    records, prefixes = {}, set()
    for entry in document["identities"]:
        identifier = entry["id"]
        old, new = entry["previous_public_key"], entry["public_key"]
        if (identifier in records or not isinstance(old, str) or not isinstance(new, str) or
                not PUBLIC_KEY.fullmatch(old) or not PUBLIC_KEY.fullmatch(new) or old == new or
                not 0xb7 <= int(new[:2], 16) <= 0xbb or entry["prefix_3_bytes"] != new[:6] or
                new[:6] in prefixes):
            raise ValueError("Invalid or colliding public replacement manifest")
        records[identifier] = entry
        prefixes.add(new[:6])
    material = {}
    for identifier, (name, role) in targets.items():
        entry = records[identifier]
        if entry["name"] != name or entry["role"] != role or entry.get("advertised_role") is not True:
            raise ValueError("Manifest peer role/name does not match its reserved device")
        for key, suffix in (("seed_file", ".seed"), ("native_key_file", ".key")):
            filename = entry[key]
            if not isinstance(filename, str) or Path(filename).name != filename or not filename.endswith(suffix):
                raise ValueError("Manifest secret filenames must be local basenames")
        seed = private_file(path.parent / entry["seed_file"], 32)
        native = private_file(path.parent / entry["native_key_file"], 64)
        if (len(seed) != 32 or len(native) != 64 or public_hex(seed) != entry["public_key"] or
                not hmac.compare_digest(stock.native_private_key(seed), native)):
            raise ValueError("Parent key material does not match its native/public manifest")
        material[identifier] = native
    return records, material


def check_current(entry, public_key):
    if public_key.lower() not in (entry["previous_public_key"], entry["public_key"]):
        raise ValueError("Live identity is neither the authorized old nor replacement key: " + entry["id"])


def stock_snapshot(label):
    device = {}
    info = field.fleet_stock_info(label, device)
    count = device.get("max_channels")
    if type(count) is not int or not 1 <= count <= 40:
        raise ValueError("Stock native channel capacity unavailable")
    if tuple(info.get(key) for key in ("radio_freq", "radio_bw", "radio_sf", "radio_cr", "tx_power")) != (
            field.active_profile()[0] / 1e6, field.active_profile()[1] / 1e3, *field.active_profile()[2:]):
        raise ValueError("Cedar is outside the held lab profile")
    channels = stock.run_cli(label + "-channels", [f"get_channel {index}" for index in range(count)],
                             timeout=40, channel_info=True, private_script=True)
    if len(channels) != count or [row["channel_idx"] for row in channels] != list(range(count)):
        raise ValueError("Stock channel readback omitted a slot")
    return {"info": info, "path_hash_mode": device["path_hash_mode"], "channels": [
        {"slot": row["channel_idx"], "name": row["channel_name"],
         "key_sha256": hashlib.sha256(bytes.fromhex(row["channel_secret"])).hexdigest()}
        for row in channels]}


def verify_stock_retained(before, after, public_key):
    expected = dict(before["info"], public_key=public_key)
    if (after["info"] != expected or after["channels"] != before["channels"] or
            after["path_hash_mode"] != before["path_hash_mode"]):
        raise ValueError("Cedar import/restart changed settings or did not retain the replacement identity")


def pine_public(link, command):
    reply = nrf.serial_command(link, command).strip().lower()
    match = re.fullmatch(r"active=([0-9a-f]{64}) saved=([0-9a-f]{64}) reboot=([01])", reply)
    if not match:
        raise ValueError("Pine public identity readback unavailable")
    return match[1], match[2], int(match[3])


def stage_pine_bot(link, native, public_key):
    reply = nrf.serial_command(link, "set bot.prv.key " + native.hex(), duration=1.5).strip()
    if reply.lower() != "ok saved; reboot required; public-key=" + public_key:
        raise ValueError("Pine private import was not acknowledged; response withheld")


def stage_aspen_key(client, role, native, public_key):
    canonical = "bot" if role == "command-bot" else role
    try:
        reply = client.command("key " + canonical + " " + native.hex())
    except (OSError, ValueError):
        raise ValueError("Encrypted identity import failed for " + role +
                         "; inspect its pending public key before retrying; response withheld") from None
    accepted = ("Pending " + public_key + "; reboot required; peers must learn new key",
                "KEY " + public_key + "; already active; no reboot required")
    if reply not in accepted:
        raise ValueError("Identity import not acknowledged for " + role +
                         "; inspect its pending public key before retrying; response withheld")


def aspen_admin(public_key):
    return field.NativeClient(inventory_value("receiver_host"), 8001, field.LAB / "companion.seed", public_key,
                              private_file(field.LAB / "password", 15).decode("ascii"))


def advertise_aspen(client, role):
    busy = "Error: advert unavailable; role inactive, radio/queue busy or rate/airtime limit"
    for attempt in range(10):
        response = client.command("role advert " + role + " zerohop")
        if response == "Queued zero-hop advert; delivery/peer learning unconfirmed":
            return
        if response != busy or attempt == 9:
            raise ValueError(role + " zero-hop advert: " + response)
        if attempt == 0:
            print(role + " advert waiting for its radio queue", flush=True)
        time.sleep(1)


def apply_aspen(manifest, base_only=False):
    targets = {identifier: value for identifier, value in mast_roles().items()
               if (identifier == "aspen-base") == base_only}
    records, material = load_manifest(manifest, targets)
    before = {row["role"]: row for row in field.dashboard()["roles"]}
    admin_before = field.admin_status()
    field.require_phy(admin_before, field.active_profile())
    for identifier, (name, role) in mast_roles().items():
        check_current(records[identifier], before[role]["public_key"])
        if before[role]["name"] != name or not before[role]["ready"]:
            raise ValueError("Aspen role name/readiness differs: " + role)
        if base_only and identifier != "aspen-base" and before[role]["public_key"] != records[identifier]["public_key"]:
            raise ValueError("Apply the other Aspen replacements before switching Aspen-Base")
    label = "aspen-rekey-" + str(time.time_ns())
    record = nrf.private_directory(field.DIRECTORY / label)
    evidence = {"roles": {}, "adverts": {}, "complete": False, "base_only": base_only}
    expected = {role: row.get("public_key") for role, row in before.items()}
    client = aspen_admin(expected["management"])
    try:
        changed = False
        for identifier, (name, role) in targets.items():
            entry = records[identifier]
            target = entry["public_key"]
            canonical = "bot" if role == "command-bot" else role
            if checked(client, "key " + canonical) != "KEY " + expected[role]:
                raise ValueError("Authenticated active identity differs: " + role)
            if expected[role] != target:
                stage_aspen_key(client, role, material[identifier], target)
                if checked(client, "key " + canonical + " pending") != (
                        "Pending " + target + "; reboot required; peers must learn new key"):
                    raise ValueError("Staged public identity differs: " + role)
                changed = True
            expected[role] = target
            evidence["roles"][identifier] = {"name": name, "public_key": target, "state": "staged"}
            print(name + " selected public identity: " + target, flush=True)
        if changed:
            checked(client, "reboot")
    finally:
        client.close()
        nrf.save(record, "staged.json", evidence)
    if changed:
        time.sleep(12)
    after = {row["role"]: row for row in field.dashboard()["roles"]}
    if {role: row.get("public_key") for role, row in after.items()} != expected:
        raise ValueError("Aspen did not activate exactly the selected replacement identities")
    for _, (name, role) in mast_roles().items():
        if after[role]["name"] != name or not after[role]["ready"]:
            raise ValueError("Aspen role did not retain its name/readiness: " + role)
    admin_after = field.admin_status()
    field.require_phy(admin_after, field.active_profile())
    for key in ("source hash", "bot policy", "room access", "role-path"):
        if admin_after[key] != admin_before[key]:
            raise ValueError("Aspen rekey changed retained configuration: " + key)
    with field.Companion() as companion:
        if companion.public_key != expected["companion"] or companion.width != 3:
            raise ValueError("Companion identity/path readback differs after rekey")
        if companion.command(b"\x06" + struct.pack("<I", int(time.time()))) != b"\0":
            raise ValueError("Native clock synchronization rejected before advertisements")
    client = aspen_admin(expected["management"])
    try:
        for identifier, (_, role) in targets.items():
            canonical = "bot" if role == "command-bot" else role
            if checked(client, "key " + canonical) != "KEY " + expected[role]:
                raise ValueError("Authenticated post-reboot identity differs: " + role)
            evidence["roles"][identifier]["state"] = "applied_and_persistent"
        wanted = {records[identifier]["public_key"]: name for identifier, (name, _) in targets.items()}
        with socket.create_connection((inventory_value("receiver_host"), 8001), timeout=5) as receiver, \
                ThreadPoolExecutor(max_workers=1) as executor:
            received = executor.submit(field.wait_named_adverts, receiver, wanted, int(time.time()) - 2,
                                       90, evidence["adverts"])
            for _, (_, role) in targets.items():
                canonical = "bot" if role == "command-bot" else role
                advertise_aspen(client, canonical)
            received.result()
        evidence["complete"] = True
    finally:
        client.close()
        nrf.save(record, "result.json", evidence)
    print("PASS selected Aspen keys active after reboot, names/settings retained, encrypted management "
          "reconnected and signed zero-hop adverts independently received; " + label, flush=True)


def apply_peers(manifest):
    records, material = load_manifest(manifest)
    label = "peer-rekey-" + str(time.time_ns())
    record = nrf.private_directory(field.DIRECTORY / label)
    evidence = {"manifest": str(Path(manifest).absolute()), "roles": {}, "adverts": {}, "complete": False}
    aspen_before = {row["role"]: row.get("public_key") for row in field.dashboard()["roles"]}
    field.require_phy(field.admin_status(), field.active_profile())
    go_before = {role: row["public_key"] for role, row in host_status().items() if "public_key" in row}
    cedar = stock_snapshot(label + "-cedar-before")
    check_current(records["cedar-base"], cedar["info"]["public_key"])
    if cedar["info"]["name"] != "Cedar-Base":
        raise ValueError("Cedar's saved tree name is missing")
    pine, _, identities = nrf.snapshot(nrf_port(), record, "pine-before.json", True)
    radio = tuple(map(float, pine["get radio"].strip().removeprefix("> ").split(",")))
    if (len(radio) != 4 or abs(radio[0] - field.active_profile()[0] / 1e6) > .0001 or
            radio[1:] != (250, 7, 5) or pine["get tx"].strip() != "> 2" or
            pine["get path.hash.mode"].strip() != "> 2" or pine["get name"].strip() != "> Pine-Relay"):
        raise ValueError("Pine name/PHY/path settings do not match the held lab profile")
    check_current(records["pine-bot"], identities[1])
    with nrf.connection(nrf_port()) as link:
        repeater = pine_public(link, "get pub.key")
        bot = pine_public(link, "get bot.pub.key")
        if repeater != (identities[0].lower(), identities[0].lower(), 0):
            raise ValueError("Unrelated Pine repeater identity has a pending change")
        if nrf.serial_command(link, "get bot.name").strip() != "> Pine-Bot":
            raise ValueError("Pine's saved bot name is missing")
        check_current(records["pine-bot"], bot[1])
        if bot[0] != identities[1].lower() or (bot[0] == records["pine-bot"]["public_key"] and bot[2]):
            raise ValueError("Pine active/pending identity changed unexpectedly")
    try:
        target = records["pine-bot"]["public_key"]
        if bot[0] != target:
            with nrf.connection(nrf_port()) as link:
                if bot[1] != target:
                    stage_pine_bot(link, material["pine-bot"], target)
                if pine_public(link, "get bot.pub.key") != (bot[0], target, 1):
                    raise ValueError("Pine staged public key does not match parent manifest")
            evidence["roles"]["pine-bot"] = {"public_key": target, "state": "staged"}
            nrf.reboot(nrf_port())
        after_pine, _, after_ids = nrf.snapshot(nrf_port(), record, "pine-after.json", True)
        if (after_ids[0] != identities[0] or after_ids[1].lower() != target or
                any(after_pine[key] != pine[key] for key in
                    ("get name", "get radio", "get tx", "get repeat", "get path.hash.mode"))):
            raise ValueError("Pine changed an unrelated key/setting or did not activate the replacement")
        with nrf.connection(nrf_port()) as link:
            if (pine_public(link, "get bot.pub.key") != (target, target, 0) or
                    nrf.serial_command(link, "get bot.name").strip() != "> Pine-Bot"):
                raise ValueError("Pine replacement/name persistence readback failed")
            field.sync_pine_clock(nrf, link)
        evidence["roles"]["pine-bot"] = {"public_key": target, "state": "applied_and_persistent",
                                       "name": "Pine-Bot", "repeater_unchanged": True}
        print("Pine-Bot replacement applied and persisted: " + target, flush=True)

        target = records["cedar-base"]["public_key"]
        if cedar["info"]["public_key"] != target:
            stock.run_cli(label + "-cedar-import", [
                "set private_key " + material["cedar-base"].hex(), "infos"],
                timeout=30, private_script=True)
            evidence["roles"]["cedar-base"] = {"public_key": target, "state": "import_acknowledged"}
        stock_lab.check()
        time.sleep(3)
        cedar_after = stock_snapshot(label + "-cedar-after")
        verify_stock_retained(cedar, cedar_after, target)
        evidence["roles"]["cedar-base"] = {"public_key": target, "state": "applied_and_persistent",
                                         "name": "Cedar-Base", "channels_unchanged": True}
        print("Cedar-Base replacement applied and persisted: " + target, flush=True)
        wanted = {records[identifier]["public_key"]: name for identifier, (name, _) in peer_roles().items()}
        since = int(time.time()) - 2
        with socket.create_connection((inventory_value("receiver_host"), 8001), timeout=5) as receiver, \
                ThreadPoolExecutor(max_workers=1) as executor:
            received = executor.submit(field.wait_named_adverts, receiver, wanted, since, 25,
                                       evidence["adverts"])
            stock.run_cli(label + "-cedar-advert", ["clock sync", "ver", "advert"], timeout=20)
            time.sleep(2)
            with nrf.connection(nrf_port()) as link:
                if not nrf.serial_command(link, "bot advert.zerohop", duration=2).startswith("Queued"):
                    raise ValueError("Pine replacement zero-hop advert not queued")
            received.result()
        if {row["role"]: row.get("public_key") for row in field.dashboard()["roles"]} != aspen_before:
            raise ValueError("An Aspen identity changed during the peer-only operation")
        if {role: row["public_key"] for role, row in host_status().items() if "public_key" in row} != go_before:
            raise ValueError("An unrelated Go identity changed during the peer-only operation")
        field.require_phy(field.admin_status(), field.active_profile())
        evidence["complete"] = True
    finally:
        nrf.save(record, "result.json", evidence)
    print("PASS parent-selected Cedar/Pine keys, persisted names/settings and independent signed zero-hop "
          "receipts; Aspen (including Base) and Go unchanged; " + label, flush=True)


if __name__ == "__main__":
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--target", choices=("peers", "aspen", "aspen-base"), default="peers")
    args = parser.parse_args()
    if args.target == "peers":
        apply_peers(args.manifest)
    else:
        apply_aspen(args.manifest, base_only=args.target == "aspen-base")
