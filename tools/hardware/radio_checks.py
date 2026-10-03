#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bounded operator-requested lab adverts and one native DM; never changes the PHY."""
import argparse
import hashlib
import json
import os
import socket
import struct
import time
import urllib.request

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware.admin import checked, install
from tools.hardware.mast_checks import Companion, DIRECTORY, gateway_host, mast_host, active_profile, admin_status, dashboard, require_phy, save
from tools.hardware.rf import wait_receipt
from tools.hardware.role_checks import connect
from tools.hardware.lua_checks import active
from tools.companion import sent_reply

from tools.hardware.inventory import value as inventory_value

def dm_target():
    return bytes.fromhex(inventory_value("dm_target"))


def write_record(path, value):
    temporary = path.with_suffix(".new")
    with temporary.open("w") as output:
        json.dump(value, output, indent=2)
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)


def host_status():
    with urllib.request.urlopen("http://127.0.0.1:9080/status", timeout=3) as response:
        state = json.load(response)
    shared = state["observer"]["shared_phy"]
    if not shared["valid"] or not shared["owner_connected"]:
        raise ValueError("Host PHY is not verified/connected")
    profile = shared["effective"]
    values = tuple(profile[name] for name in
                   ("frequency_hz", "bandwidth_hz", "spreading_factor", "coding_rate", "tx_power_dbm"))
    if values != active_profile():
        raise ValueError("Refusing host RF outside the isolated lab profile")
    return state


def signed_advert(raw):
    raw = bytes(raw)
    if len(raw) < 102 or raw[0] >> 6 or (raw[0] >> 2) & 15 != 4:
        return None
    start = 5 if raw[0] & 3 in (0, 3) else 1
    width, count = (raw[start] >> 6) + 1, raw[start] & 63
    if width > 3 or width * count > 64:
        return None
    payload = raw[start + 1 + width * count:]
    if not 100 <= len(payload) <= 132:
        return None
    try:
        Ed25519PublicKey.from_public_bytes(payload[:32]).verify(
            payload[36:100], payload[:36] + payload[100:])
    except InvalidSignature:
        return None
    name = None
    app = payload[100:]
    if app:
        flags = app[0]
        offset = 1 + (8 if flags & 0x10 else 0) + (2 if flags & 0x20 else 0) + (2 if flags & 0x40 else 0)
        if offset > len(app):
            return None
        if flags & 0x80:
            try:
                name = app[offset:].decode("utf-8")
            except UnicodeDecodeError:
                return None
    return {"public_key": payload[:32].hex(), "timestamp": struct.unpack_from("<I", payload, 32)[0],
            "flood": raw[0] & 3 in (0, 1), "path_bytes": width, "hops": count,
            "digest": hashlib.sha256(payload).hexdigest(), "name": name}


def wait_advert(connection, key):
    def inspect(raw, _):
        advert = signed_advert(raw)
        if advert and advert["public_key"] == key and advert["flood"]:
            return advert
        return None
    return wait_receipt(connection, None, 12, inspect, "operator-requested signed flood advert")


def preflight():
    require_phy(admin_status(), active_profile())
    peer = dashboard(gateway_host())
    profile = peer["profile"]
    if tuple(profile[key] for key in
             ("frequency_hz", "bandwidth_hz", "sf", "cr", "tx_power_dbm")) != active_profile():
        raise ValueError("Lab gateway profile changed")
    if peer["kiss"]["connected"] or peer["scheduler"]["queued"]:
        raise ValueError("Lab gateway is already in use")


def adverts():
    preflight()
    mast = {role["role"]: role for role in dashboard()["roles"]}
    hosts = host_status()
    candidates = [("esp-companion", mast_host(), 5000, mast["companion"]),
                  ("go-companion", "127.0.0.1", 5000, hosts["companion"]),
                  ("go-bot-companion", "127.0.0.1", 8106, hosts["bot_companion"])]
    evidence = {"observed": {}, "unsupported": [
        "ESP repeater/room: no exposed mast one-shot advert command; native role administrator login required",
        "Go repeater/room: no local one-shot control; native role administrator login required",
        "Gateway: select a companion-role image to request its advert",
        "Observers: receive-only; bot KISS endpoints are not command-bot radio applications"]}
    ledger_path = DIRECTORY / "operator-advert-state.json"
    ledger = json.loads(ledger_path.read_text()) if ledger_path.exists() else {}

    def record(label, value):
        ledger[label] = value
        write_record(ledger_path, ledger)

    time.sleep(5)
    try:
        with socket.create_connection((gateway_host(), 8001), timeout=5) as observer:
            for label, address, port, role in candidates:
                if label in ledger:
                    print("Not replayed, already requested: " + label, flush=True)
                    if "observed" in ledger[label]:
                        evidence["observed"][label] = ledger[label]["observed"]
                    continue
                if role.get("state") != "running":
                    evidence["unsupported"].append(label + ": not running")
                    continue
                if address == "127.0.0.1":
                    host_status()
                with Companion(address, port) as client:
                    if client.public_key != role["public_key"]:
                        raise ValueError("Native advert endpoint identity changed: " + label)
                    record(label, {"requested": True, "outcome": "unknown"})
                    if client.command(b"\x07\x01") != b"\0":
                        raise ValueError("Native flood advert not accepted: " + label)
                evidence["observed"][label] = wait_advert(observer, role["public_key"])
                record(label, {"requested": True, "observed": evidence["observed"][label]})
                print("Observed signed flood advert: " + label, flush=True)
                time.sleep(2)
            bot = mast["command-bot"]
            retry_bot = ledger.get("esp-command-bot", {}).get("rejected_before_tx", False)
            if bot.get("ready") and ("esp-command-bot" not in ledger or retry_bot):
                client = connect()
                original = checked(client, "source hash").split()[1]
                source = b"function announcement() return advert() end"
                digest = hashlib.sha256(source).hexdigest()
                attempted = False
                try:
                    if "upload=none" not in checked(client, "source status"):
                        raise ValueError("Another source upload is active")
                    attempted = True
                    install(client, source, progress=lambda _: None)
                    active(client, digest)
                    with Companion() as companion:
                        channel = companion.command(b"\x1f\x01")
                        if channel[2:34].split(b"\0", 1)[0] != (inventory_value("channel_tag")).encode("ascii"):
                            raise ValueError("Command channel does not match the operator inventory")
                        command = b"\x03\x00\x01" + struct.pack("<I", int(time.time())) + b"!announcement"
                        record("esp-command-bot", {"requested": True, "outcome": "unknown"})
                        if companion.command(command) != b"\0":
                            raise ValueError("Native announcement invocation not accepted")
                    evidence["observed"]["esp-command-bot"] = wait_advert(observer, bot["public_key"])
                    record("esp-command-bot", {"requested": True, "observed": evidence["observed"]["esp-command-bot"]})
                    print("Observed signed flood advert: esp-command-bot", flush=True)
                finally:
                    try:
                        if attempted:
                            current = checked(client, "source hash").split()[1]
                            if current == digest:
                                checked(client, "source rollback")
                                active(client, original)
                            elif current != original:
                                raise ValueError("Source changed concurrently; refusing unrelated rollback")
                            elif "upload=" + digest[:16] in checked(client, "source status"):
                                checked(client, "source cancel")
                            evidence["source_restored"] = checked(client, "source hash").split()[1] == original
                    finally:
                        client.close()
    finally:
        save("operator-adverts-" + str(int(time.time())) + ".json", evidence)
    for limitation in evidence["unsupported"]:
        print("Not triggered: " + limitation)


def operator_contact(client):
    record = client.command(b"\x1e" + dm_target(), allow_error=True)
    if record == b"\x01\x02":
        return None
    if len(record) != 148 or record[0] != 3 or record[1:33] != dm_target():
        raise ValueError("Native operator contact lookup failed or returned another identity")
    return record


def isolated_adverts():
    preflight()
    roles = {role["role"]: role for role in dashboard()["roles"]}
    evidence = {"observed": {}, "unsupported": [
        "ESP repeater/room: native role-admin advert control is not exposed through the mast backend",
        "Gateway: select a companion-role image to request its advert",
        "Real-mesh host endpoints excluded; no requests made to the host stack"]}
    previous = json.loads((DIRECTORY / "operator-advert-state.json").read_text())
    last_bot = previous.get("esp-command-bot", {}).get("observed", {}).get("timestamp")
    if last_bot and time.time() < last_bot + 900:
        evidence["unsupported"].append(
            "ESP command bot: 15-minute advert limit; next eligible timestamp " + str(last_bot + 900))
    else:
        evidence["unsupported"].append(
            "ESP command bot: no direct mast advert command; retained source was not replaced")
    name = "operator-isolated-adverts.json"
    save(name, evidence)
    try:
        role = roles["companion"]
        if role.get("state") != "running" or not role.get("ready"):
            raise ValueError("ESP companion is not running; no advert requested")
        with socket.create_connection((gateway_host(), 8001), timeout=5) as observer, Companion() as companion:
            if companion.public_key != role["public_key"]:
                raise ValueError("Isolated ESP companion identity changed")
            evidence["companion_requested"] = True
            evidence["outcome"] = "Unknown; do not replay"
            write_record(DIRECTORY / name, evidence)
            if companion.command(b"\x07\x01") != b"\0":
                raise ValueError("Native ESP flood advert was not accepted")
            evidence["native_accepted"] = True
            evidence["observed"]["esp-companion"] = wait_advert(observer, role["public_key"])
            evidence["outcome"] = "One signed ESP companion flood advert independently received"
            print(evidence["outcome"], flush=True)
    finally:
        write_record(DIRECTORY / name, evidence)
        for reason in evidence["unsupported"]:
            print("Not triggered: " + reason, flush=True)


def contact():
    require_phy(admin_status(), active_profile())
    host_status()
    for label, address, port in (("esp-companion", mast_host(), 5000),
                                 ("go-companion", "127.0.0.1", 5000),
                                 ("go-bot-companion", "127.0.0.1", 8106)):
        with Companion(address, port) as client:
            record = operator_contact(client)
            if record is None:
                print(label + ": approved operator contact not learned")
                continue
            path = record[35]
            size = (path & 63) * ((path >> 6) + 1) if path != 255 else 0
            print(json.dumps({"endpoint": label, "public_key": dm_target().hex(),
                              "last_advert": struct.unpack_from("<I", record, 132)[0],
                              "path_known": path != 255,
                              "path_bytes": (path >> 6) + 1 if path != 255 else None,
                              "path": record[36:36 + size].hex() if path != 255 else None}))


def learn_contact():
    require_phy(admin_status(), active_profile())
    with Companion() as destination:
        if operator_contact(destination) is not None:
            print("Approved operator contact already present; no import needed")
            return
        # Native export is local-only: no advert request, discovery, send or retune on this host.
        with Companion("127.0.0.1", 5000) as source:
            exported = source.command(b"\x11" + dm_target(), allow_error=True)
        advert = signed_advert(exported[1:]) if exported[:1] == b"\x0b" else None
        if not advert or advert["public_key"] != dm_target().hex():
            raise ValueError("Host cache has no verifiable approved contact; no import or RF")
        evidence = {name: advert[name] for name in ("public_key", "timestamp", "digest")}
        evidence["route"] = "Unknown; signed cached identity is not evidence of current lab presence"
        save("operator-contact-import.json", evidence)
        if destination.command(b"\x12" + exported[1:]) != b"\0":
            raise ValueError("Native signed contact import rejected; no RF")
        if operator_contact(destination) is None:
            raise ValueError("Imported operator identity failed native readback; no RF")
        print("Imported and read back approved signed contact on lab ESP; no RF sent, route still unverified")


def wait_push(client, matches, seconds):
    deadline = time.monotonic() + seconds
    for _ in range(128):
        try:
            frame = client.pushes.pop(0) if client.pushes else client.frame(deadline)
        except TimeoutError:
            return None
        if frame[0] < 128:
            raise ValueError("Unexpected command response while awaiting native push")
        if matches(frame):
            return frame
    raise ValueError("Native push observation budget exceeded")


def discovered_path(frame):
    if len(frame) < 10 or frame[:8] != b"\x8d\0" + dm_target()[:6]:
        raise ValueError("Invalid native path discovery response")
    paths = []
    offset = 8
    for _ in range(2):
        if offset >= len(frame):
            raise ValueError("Truncated native discovered path")
        encoded = frame[offset]
        size = ((encoded >> 6) + 1) * (encoded & 63)
        offset += 1
        if encoded >> 6 == 3 or size > 64 or offset + size > len(frame):
            raise ValueError("Invalid native discovered path length")
        paths.append((encoded, frame[offset:offset + size]))
        offset += size
    if offset != len(frame):
        raise ValueError("Trailing native discovered path data")
    return paths[0]


def admit_dm_attempt(name, evidence, retry_discovery=False):
    if not retry_discovery:
        save(name, evidence)
        return
    path = DIRECTORY / name
    previous = json.loads(path.read_text())
    if (previous.get("dm_requested") is not False or
            previous.get("outcome") != "No authenticated discovery response; no DM sent" or
            previous.get("sender") != evidence["sender"] or previous.get("target") != dm_target().hex()):
        raise ValueError("Cannot retry discovery after a possible DM or unverified earlier outcome")
    save(path.stem + "-history-" + str(time.time_ns()) + ".json", previous)
    write_record(path, evidence)


def guard_operator_dm():
    for name in ("operator-dm.json", "operator-isolated-dm.json", "operator-dm-external.json"):
        path = DIRECTORY / name
        if path.exists():
            previous = json.loads(path.read_text())
            if previous.get("dm_requested") is not False:
                raise ValueError("Operator DM already requested or outcome uncertain; no duplicate DM")


def dm(isolated=False, retry_discovery=False):
    guard_operator_dm()
    if isolated:
        require_phy(admin_status(), active_profile())
        role = next(role for role in dashboard()["roles"] if role["role"] == "companion")
        if role.get("state") != "running" or not role.get("ready"):
            raise ValueError("Isolated ESP companion is not running")
        expected_key = role["public_key"]
    else:
        expected_key = host_status()["companion"]["public_key"]

    def validate_phy():
        if isolated:
            require_phy(admin_status(), active_profile())
        else:
            host_status()

    with Companion(mast_host() if isolated else "127.0.0.1", 5000) as client:
        if client.public_key != expected_key:
            raise ValueError("Host companion identity changed")
        record = operator_contact(client)
        if record is None:
            raise ValueError("Approved operator contact not learned; no DM sent")
        exported = client.command(b"\x11" + dm_target())
        advert = signed_advert(exported[1:]) if exported[:1] == b"\x0b" else None
        if not advert or advert["public_key"] != dm_target().hex():
            raise ValueError("Operator cached advert signature could not be verified; no DM sent")
        evidence = {"sender": expected_key, "target": dm_target().hex(), "advert": advert,
                    "discovery_requested": True, "dm_requested": False,
                    "outcome": "Route discovery outcome unknown; do not replay automatically"}
        name = "operator-isolated-dm.json" if isolated else "operator-dm.json"
        admit_dm_attempt(name, evidence, retry_discovery)
        try:
            time.sleep(5)
            validate_phy()
            discovery = sent_reply(client.command(b"\x34\0" + dm_target()))
            evidence["discovery_admission"] = discovery
            route = wait_push(client, lambda frame: frame[:8] == b"\x8d\0" + dm_target()[:6], 25)
            if route is None:
                evidence["outcome"] = "No authenticated discovery response; no DM sent"
                raise ValueError(evidence["outcome"])
            encoded, path = discovered_path(route)
            evidence["route"] = {"encoded": encoded, "path": path.hex()}
            current = operator_contact(client)
            if current is None:
                raise ValueError("Operator contact disappeared after discovery; no DM sent")
            update = bytearray(current[:144])
            update[0], update[35] = 9, encoded
            update[36:100] = path.ljust(64, b"\0")
            if client.command(bytes(update)) != b"\0":
                raise ValueError("Native discovered-route update rejected; no DM sent")
            readback = operator_contact(client)
            if readback is None or readback[35:100] != update[35:100]:
                raise ValueError("Native discovered-route readback mismatch; no DM sent")
            validate_phy()
            evidence["dm_requested"] = True
            evidence["outcome"] = "DM outcome unknown; do not replay"
            write_record(DIRECTORY / name, evidence)
            text = b"Hello from the MeshCore lab on 912.525 MHz. This is the invited test DM."
            frame = b"\x02\0\0" + struct.pack("<I", int(time.time())) + dm_target()[:6] + text
            sent = sent_reply(client.command(frame))
            evidence["dm_admission"] = sent
            ack = wait_push(client, lambda item: len(item) == 9 and item[:5] ==
                            b"\x82" + bytes.fromhex(sent["tag"]), 30)
            evidence["acknowledged"] = ack is not None
            if ack is None:
                evidence["outcome"] = "One DM admitted; no matching ACK observed; delivery unknown; do not replay"
            else:
                evidence["ack_rtt_ms"] = struct.unpack_from("<I", ack, 5)[0]
                evidence["outcome"] = "One ordinary DM sent with matching native ACK"
            print(evidence["outcome"], flush=True)
        finally:
            write_record(DIRECTORY / name, evidence)


def recover_companion_observation():
    key = next(role["public_key"] for role in dashboard()["roles"] if role["role"] == "companion")
    host_status()
    with Companion("127.0.0.1", 5000) as client:
        packet = client.command(b"\x11" + bytes.fromhex(key))
    if not packet or packet[0] != 11:
        raise ValueError("Host did not export a cached signed advert")
    advert = signed_advert(packet[1:])
    requested_at = (DIRECTORY.parent / "onchip-operator-adverts.log").stat().st_mtime
    if (not advert or not advert["flood"] or advert["public_key"] != key or
            abs(advert["timestamp"] - requested_at) > 5):
        raise ValueError("Cached advert cannot corroborate the original one-shot request")
    # Go exports normalize route metadata; only the signed payload corroborates reception.
    advert = {name: advert[name] for name in ("public_key", "timestamp", "digest")}
    path = DIRECTORY / "operator-advert-state.json"
    ledger = json.loads(path.read_text())
    ledger["esp-companion"]["observed"] = advert
    ledger["esp-companion"]["confirmation"] = "Signed RF advert cached by separate Go-host radio"
    ledger["esp-companion"]["outcome"] = "Flood request admitted; signed reception corroborated; original RF route metadata unavailable"
    write_record(path, ledger)
    print("Corroborated ESP companion signed advert reception; exported cache does not preserve RF route metadata; no retransmit")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("adverts", "contact", "recover-companion", "dm",
                                          "isolated-adverts", "isolated-dm", "learn-contact"))
    parser.add_argument("--retry-discovery", action="store_true",
                        help="explicitly renew only a recorded failed discovery with no DM request")
    args = parser.parse_args()
    if args.retry_discovery and args.action not in ("dm", "isolated-dm"):
        parser.error("--retry-discovery is valid only with a DM action")
    os.umask(0o077)
    DIRECTORY.mkdir(mode=0o700, exist_ok=True)
    {"adverts": adverts, "contact": contact, "recover-companion": recover_companion_observation,
     "dm": lambda: dm(retry_discovery=args.retry_discovery), "isolated-adverts": isolated_adverts,
     "isolated-dm": lambda: dm(isolated=True, retry_discovery=args.retry_discovery),
     "learn-contact": learn_contact}[args.action]()


if __name__ == "__main__":
    main()
