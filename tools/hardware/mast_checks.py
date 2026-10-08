#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Finite Make-driven checks and deployment of the explicitly authorized beta mast.

rf-management-check temporarily disconnects only the mast's WiFi, exercises its
existing encrypted management-role RF interface, and restores the saved SSID
over RF. Set MESHCORE_ENV_FILE to the authorized .env.dev.local; no credentials
are logged, native role authority is unchanged, and no radio retune is required.

aspen-names applies Aspen's saved/live labels and receives its five signed
zero-hop adverts on Birch's MeshCore radio. It does not restart either device
or change Go configuration; pending identity changes remain staged.
aspen-names-persist also reboots Aspen to check persistence; use it only before
staging replacement keys, since a device reboot applies pending identities.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import hmac
import json
import os
import shlex
import socket
import struct
import subprocess
import time
import urllib.request

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware.esp32_device import DIRECTORY as LAB, device_mac, device_port, ROOT, check
from tools.hardware.admin import NativeClient, checked, encrypt, private_file
from tools.hardware.rf import kiss, wait_receipt
from tools.hardware.role_checks import connect, select
from tools.hardware.companion_cli import documents

from tools.hardware.inventory import value as inventory_value
from tools.companion import Companion as TcpCompanion

DIRECTORY = ROOT / ".tmp/onchip-owner-field"
def mast_host():
    return inventory_value("mast_host")
def gateway_host():
    return inventory_value("gateway_host")
def channel_tag():
    return inventory_value("channel_tag")
def active_profile():
    return tuple(inventory_value("active_profile"))
# Historical commissioning profile; active fleet operations use LAB_PHY.
def commissioning_profile():
    return tuple(inventory_value("commissioning_profile"))


def save(name, value):
    path = DIRECTORY / name
    if path.exists():
        raise ValueError("Refusing to replace field evidence: " + name)
    data = value if isinstance(value, bytes) else (json.dumps(value, indent=2) + "\n").encode()
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "wb") as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())


def dashboard(host=None):
    if host is None:
        host = mast_host()
    with urllib.request.urlopen(f"http://{host}/api/status", timeout=5) as response:
        return json.load(response)


def admin_status():
    client = connect()
    try:
        return {name: checked(client, name) for name in
                ("status", "wifi status", "bot status", "bot policy", "role-path", "room access", "source hash", "job")}
    finally:
        client.close()


def require_phy(actual, expected):
    wanted = "PHY=" + ",".join(str(value) for value in expected)
    if wanted not in actual["status"] or "temp=0" not in actual["status"]:
        raise ValueError("Unexpected mast PHY or temporary retune: " + actual["status"])


class Companion(TcpCompanion):
    def __init__(self, host=None, port=5000):
        if host is None:
            host = mast_host()
        super().__init__(host, port)


def configure_channel():
    key = hashlib.sha256(channel_tag().encode()).digest()[:16]
    with Companion() as client:
        if client.width != 3:
            raise ValueError("Companion does not report three-byte path mode")
        free = None
        for index in range(client.channels):
            record = client.command(bytes((31, index)))
            if len(record) != 50 or record[:2] != bytes((18, index)):
                raise ValueError("Malformed native channel readback")
            name = record[2:34].split(b"\0", 1)[0].decode("utf-8")
            if name == channel_tag():
                if record[34:] != key:
                    raise ValueError("Existing tag channel uses a different key; refusing replacement")
                return index
            if index and not name and free is None:
                free = index
        if free is None:
            raise ValueError("No unused native companion channel slot")
        frame = bytes((32, free)) + channel_tag().encode().ljust(32, b"\0") + key
        if client.command(frame) != b"\0":
            raise ValueError("Native channel write not accepted")
        readback = client.command(bytes((31, free)))
        if (len(readback) != 50 or readback[:2] != bytes((18, free)) or
                readback[2:34].split(b"\0", 1)[0] != channel_tag().encode() or readback[34:] != key):
            raise ValueError("Native channel write/readback mismatch")
        return free


def cli(label, lines, timeout=60):
    script = DIRECTORY / (label + ".meshcli")
    save(script.name, ("\n".join(lines) + "\n").encode())
    home = DIRECTORY / "cli-home"
    home.mkdir(mode=0o700, exist_ok=True)
    env = os.environ.copy()
    env["HOME"] = str(home)
    result = subprocess.run(["meshcli", "-j", "-t", mast_host(), "-p", "5000", "script", str(script)],
                            env=env, capture_output=True, text=True, timeout=timeout)
    save(label + ".stdout", result.stdout.encode())
    save(label + ".stderr", result.stderr.encode())
    if result.returncode:
        raise ValueError("Stock TCP meshcli failed; inspect private " + label + " logs")
    try:
        return documents(result.stdout)
    except json.JSONDecodeError as error:
        raise ValueError("Stock CLI reported non-JSON failure; inspect private " + label + " logs") from error


def prepare():
    current = admin_status()
    require_phy(current, active_profile())
    save("before.json", {"admin": current, "dashboard": dashboard(), "mac": device_mac()})
    client = connect()
    try:
        for command in ("bot channel " + channel_tag(), "bot path 3", "bot airtime 1200", "bot on"):
            checked(client, command)
    finally:
        client.close()
    select(7)
    client = connect()
    try:
        checked(client, "role-path 3")
    finally:
        client.close()
    index = configure_channel()
    save("channel.json", {"name": channel_tag(), "index": index})
    print("Applied isolated roles 7 and native hashtag policy; companion path mode 3 and channel read back", flush=True)


def group_exchange(command, expected, repeater=None, path_width=3):
    if path_width not in (1, 2, 3):
        raise ValueError("Native group path width must be 1..3")
    key = hashlib.sha256(channel_tag().encode()).digest()[:16]
    secret = key + bytes(16)
    channel_hash = hashlib.sha256(key).digest()[:1]
    timestamp = int(time.time())
    text = ("owner-field: " + command).encode()
    payload = channel_hash + encrypt(secret, struct.pack("<I", timestamp) + b"\0" + text)
    packet = bytes((0x15, (path_width - 1) << 6)) + payload
    reply = None
    forwarded = False
    with socket.create_connection((gateway_host(), 8001), timeout=5) as connection:
        connection.sendall(kiss(packet))

        def observe(raw, _):
            nonlocal reply, forwarded
            if len(raw) < 5 or raw[0] >> 6 or (raw[0] >> 2) & 15 != 5:
                return None
            start = 5 if raw[0] & 3 in (0, 3) else 1
            width, count = (raw[start] >> 6) + 1, raw[start] & 63
            if width > 3 or width * count > 64:
                return None
            path = raw[start + 1:start + 1 + width * count]
            body = raw[start + 1 + width * count:]
            if body == payload and width == 3 and repeater:
                forwarded = forwarded or bytes.fromhex(repeater)[:3] in [
                    path[i:i + 3] for i in range(0, len(path), 3)]
            if len(body) >= 19 and body[:1] == channel_hash and (len(body) - 3) % 16 == 0:
                mac = hmac.new(secret, body[3:], hashlib.sha256).digest()[:2]
                if hmac.compare_digest(body[1:3], mac):
                    cipher = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
                    plain = cipher.update(body[3:]) + cipher.finalize()
                    message = plain[5:].split(b"\0", 1)[0].decode("ascii")
                    if message.startswith("MeshCore command bot: "):
                        if expected not in message or width != 3:
                            raise ValueError("Unexpected physical channel reply: " + message)
                        reply = message
            if reply and (forwarded or repeater is None):
                return {"reply": reply, "repeater_forwarded": forwarded if repeater else None}
            return None

        try:
            return wait_receipt(connection, None, 15, observe, "owner field group exchange")
        except (OSError, ValueError) as error:
            raise ValueError(f"Group observation incomplete: reply={reply!r}, "
                             f"required-forward={forwarded if repeater else None}; {error}") from error


def lab_check():
    current = admin_status()
    require_phy(current, active_profile())
    if "roles applied=7 saved=7" not in current["status"]:
        raise ValueError("All three native roles must be selected")
    state = dashboard()
    roles = {role["role"]: role for role in state["roles"]}
    names = ("repeater", "room", "companion", "management", "command-bot")
    if any(not roles[name]["ready"] for name in names):
        raise ValueError("A required native role is not ready")
    if len({roles[name]["public_key"] for name in names}) != len(names):
        raise ValueError("Native role identities are not independent")
    gateway = dashboard(gateway_host())
    if gateway["profile"]["frequency_hz"] != active_profile()[0] or gateway["kiss"]["connected"]:
        raise ValueError("Gateway is not idle on the isolated lab profile")
    with Companion() as first, Companion() as second:
        if first.width != 3 or second.public_key != roles["companion"]["public_key"]:
            raise ValueError("Concurrent native TCP identity/path mismatch")
    ping = group_exchange("!ping", "Pong", roles["repeater"]["public_key"])
    print("PASS physical hashtag Pong and native three-byte repeater forwarding", flush=True)
    time.sleep(35)
    signal = group_exchange("!test", "RSSI=")
    received = cli("lab-tcp-receive-" + str(int(time.time())), ["ver", "infos", "sync_msgs"])
    messages = [entry.get("text", "") for entry in received]
    if not any("MeshCore command bot: Pong" in text for text in messages):
        raise ValueError("Stock protocol13 TCP client did not receive native hashtag Pong")
    save("lab-network-evidence.json",
         {"admin": current, "roles": {name: roles[name]["public_key"] for name in names},
          "ping": ping, "signal": signal, "stock_tcp_receive": True, "companion_clients": 2})
    if (DIRECTORY / "room-evidence.json").is_file():
        finish_evidence()
    elif "password-protected" in current["room access"]:
        raise ValueError("Network evidence saved; protected-room guest login remains unverified. "
                         "Existing room credentials were not changed")
    else:
        room_check()


def room_check():
    state = dashboard()
    role = next(role for role in state["roles"] if role["role"] == "room")
    name = shlex.quote(role["name"])
    label = "lab-room-" + str(int(time.time()))
    room = cli(label, [
        f"add_contact {role['public_key']} 3 {name}",
        "sync_msgs", f'login {name} ""', "sleep 2",
        f"req_status {name}", "sleep 2", "sync_msgs", f"logout {name}"])
    if not any(entry.get("login_success") is True for entry in room):
        raise ValueError("Native room guest login did not succeed")
    save("room-evidence.json", {"room": role["public_key"], "guest_login": True, "log": label})
    if (DIRECTORY / "lab-network-evidence.json").is_file():
        finish_evidence()
    else:
        print("PASS native room guest login; separate network evidence remains to be finalized", flush=True)


def room_admin_check():
    current = admin_status()
    require_phy(current, active_profile())
    state = dashboard()
    role = next(role for role in state["roles"] if role["role"] == "room")
    if not role["ready"]:
        raise ValueError("Native room is not ready")
    gateway = dashboard(gateway_host())
    if gateway["profile"]["frequency_hz"] != active_profile()[0] or gateway["kiss"]["connected"]:
        raise ValueError("Gateway is not idle on the isolated lab profile")
    try:
        client = NativeClient(gateway_host(), 8001, LAB / "companion.seed", role["public_key"],
                              private_file(LAB / "role-password", 15).decode("ascii"), room=True)
    except TimeoutError:
        raise ValueError("No authenticated room reply; access remains unverified and guest policy unchanged") from None
    client.close()
    save("room-admin-evidence.json", {"room": role["public_key"], "admin_login": True,
                                     "independent_rf": True, "guest_policy": current["room access"]})
    print("PASS independent native room administrator RF login; existing guest policy unchanged", flush=True)


def finish_evidence():
    evidence = json.loads(private_file(DIRECTORY / "lab-network-evidence.json", 65536))
    room = json.loads(private_file(DIRECTORY / "room-evidence.json", 65536))
    if room["room"] != evidence["roles"]["room"] or not room["guest_login"]:
        raise ValueError("Room and network evidence refer to different identities")
    evidence["room_login"] = True
    save("lab-evidence.json", evidence)
    print("PASS actual radio diagnostics, stock TCP channel receive, native room guest login and two TCP clients", flush=True)


def backup():
    current = admin_status()
    require_phy(current, active_profile())
    image = DIRECTORY / "pre-real.bin"
    if image.exists() or (DIRECTORY / "backup.log").exists():
        raise ValueError("Pre-real backup already exists; refusing overwrite")
    check()
    with (DIRECTORY / "backup.log").open("x") as output:
        subprocess.run(["make", "-s", "-C", str(ROOT), "firmware-backup",
                        f"UPLOAD_PORT={device_port()}", f"FIRMWARE_BACKUP={image}", "FLASH_SIZE=0x800000"],
                       stdout=output, stderr=subprocess.STDOUT, check=True)
    if image.stat().st_size != 0x800000:
        raise ValueError("Incomplete pre-real 8 MiB backup")
    save("pre-real.json", {"sha256": hashlib.sha256(image.read_bytes()).hexdigest(), "mac": device_mac(),
                           "admin": current})
    time.sleep(10)
    require_phy(admin_status(), active_profile())
    print("Saved and hashed full 8 MiB beta mast backup; lab PHY returned after serial reset", flush=True)


def deployment_evidence(allow_protected_room, current, state):
    complete = DIRECTORY / "lab-evidence.json"
    path = complete if complete.is_file() else DIRECTORY / "lab-network-evidence.json"
    if not complete.is_file() and (not allow_protected_room or
                                  current["room access"] != "Room guest access password-protected"):
        raise ValueError("Complete the short isolated mixed-role check before real deployment")
    evidence = json.loads(private_file(path, 65536))
    live = {role["role"]: role for role in state["roles"]}
    names = ("repeater", "room", "companion", "management", "command-bot")
    if (set(evidence["roles"]) != set(names) or
            any(not live[name]["ready"] or evidence["roles"][name] != live[name]["public_key"]
                for name in names) or
            evidence["admin"]["source hash"].split()[1] != current["source hash"].split()[1] or
            evidence["ping"]["repeater_forwarded"] is not True or
            "Pong" not in evidence["ping"]["reply"] or
            "RSSI=" not in evidence["signal"]["reply"] or
            evidence["stock_tcp_receive"] is not True or evidence["companion_clients"] != 2 or
            (complete.is_file() and evidence.get("room_login") is not True)):
        raise ValueError("Mixed-role evidence does not match the current mast")
    return {"lab_network": True, "room_guest_login": complete.is_file(),
            "mobile_app_send_receive": False, "real_phy_stock_peer": False}


def deploy(allow_protected_room=False):
    saved = json.loads(private_file(DIRECTORY / "pre-real.json", 65536))
    image = DIRECTORY / "pre-real.bin"
    if saved["mac"] != device_mac() or image.stat().st_size != 0x800000 or \
            hashlib.sha256(image.read_bytes()).hexdigest() != saved["sha256"]:
        raise ValueError("Pre-real backup identity/hash mismatch")
    if not allow_protected_room and not (DIRECTORY / "lab-evidence.json").is_file():
        raise ValueError("Complete the short isolated mixed-role check before real deployment")
    current = admin_status()
    require_phy(current, active_profile())
    if "roles applied=7 saved=7" not in current["status"] or \
            "repeater=3 room=3 companion=3 management=3" not in current["role-path"]:
        raise ValueError("Integrated role/path configuration did not survive backup reset")
    if current["source hash"].split()[1] != saved["admin"]["source hash"].split()[1]:
        raise ValueError("Source changed since the pre-real backup")
    acceptance = deployment_evidence(allow_protected_room, current, dashboard())
    client = connect()
    requested_at = datetime.now(timezone.utc).isoformat()
    try:
        checked(client, "radio " + " ".join(str(value) for value in active_profile()))
    finally:
        client.close()
    time.sleep(3)
    final = admin_status()
    require_phy(final, active_profile())
    with Companion() as companion:
        if companion.width != 3:
            raise ValueError("Lab-profile companion path mode mismatch")
    confirmed_at = datetime.now(timezone.utc).isoformat()
    save("deployed.json", {"admin": final, "dashboard": dashboard(), "tcp": f"{mast_host()}:5000",
                           "channel": channel_tag(), "protocol": 13, "retune_requested_at": requested_at,
                           "launch_confirmed_at": confirmed_at, "status_url": f"http://{mast_host()}/api/status",
                           "acceptance": acceptance})
    print(f"Beta mast on isolated 912.525/BW250/SF7/CR5; native protocol13 TCP {mast_host()}:5000; {channel_tag()}; "
          f"launch confirmed {confirmed_at}; status http://{mast_host()}/api/status", flush=True)
    if not acceptance["room_guest_login"]:
        print("Room guest access remains protected and unverified; no credential/policy change", flush=True)


def diagnostic():
    from tools.hardware.lua_checks import start_capture, finish_capture
    capture = start_capture()
    try:
        time.sleep(3)
        require_phy(admin_status(), active_profile())
        print(group_exchange("!test", "RSSI="))
    finally:
        finish_capture(capture, "owner-field-diagnostic.log")


def conversation_check():
    require_phy(admin_status(), active_profile())
    before = dashboard()
    time.sleep(max(0, 61 - before["uptime_ms"] / 1000))
    index = json.loads(private_file(DIRECTORY / "channel.json", 4096))["index"]
    with Companion() as companion:
        if companion.width != 3 or not 0 <= index < companion.channels:
            raise ValueError("Unexpected field channel/path configuration")
        record = companion.command(bytes((31, index)))
        if (len(record) != 50 or record[:2] != bytes((18, index)) or
                record[2:34].split(b"\0", 1)[0] != channel_tag().encode()):
            raise ValueError("Saved field channel index no longer matches; not changing channels")
    label = "conversation-" + str(time.time_ns())
    client = connect()
    try:
        initial = checked(client, "bot stats")
        started = int(time.time())
        rows = cli(label, [
            "sync_msgs", "msgs_subscribe", f"chan {index} '!ping'", "sleep 3",
            f"chan {index} '!calc 6*7'", "sleep 4", "sync_msgs"], timeout=70)
        final = checked(client, "bot stats")
        admission = checked(client, "bot admission")
    finally:
        client.close()
    after = dashboard()
    evidence = {"input": "mast Companion TCP with local reflection",
                "physical_peer_receipt": False, "before": before, "after": after,
                "initial_stats": initial, "final_stats": final, "admission": admission,
                "channel_messages": [row for row in rows if row.get("type") == "CHAN" and
                                     row.get("channel_idx") == index and
                                     row.get("sender_timestamp", 0) >= started]}
    save(label + ".json", evidence)
    bot = next(role for role in after["roles"] if role["role"] == "command-bot")
    messages = [row for row in evidence["channel_messages"]
                if row.get("text", "").startswith(bot["name"] + ": ")]
    texts = [row["text"] for row in messages]
    if (sum(text.endswith(": Pong") for text in texts) < 1 or
            not any(text.endswith(": = 42") for text in texts) or
            any(": Not run: channel cooldown;" in text for text in texts)):
        raise ValueError("Consecutive channel replies missing; inspect " + label)
    if any(row.get("path_hash_mode") != 2 or row.get("txt_type") != 0 for row in messages):
        raise ValueError("Conversation reply path/plaintext mismatch")
    first = dict(item.split("=", 1) for item in initial.split())
    last = dict(item.split("=", 1) for item in final.split())
    if (int(last["Replies"]) - int(first["Replies"]) < 2 or
            last["vm-fail"] != first["vm-fail"] or
            "wait-ms=0 active=0" not in admission):
        raise ValueError("Conversation admission/VM counters mismatch; inspect " + label)
    transmitted = [event for event in after["history"]["events"] if
                   event["direction"] == "tx" and event["source_slot"] == bot["source_slot"] and
                   event["state"] == 2 and event["at_ms"] > before["uptime_ms"]]
    if len(transmitted) < 2 or not bot["ready"] or bot.get("fault"):
        raise ValueError("Conversation bot RF completions/readiness missing; inspect " + label)
    print("PASS consecutive Pong and calculation without a command cooldown; "
          "bot RF TX confirmed, independent handset receipt unverified; " + label, flush=True)


def fleet_peer():
    from tools.hardware.companion_cli import configure
    _, _, identity = configure(public=True, preserve_identity=True, name="Cedar-Base")
    print("Cedar-Base on 912.525/250, three-byte paths, preserved key " + identity, flush=True)


def rf_management_check():
    from tools.hardware.wifi_checks import environment_file, lan_credentials
    if environment_file().resolve() != (ROOT / ".env.dev.local").resolve():
        raise ValueError("RF-only management check requires the authorized .env.dev.local source")
    ssid, _ = lan_credentials()
    before = dashboard()
    identities = {role["role"]: role.get("public_key") for role in before["roles"]}
    gateway = inventory_value("receiver_host")
    profile = dashboard(gateway)["profile"]
    if (tuple(profile[key] for key in
              ("frequency_hz", "bandwidth_hz", "sf", "cr", "tx_power_dbm")) != active_profile() or
            not profile["committed"] or profile["fault"]):
        raise ValueError("RF gateway must already have the committed lab PHY")

    def connect_rf():
        for attempt in range(3):
            try:
                return NativeClient(gateway, 8001, LAB / "companion.seed",
                                    identities["management"], "", timeout=12)
            except TimeoutError:
                if attempt == 2:
                    raise
                print(f"RF management login retry {attempt + 2}/3", flush=True)
                time.sleep(2)

    label = "rf-management-" + str(time.time_ns())
    evidence = {"gateway": gateway, "management_key": identities["management"],
                "path_bytes": 3, "role_authority_changed": False}
    client = connect_rf()
    restore_wifi = False
    try:
        original = {name: checked(client, name) for name in
                    ("status", "wifi status", "source hash", "role-path")}
        require_phy(original, active_profile())
        if "saved=1" not in original["wifi status"] or "connected=1" not in original["wifi status"]:
            raise ValueError("Expected the existing saved, connected WiFi override")
        evidence["before"] = original
        # Change only the SSID; retain the saved password and restore via encrypted RF.
        restore_wifi = True
        offline_ssid = ("rf-only-" + str(time.time_ns())).encode("ascii")
        checked(client, "wifi ssid hex " + offline_ssid.hex())
        checked(client, "wifi apply")
        for _ in range(12):
            time.sleep(1)
            wifi = checked(client, "wifi status")
            if "connected=0" in wifi:
                break
        else:
            raise ValueError("Mast did not disconnect from WiFi")
        evidence["offline_wifi"] = wifi
        evidence["radio_save"] = checked(client, "radio " + " ".join(map(str, active_profile())))
        for _ in range(12):
            time.sleep(1)
            outcome = checked(client, "job")
            if outcome.startswith("Radio applied and saved;"):
                evidence["radio_commit"] = outcome
                break
        else:
            raise TimeoutError("RF common-profile save did not report durable completion")
        evidence["reboot"] = checked(client, "reboot")
        client.close()
        client = None
        time.sleep(12)
        client = connect_rf()
        offline = {name: checked(client, name) for name in
                   ("status", "wifi status", "source hash", "role-path", "bot status")}
        require_phy(offline, active_profile())
        if ("connected=0" not in offline["wifi status"] or
                offline["source hash"] != original["source hash"] or
                offline["role-path"] != original["role-path"] or "ready=1" not in offline["bot status"]):
            raise ValueError("RF management/reboot did not retain the offline mast state")
        evidence["after_offline_reboot"] = offline
        print("PASS independent encrypted RF management while WiFi disconnected, including "
              "common-profile save and RF-requested reboot; native role authority unchanged", flush=True)
    finally:
        if client:
            client.close()
        if restore_wifi:
            recovery = None
            try:
                recovery = connect_rf()
                checked(recovery, "wifi ssid hex " + ssid.hex())
                checked(recovery, "wifi apply")
                for _ in range(20):
                    time.sleep(1)
                    if "connected=1" in checked(recovery, "wifi status"):
                        evidence["wifi_restored_over_rf"] = True
                        break
                else:
                    raise TimeoutError("Encrypted RF WiFi restoration did not reconnect")
            finally:
                if recovery:
                    recovery.close()
                save(label + ".json", evidence)
    after = dashboard()
    if ({role["role"]: role.get("public_key") for role in after["roles"]} != identities or
            tuple(after["profile"][key] for key in
                  ("frequency_hz", "bandwidth_hz", "sf", "cr", "tx_power_dbm")) != active_profile() or
            not after["profile"]["committed"] or after["profile"]["fault"]):
        raise ValueError("Final management identity/shared-profile readback mismatch")
    print("PASS original WiFi restored through encrypted RF; all mast keys and lab PHY retained; " +
          label, flush=True)


def rename_companion(host, port, name, expected_key, apply=True):
    with Companion(host, port) as peer:
        if peer.public_key != expected_key or peer.width != 3:
            raise ValueError("Companion identity/path mismatch before naming")
        before = peer.command(b"\x01" + bytes(7) + b"fleet-names")
        if len(before) < 58 or before[0] != 5 or before[4:36].hex() != expected_key:
            raise ValueError("Invalid native companion identity/profile before naming")
        after = before
        if apply:
            if peer.command(b"\x08" + name.encode("ascii")) != b"\0":
                raise ValueError("Native companion name change was not acknowledged")
            after = peer.command(b"\x01" + bytes(7) + b"fleet-names")
        if (len(before) < 58 or len(after) < 58 or after[0] != 5 or
                after[4:36].hex() != expected_key or after[48:58] != before[48:58] or
                after[2] != before[2] or after[58:].split(b"\0", 1)[0].decode("ascii") != name):
            raise ValueError("Native companion name/identity/PHY readback mismatch")
    return {"name": name, "public_key": expected_key}


def wait_named_adverts(connection, wanted, since, seconds=65, observed=None):
    from tools.hardware.radio_checks import signed_advert
    if observed is None:
        observed = {}

    def inspect(raw, _):
        advert = signed_advert(raw)
        if (advert and raw[0] & 3 == 2 and advert["hops"] == 0 and
                advert["public_key"] in wanted and advert["timestamp"] >= since and
                wanted[advert["public_key"]] == advert["name"]):
            observed[advert["public_key"]] = advert
            if len(observed) == len(wanted):
                return observed
        return None

    if not wanted:
        raise ValueError("Independent receiver requires at least one remote advertised identity")
    try:
        return wait_receipt(connection, None, seconds, inspect, "named zero-hop advertisements")
    except ValueError as error:
        raise ValueError("Missing independent zero-hop adverts: " +
                         ", ".join(name for key, name in wanted.items() if key not in observed)) from error


def fleet_stock_info(label, device_info=None):
    from tools.hardware import companion_cli as stock_cli
    from tools.hardware import companion_device as stock_lab
    for attempt in range(2):
        try:
            rows = stock_cli.run_cli(label + f"-info-{attempt}", ["ver", "infos"], timeout=30)
        except stock_cli.NoResponseError:
            if attempt:
                raise
            print("Stock self-info handshake was empty/truncated; resetting verified Cedar once "
                  "before a read-only retry", flush=True)
            stock_lab.check()
            time.sleep(3)
            continue
        infos = stock_cli.values_with(rows, "public_key")
        versions = stock_cli.values_with(rows, "path_hash_mode")
        if len(infos) != 1 or len(versions) != 1 or versions[0]["path_hash_mode"] != 2:
            raise ValueError("Cedar native identity/three-byte path readback missing")
        if device_info is not None:
            device_info.update(versions[0])
        return infos[0]


def sync_pine_clock(nrf, link):
    reply = nrf.serial_command(link, f"time {int(time.time())}", duration=1).strip()
    if reply.startswith("OK"):
        return
    if reply == "(ERR: clock cannot go backwards)":
        clock = nrf.serial_command(link, "clock").strip()
        minute = datetime.strptime(clock, "%H:%M - %d/%m/%Y UTC").replace(tzinfo=timezone.utc).timestamp()
        if -1 <= time.time() - minute < 61:
            print("Pine native clock is already current; retaining its forward-only time", flush=True)
            return
    raise ValueError("Pine clock synchronization failed: " + reply)


def configure_aspen_names(notify_only=False, restart=True):
    names = {}
    desired = {"repeater": "Aspen-Relay", "room": "Aspen-Room", "companion": "Aspen-Base",
               "bot": "Aspen-Bot", "management": "Aspen-Admin", "kiss": "Aspen"}
    admin = connect()
    try:
        settings = ("status", "bot policy", "role-path", "room access")
        before_admin = {setting: checked(admin, setting) for setting in settings}
        require_phy(before_admin, active_profile())
        roles = {entry["role"]: entry for entry in dashboard()["roles"]}
        source = checked(admin, "source hash")
        for role, name in desired.items():
            if not notify_only:
                checked(admin, f"role name {role} {name}")
            if checked(admin, f"role name {role}") != "Name: " + name:
                raise ValueError("Mast native name readback mismatch: " + role)
            stored_role = "command-bot" if role == "bot" else "bot" if role == "kiss" else role
            names[role] = {"name": name, "public_key": roles[stored_role]["public_key"],
                           "advertised": role != "kiss"}
        if restart and not notify_only:
            checked(admin, "reboot")
            admin.close()
            admin = None
            time.sleep(12)
            admin = connect()
        after = dashboard()
        if {row["role"]: row.get("public_key") for row in after["roles"]} != {
                role: row.get("public_key") for role, row in roles.items()}:
            raise ValueError("Aspen identity changed during naming")
        if checked(admin, "source hash") != source:
            raise ValueError("Aspen source changed during naming")
        for role, name in desired.items():
            if checked(admin, f"role name {role}") != "Name: " + name:
                raise ValueError("Saved mast role name readback mismatch: " + role)
        after_admin = {setting: checked(admin, setting) for setting in settings}
        require_phy(after_admin, active_profile())
        for setting in ("bot policy", "role-path", "room access"):
            if after_admin[setting] != before_admin[setting]:
                raise ValueError("Aspen naming changed " + setting)
    finally:
        if admin:
            admin.close()
    with Companion() as companion:
        if companion.width != 3 or companion.public_key != roles["companion"]["public_key"]:
            raise ValueError("Aspen companion identity/path changed during naming")
        if companion.command(b"\x06" + struct.pack("<I", int(time.time()))) != b"\0":
            raise ValueError("Native clock synchronization rejected before name advertisements")
    return names


def aspen_names(restart=False):
    receiver = inventory_value("receiver_host")
    profile = dashboard(receiver)["profile"]
    if (tuple(profile[key] for key in ("frequency_hz", "bandwidth_hz", "sf", "cr", "tx_power_dbm")) != active_profile() or
            not profile["committed"] or profile["fault"]):
        raise ValueError("Birch receiver is not on the committed lab profile")
    names = configure_aspen_names(restart=restart)
    wanted = {row["public_key"]: row["name"] for row in names.values() if row["advertised"]}
    if len(wanted) != 5:
        raise ValueError("Aspen must expose five distinct advertised identities")
    label = "aspen-names-" + str(time.time_ns())
    evidence = {"names": names, "receiver": receiver, "adverts": {}, "complete": False,
                "reboot_checked": restart}
    try:
        with socket.create_connection((receiver, 8001), timeout=5) as connection, \
                ThreadPoolExecutor(max_workers=1) as executor:
            received = executor.submit(wait_named_adverts, connection, wanted, int(time.time()) - 2,
                                       35, evidence["adverts"])
            admin = connect()
            try:
                for role, row in names.items():
                    if row["advertised"]:
                        row["advert_request"] = checked(admin, f"role advert {role} zerohop")
                        time.sleep(2)
            finally:
                admin.close()
            received.result()
        evidence["complete"] = True
    finally:
        save(label + ".json", evidence)
    persistence = "after Aspen reboot" if restart else "without device reboot"
    print("Aspen labels saved and read back " + persistence +
          "; five signed zero-hop adverts received on Birch; no Go restart; " + label, flush=True)


def fleet_names(notify_only=False):
    from tools.hardware.radio_checks import host_status
    from tools.hardware import companion_cli as stock_cli
    from tools.hardware import companion_device as stock_lab
    sys.path.insert(0, str(ROOT / "firmware/nrf52840"))
    import hardware as nrf
    host = host_status()
    host_keys = {role: state["public_key"] for role, state in host.items()
                 if isinstance(state, dict) and "public_key" in state}
    host_config = json.loads(private_file(Path.home() / ".config/meshcore-host/config.json", 65536))
    expected_host = {"repeater": "Birch-Relay", "room": "Birch-Room",
                     "companion": "Birch-Base", "bot_companion": "Birch-Bot"}
    for role, name in expected_host.items():
        if host_config.get(role + "_name") != name:
            raise ValueError("Set the existing host " + role + "_name configuration to " + name)
    label = ("fleet-adverts-" if notify_only else "fleet-names-") + str(time.time_ns())
    results = {"aspen": configure_aspen_names(notify_only), "birch": {}, "cedar": {}, "pine": {}, "adverts": {},
               "restart_checked": not notify_only}

    for role in ("companion", "bot_companion"):
        endpoint = int(host[role]["endpoint"].rsplit(":", 1)[1])
        results["birch"][role] = rename_companion(
            "127.0.0.1", endpoint, expected_host[role], host[role]["public_key"], apply=not notify_only)
    for role in ("repeater", "room"):
        path = Path.home() / ".local/state/meshcore-host" / role / "state.json"
        envelope = path.with_name("identity-state.json")
        if envelope.exists():
            authority = json.loads(private_file(envelope, 16 * 1024 * 1024))
            if authority.get("pending_identity"):
                raise ValueError("Refusing a naming restart with a staged host identity")
            if authority.get("document") != path.name or not isinstance(authority.get("state"), dict):
                raise ValueError("Host role envelope has no matching active document")
            saved = authority["state"]
        else:
            saved = json.loads(private_file(path, 8 * 1024 * 1024))
        if saved.get("Name") != expected_host[role] or saved.get("Identity") != host[role]["public_key"]:
            raise ValueError("Host native saved name/identity mismatch: " + role)
        results["birch"][role] = {"name": saved["Name"], "public_key": saved["Identity"]}

    pine_port = inventory_value("nrf_port")
    record = nrf.private_directory(DIRECTORY / label)
    pine_before, _, pine_keys = nrf.snapshot(pine_port, record, "pine-before.json", True)
    radio = tuple(map(float, pine_before["get radio"].strip().removeprefix("> ").split(",")))
    if (len(radio) != 4 or abs(radio[0] - 912.525) > .0001 or radio[1:] != (250, 7, 5) or
            pine_before["get tx"].strip() != "> 2" or
            pine_before["get path.hash.mode"].strip() != "> 2"):
        raise ValueError("Pine is not on the lab profile/three-byte path mode")
    with nrf.connection(pine_port) as link:
        for command, key in zip(("get pub.key", "get bot.pub.key"), pine_keys):
            if nrf.serial_command(link, command).strip().lower() != f"active={key} saved={key} reboot=0".lower():
                raise ValueError("Refusing naming reboot with pending/changed Pine identity")
        if not notify_only:
            for command in ("set name Pine-Relay", "set bot.name Pine-Bot"):
                if not nrf.serial_command(link, command, duration=1).strip().startswith("OK"):
                    raise ValueError("Pine native name request failed")
    if not notify_only:
        nrf.hardware_check(pine_port, record, True)
    since = int(time.time()) - 2
    with nrf.connection(pine_port) as link:
        sync_pine_clock(nrf, link)
        for command, name in (("get name", "Pine-Relay"), ("get bot.name", "Pine-Bot")):
            if nrf.serial_command(link, command).strip() != "> " + name:
                raise ValueError("Pine native name readback mismatch")
    results["pine"] = {role: {"name": name, "public_key": key.lower()}
                       for role, name, key in zip(("repeater", "bot"),
                                                 ("Pine-Relay", "Pine-Bot"), pine_keys)}

    if not notify_only:
        stock_lab.check()
        time.sleep(3)
    original_cedar = fleet_stock_info(label)
    cedar = original_cedar
    if original_cedar.get("name") != "Cedar-Base" and not notify_only:
        stock_cli.run_cli(label + "-set", ["set name Cedar-Base"], timeout=30)
        stock_lab.check()
        time.sleep(3)
        cedar = fleet_stock_info(label + "-persist")
    if (cedar.get("name") != "Cedar-Base" or
            any(original_cedar.get(key) != cedar.get(key) for key in
                ("public_key", "radio_freq", "radio_bw", "radio_sf", "radio_cr", "tx_power"))):
        raise ValueError("Cedar persisted name/identity/PHY readback mismatch; inspect " + label)
    if tuple(cedar.get(key) for key in ("radio_freq", "radio_bw", "radio_sf", "radio_cr", "tx_power")) != (
            active_profile()[0] / 1e6, active_profile()[1] / 1e3, *active_profile()[2:]):
        raise ValueError("Refusing Cedar advertisements outside the lab profile")
    results["cedar"] = {"name": "Cedar-Base", "public_key": cedar["public_key"], "path_hash_bytes": 3}
    save(label + "-names.json", results)
    aspen_adverts = {row["public_key"]: row["name"] for role, row in results["aspen"].items()
                     if role != "kiss"}
    birch_adverts = {row["public_key"]: row["name"] for row in results["birch"].values()}
    other_adverts = {row["public_key"]: row["name"] for row in results["pine"].values()}
    other_adverts[results["cedar"]["public_key"]] = "Cedar-Base"

    results["adverts"] = {"received_on_aspen": {}, "received_on_birch": {}}
    try:
        with socket.create_connection((mast_host(), 8001), timeout=5) as aspen_rx, \
                socket.create_connection((inventory_value("receiver_host"), 8001), timeout=5) as birch_rx, \
                ThreadPoolExecutor(max_workers=3) as executor:
            aspen_observed = executor.submit(wait_named_adverts, aspen_rx, birch_adverts | other_adverts,
                                             since, 65, results["adverts"]["received_on_aspen"])
            birch_observed = executor.submit(wait_named_adverts, birch_rx, aspen_adverts | other_adverts,
                                             since, 65, results["adverts"]["received_on_birch"])
            stock_contacts = executor.submit(stock_cli.run_cli, label + "-discovery", [
                "clock sync", "ver", "infos", "advert", "sleep 50", "reload_contacts"], 65)
            # Native Go repeater/room startup adverts are zero-hop; no password or ACL is changed.
            subprocess.run(["systemctl", "--user", "restart", "meshcore-host.service"], check=True)
            deadline = time.monotonic() + 20
            while True:
                try:
                    host_status()
                    break
                except urllib.error.URLError as error:
                    if not isinstance(error.reason, ConnectionRefusedError) or time.monotonic() >= deadline:
                        raise
                    time.sleep(.5)
            for role in ("companion", "bot_companion"):
                endpoint = int(host[role]["endpoint"].rsplit(":", 1)[1])
                with Companion("127.0.0.1", endpoint) as companion:
                    if (companion.public_key != host_keys[role] or
                            companion.command(b"\x06" + struct.pack("<I", int(time.time()))) != b"\0" or
                            companion.command(b"\x07\0") != b"\0"):
                        raise ValueError("Birch native clock/zero-hop request failed")
                time.sleep(2)
            deadline = time.monotonic() + 25
            while not birch_adverts.keys() <= results["adverts"]["received_on_aspen"].keys():
                if time.monotonic() >= deadline:
                    raise TimeoutError("Birch zero-hop startup notices were not independently received")
                time.sleep(.2)
            admin = connect()
            try:
                for role in results["aspen"]:
                    if role != "kiss":
                        results["aspen"][role]["advert"] = checked(admin, f"role advert {role} zerohop")
                        time.sleep(2)
            finally:
                admin.close()
            with nrf.connection(pine_port) as link:
                for command in ("advert.zerohop", "bot advert.zerohop"):
                    reply = nrf.serial_command(link, command, duration=2).strip()
                    if not reply.startswith(("OK", "Queued")):
                        raise ValueError("Pine zero-hop advert request failed")
            aspen_observed.result()
            birch_observed.result()
            contacts = stock_contacts.result()
    finally:
        try:
            save(label + "-adverts.json", results["adverts"])
        finally:
            subprocess.run(["systemctl", "--user", "start", "meshcore-bot.service"], check=True)
    learned = {}
    for row in contacts:
        for key, value in row.items():
            if isinstance(value, dict) and value.get("public_key") == key:
                learned[key] = value
    wanted = aspen_adverts | birch_adverts | {
        row["public_key"]: row["name"] for row in results["pine"].values()}
    results["cedar_learned"] = {key: learned.get(key, {}).get("adv_name") for key in wanted}
    results["cedar_advert_times"] = {key: learned.get(key, {}).get("last_advert", 0) for key in wanted}
    if any(results["cedar_learned"][key] != name or results["cedar_advert_times"][key] < since
           for key, name in wanted.items()):
        save(label + ".json", results)
        raise ValueError("Signed zero-hop adverts received, but stock contact learning differs; inspect " + label)

    final_host = host_status()
    if any(final_host[role]["public_key"] != key for role, key in host_keys.items()):
        raise ValueError("Host identity changed during naming")
    for role in ("companion", "bot_companion"):
        endpoint = int(final_host[role]["endpoint"].rsplit(":", 1)[1])
        rename_companion("127.0.0.1", endpoint, expected_host[role],
                         final_host[role]["public_key"], apply=False)
    pine_after, _, final_pine_keys = nrf.snapshot(pine_port, record, "pine-after.json", True)
    if pine_keys != final_pine_keys or any(pine_before[key] != pine_after[key] for key in
                                         ("get radio", "get tx", "get repeat", "get path.hash.mode")):
        raise ValueError("Pine identity/PHY changed during naming")
    require_phy(admin_status(), active_profile())
    subprocess.run(["systemctl", "--user", "is-active", "meshcore-host.service",
                    "meshcore-bot.service", "meshcore-bot-service.service"],
                   stdout=subprocess.DEVNULL, check=True)
    save(label + ".json", results)
    print("PASS all fleet native name readbacks; independent signed zero-hop adverts and stock peer "
          "learning verified; keys/PHY/source unchanged; " + label,
          flush=True)


def fleet_gateway():
    from tools.hardware.wifi_checks import write
    host = inventory_value("receiver_host")
    before = dashboard(host)
    save("birch-before-" + str(time.time_ns()) + ".json", before)
    subprocess.run(["systemctl", "--user", "stop", "meshcore-host.service"], check=True)
    config = DIRECTORY / "birch-controller.json"
    write(config, {
        "radio_address": host + ":8001", "phy_authority": "host", "require_parity": True,
        "enabled_roles": [], "state_dir": "birch-controller-state",
        "radio": {"FreqHz": active_profile()[0], "BwHz": active_profile()[1], "SF": 7, "CR": 5}, "tx_power": 2,
        "status_listen": "127.0.0.1:19089", "advert_interval_seconds": 0,
        "mqtt": {"url": "", "broker_listen": ""}})
    with (DIRECTORY / ("birch-controller-" + str(time.time_ns()) + ".log")).open("x") as log:
        process = subprocess.Popen([str(ROOT / "bin/meshcore-host"), "-config", str(config)],
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 25
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise ValueError("Birch controller exited; inspect its private log")
                time.sleep(1.1)
                state = dashboard(host)
                profile = state["profile"]
                if (tuple(profile[key] for key in
                          ("frequency_hz", "bandwidth_hz", "sf", "cr", "tx_power_dbm")) == active_profile() and
                        profile["committed"] and not profile["fault"]):
                    save("birch-lab-" + str(time.time_ns()) + ".json", state)
                    print("Birch KISS radio committed to 912.525/250; host roles paused, identities retained",
                          flush=True)
                    return
            raise TimeoutError("Birch radio did not apply the lab profile")
        finally:
            process.terminate()
            process.wait(timeout=10)


def multihop_install(interrupt=False):
    from tools.hardware.admin import download, install
    sys.path.insert(0, str(ROOT / "firmware/nrf52840"))
    import hardware as nrf
    port = inventory_value("nrf_port")
    label = "multihop-" + str(time.time_ns())
    record = nrf.private_directory(DIRECTORY / label)
    first, _, identities = nrf.snapshot(port, record, "relay-before.json", True)
    relay_phy = tuple(map(float, first["get radio"].strip().removeprefix("> ").split(",")))
    if (len(relay_phy) != 4 or abs(relay_phy[0] * 1_000_000 - active_profile()[0]) > 100 or
            relay_phy[1:] != (active_profile()[1] / 1000, active_profile()[2], active_profile()[3]) or
            first["get tx"].strip() != "> 2" or first["get repeat"].strip() != "> on" or
            first["get path.hash.mode"].strip() != "> 2"):
        raise ValueError("Pine must be repeating on the lab PHY before routed installation")
    relay = bytes.fromhex(identities[0])
    state = dashboard()
    target = next(role["public_key"] for role in state["roles"] if role["role"] == "management")
    require_phy(admin_status(), active_profile())
    gateway = dashboard(inventory_value("receiver_host"))
    if (gateway["profile"]["frequency_hz"] != active_profile()[0] or
            gateway["profile"]["bandwidth_hz"] != active_profile()[1] or gateway["kiss"]["connected"]):
        raise ValueError("Birch must be idle on the lab PHY before routed installation")
    route = b"\x81" + relay[:3]
    wrong = b"\x81" + bytes(value ^ 0xff for value in relay[:3])
    if wrong[1:] in [bytes.fromhex(role["public_key"])[:3] for role in state["roles"]
                     if role.get("public_key")]:
        raise ValueError("Negative-control route collides with a live field role")
    evidence = {"gateway": inventory_value("receiver_host"), "relay_key": identities[0], "target": target,
                "path_bytes": 3, "route": route.hex(), "negative_control_denied": False}
    try:
        unreachable = NativeClient(inventory_value("receiver_host"), 8001, LAB / "companion.seed", target, "",
                                   timeout=5, path=wrong)
    except TimeoutError:
        evidence["negative_control_denied"] = True
    else:
        unreachable.close()
        raise ValueError("False route reached the mast; physical routing is not enforced")
    client = NativeClient(inventory_value("receiver_host"), 8001, LAB / "companion.seed", target, "", path=route)
    original = None
    try:
        original_status = checked(client, "source status")
        original = checked(client, "source hash")
        evidence["original_source_status"] = original_status
        source = download(client)
        if checked(client, "source hash") != original:
            raise ValueError("Useful source changed before routed installation")
        nrf.save(record, "source-before.lua", source)
        evidence["original_sha256"] = original.split()[1]
        evidence["installed_sha256"] = hashlib.sha256(source).hexdigest()
        if interrupt:
            class PlannedDisconnect(Exception):
                pass

            def progress(message):
                print(message, flush=True)
                if message.startswith("Durable chunk 2/"):
                    raise PlannedDisconnect

            count = (len(source) + 47) // 48
            staged = checked(client, "source status")
            expected = f"upload={evidence['installed_sha256'][:16]} next=2/{count};"
            if expected not in staged:
                try:
                    install(client, source, progress=progress)
                except PlannedDisconnect:
                    pass
                else:
                    raise ValueError("Installer did not reach the planned two-chunk interruption")
            else:
                print("Reusing the matching two-chunk durable upload from the previous interruption", flush=True)
            evidence["interrupted_after_chunks"] = 2
            client.close()
            admin = connect()
            try:
                print(checked(admin, "reboot"), flush=True)
                evidence["mast_reboot_requested"] = True
            finally:
                admin.close()
            time.sleep(12)
            client = NativeClient(inventory_value("receiver_host"), 8001, LAB / "companion.seed", target, "", path=route)
            pending = checked(client, "source status")
            if f"next=2/{count};" not in pending or checked(client, "source hash") != original:
                raise ValueError("Reboot did not preserve exactly two staged chunks and the useful source")
            evidence["reboot_resume_status"] = pending
            print("PASS two durable chunks survived disconnect and mast reboot; resuming same upload",
                  flush=True)
        evidence["activation"] = install(client, source)
        returned = download(client)
        if returned != source or checked(client, "source hash") == original:
            raise ValueError("Physical multihop source readback differs")
        evidence["readback_bytes"] = len(returned)
        print(f"PASS physical three-radio source upload, durable activation and exact {len(returned)}-byte readback",
              flush=True)
    finally:
        recovery = None
        try:
            try:
                selected = checked(client, "source hash") if original is not None else None
            except (OSError, ValueError) as error:
                evidence["rf_cleanup_error"] = str(error)
                print("RF cleanup unavailable; checking source through authenticated web: " + str(error),
                      flush=True)
                recovery = connect()
                selected = checked(recovery, "source hash")
            cleanup = recovery or client
            if original is not None and selected != original:
                if selected.split()[1] != original.split()[1]:
                    raise ValueError("Unexpected source content selected; refusing unrelated-source rollback")
                print(checked(cleanup, "source rollback"), flush=True)
                for _ in range(20):
                    time.sleep(.5)
                    outcome = checked(cleanup, "source status")
                    if "Error:" in outcome:
                        raise ValueError("Useful source rollback failed: " + outcome)
                    if "durably saved and active" in outcome:
                        break
                else:
                    raise TimeoutError("Useful source rollback activation did not complete")
                if (checked(cleanup, "source hash").split()[1] != original.split()[1] or
                        outcome.split()[1] != original_status.split()[1]):
                    raise ValueError("Useful source rollback failed; inspect mast source status")
                evidence["original_source_restored"] = True
            evidence["routed_responses"] = client.routed_responses
            evidence["incomplete_direct_copies_ignored"] = client.incomplete_routes
            evidence["return_route_repairs"] = client.route_repairs
        finally:
            if recovery:
                recovery.close()
            client.close()
            nrf.save(record, "installer.json", evidence)
    second, _, final_ids = nrf.snapshot(port, record, "relay-after.json", True)
    if final_ids != identities:
        raise ValueError("Relay identities changed")
    before_packets = nrf.packet_stats(first["stats-packets"])[0]
    after_packets = nrf.packet_stats(second["stats-packets"])[0]
    if (after_packets["direct_tx"] <= before_packets["direct_tx"] or
            not evidence.get("original_source_restored") or
            not evidence["routed_responses"] or not evidence["incomplete_direct_copies_ignored"]):
        raise ValueError("Missing physical relay forwarding/source restoration evidence")
    print("PASS enforced physical multihop installer and rollback; three-byte paths, wrong-hop rejection, "
          "unfinished RF copies ignored, relay TX counted, identities unchanged; " + label, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "lab-check", "room-check", "room-admin-check", "deploy-protected",
                                         "backup", "deploy", "status", "diagnostic", "conversation-check",
                                         "fleet-gateway", "fleet-peer", "fleet-names", "fleet-adverts",
                                         "aspen-names", "aspen-names-persist",
                                         "multihop-install", "multihop-resume",
                                         "rf-management-check"))
    args = parser.parse_args()
    os.umask(0o077)
    DIRECTORY.mkdir(mode=0o700, exist_ok=True)
    if args.action == "status":
        for key, value in admin_status().items():
            print(key + ": " + value)
        with Companion() as client:
            print(f"TCP protocol=13 path-bytes={client.width} public-key={client.public_key}")
    else:
        {"prepare": prepare, "lab-check": lab_check, "backup": backup, "deploy": deploy,
         "deploy-protected": lambda: deploy(allow_protected_room=True),
         "diagnostic": diagnostic, "conversation-check": conversation_check,
         "fleet-gateway": fleet_gateway, "fleet-peer": fleet_peer, "multihop-install": multihop_install,
         "rf-management-check": rf_management_check,
         "fleet-names": fleet_names,
         "aspen-names": aspen_names,
         "aspen-names-persist": lambda: aspen_names(restart=True),
         "fleet-adverts": lambda: fleet_names(notify_only=True),
         "multihop-resume": lambda: multihop_install(interrupt=True),
         "room-check": room_check, "room-admin-check": room_admin_check}[args.action]()


if __name__ == "__main__":
    main()
