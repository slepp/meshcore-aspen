#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bounded checks of the explicitly selected companion radio.

--management-only --keep-peer uses the stock companion and native encrypted CLI
to exercise the mast management identity without WiFi. It changes saved power,
reboots, follows a temporary SF8 profile and its automatic return, then restores
WiFi and the lab PHY. Credential-bearing CLI scripts/output stay in memory.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
import secrets
import shlex
import time

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware import mast_checks as field
from tools.hardware import companion_cli as stock_cli
from tools.hardware import companion_device as stock


def channel_messages(documents):
    return [entry for entry in documents if entry.get("type") == "CHAN" and
            entry.get("channel_idx") == 1]


def verify_channel_messages(documents):
    messages = [entry for entry in channel_messages(documents)
                if entry.get("text", "").startswith("MeshCore command bot: ")]
    if not any(entry.get("text") == "MeshCore command bot: Pong" for entry in messages):
        raise ValueError("No stock RF channel Pong on the real PHY")
    if not stock_cli.has_radio_diagnostic(messages):
        raise ValueError("No numeric stock RF channel diagnostic on the real PHY")
    if any(entry.get("path_hash_mode") != 2 or entry.get("txt_type") != 0 or
           type(entry.get("SNR")) not in (int, float) or not -30 <= entry["SNR"] <= 30
           for entry in messages):
        raise ValueError("Stock RF replies lack three-byte paths/plain-text type/receive metadata")
    return messages


def matched_tcp_reply(tcp, radio):
    fields = ("channel_idx", "sender_timestamp", "txt_hash", "text")
    return any(entry.get("type") == "CHAN" and
               all(key in peer and entry.get(key) == peer[key] for key in fields)
               for entry in tcp for peer in radio if peer.get("text") == "MeshCore command bot: Pong")


def configure(label, preserve_peer=False):
    current = field.admin_status()
    field.require_phy(current, field.active_profile())
    state = field.dashboard()
    roles = {role["role"]: role for role in state["roles"]}
    _, bot, public_key = stock_cli.configure(
        public=True, field=False, preserve_identity=preserve_peer,
        name="Cedar-Base" if preserve_peer else None)
    if bot != roles["command-bot"]["public_key"]:
        raise ValueError("Stock fixture bot contact differs from the live mast")
    key = hashlib.sha256(field.channel_tag().encode()).digest()[:16].hex()
    channel = stock_cli.run_cli(label + "-channel", [
        f"set_channel 1 {field.channel_tag()} {key}", "get_channel 1"], channel_info=True)
    records = [entry for entry in channel if entry.get("channel_idx") == 1]
    if not records or records[-1].get("channel_name") != field.channel_tag() or records[-1].get("channel_secret") != key:
        raise ValueError("Stock hashtag key/name readback mismatch")
    return roles, bot, public_key


def exercise(extended=False, private_only=False, https_only=False):
    label = "field-" + secrets.token_hex(4)
    roles, bot, public_key = configure(label)
    if private_only or https_only:
        stock_cli.run_cli(label + "-advert", ["advert", "sleep 2"])
        return {"profile": list(field.active_profile()), "stock_version": "1.17.1",
                "stock_public_key": public_key, "bot_key": bot, "log_prefix": label,
                "peer_restored": False, "extended": network_concurrent(label, bot) if https_only else
                extended_exercise(label, bot, roles, private_only=True)}
    field.cli(label + "-tcp-before", ["ver", "infos", "sync_msgs"])
    replies = stock_cli.run_cli(label + "-channel-commands", [
        "sync_msgs", "msgs_subscribe", "chan 1 '!ping'", "sleep 1", "ver", "sleep 3",
        "sleep 35", "chan 1 '!test'", "sleep 1", "ver", "sleep 3", "sync_msgs"], timeout=90)
    messages = verify_channel_messages(replies)
    print("PASS independent stock 1.17.1 RF hashtag Pong and RSSI/SNR on 912.525", flush=True)

    nonce = "field-tcp-" + secrets.token_hex(4)
    with ThreadPoolExecutor(max_workers=1) as executor:
        receiver = executor.submit(stock_cli.run_cli, label + "-tcp-receiver", [
            "sync_msgs", "msgs_subscribe", "sleep 6", "ver", "sleep 6", "sync_msgs"])
        time.sleep(4)
        index = json.loads(field.private_file(field.DIRECTORY / "channel.json", 4096))["index"]
        tcp = field.cli(label + "-tcp-send", ["ver", "infos", "sync_msgs", f"chan {index} {nonce}"])
        received = channel_messages(receiver.result())
    if not any(nonce in entry.get("text", "") for entry in received):
        raise ValueError("Stock RF peer did not receive the native TCP client's channel message")
    if not matched_tcp_reply(tcp, messages):
        raise ValueError("Native TCP client did not receive the real-PHY channel Pong")
    print("PASS stock CLI over mast TCP send/receive with independent RF; not a mobile-app test", flush=True)

    relay = roles["repeater"]["public_key"]
    path = stock_cli.three_byte_relay_path(relay, bot)
    routed = stock_cli.run_cli(label + "-local-relay", [
        "sync_msgs", "msgs_subscribe", "sleep 35", "advert", "sleep 2",
        "change_path beta-bot " + path,
        "msg beta-bot '!test'", "sleep 1", "ver", "sleep 4", "sync_msgs",
        "reset_path beta-bot"], timeout=75)
    direct = [entry for entry in routed if entry.get("type") == "PRIV" and
              entry.get("pubkey_prefix") == bot[:12] and entry.get("txt_type") == 0]
    if not any("local; RSSI/SNR unavailable" in entry.get("text", "") for entry in direct):
        raise ValueError("Real-PHY native repeater-to-bot route not confirmed")
    print("PASS RF request through mast repeater into distinct local bot and RF reply; not three radios", flush=True)
    extra = extended_exercise(label, bot, roles) if extended else {}
    return {"profile": list(field.active_profile()), "stock_version": "1.17.1",
            "stock_public_key": public_key, "bot_key": bot, "channel_messages": messages,
            "tcp_rf_messages": received, "routed_messages": direct, "log_prefix": label,
            "native_tcp_send_receive": True, "mobile_app_send_receive": False,
            "room_guest_login": False, "peer_restored": False, "extended": extra}


def expected_reply(documents, bot, expected, channel=False):
    replies = [entry for entry in documents
               if (entry.get("type") == "CHAN" and entry.get("channel_idx") == 1 if channel else
                   entry.get("type") == "PRIV" and entry.get("pubkey_prefix") == bot[:12])]
    if not any(expected in entry.get("text", "") for entry in replies):
        texts = [entry.get("text", "") for entry in replies]
        raise ValueError("Expected RF reply " + repr(expected) + "; received " + repr(texts))
    for entry in replies:
        mode, length = entry.get("path_hash_mode"), entry.get("path_len")
        path_reported = (mode == 2 if channel else
                         type(mode) is int and mode in (0, 1, 2) and
                         type(length) is int and 0 <= length <= 64 // (mode + 1))
        direct_unknown = (not channel and entry.get("path_hash_mode") == -1 and
                          entry.get("path_len") == 255)
        if not (path_reported or direct_unknown) or entry.get("txt_type") != 0:
            raise ValueError("Reply lacks native plaintext/path metadata or three-byte originated channel path")
    return replies


def extended_exercise(label, bot, roles, private_only=False):
    records = {}
    nonce = secrets.token_hex(4)

    def exchange(name, command, expected, channel=False):
        time.sleep(35)
        instruction = "chan 1 " if channel else "msg beta-bot "
        rows = stock_cli.run_cli(label + "-" + name, [
            "sync_msgs", "msgs_subscribe", instruction + shlex.quote(command),
            "sleep 1", "ver", "sleep 12", "sync_msgs"], timeout=40)
        replies = expected_reply(rows, bot, expected, channel)
        records[name] = {"request": command, "replies": replies,
                         "stock_submit": [entry for entry in rows if "expected_ack" in entry]}
        print("PASS stock RF " + name + "; receive verified independently of submission/ACK", flush=True)
        return replies

    if not private_only:
        exchange("utility", "!calc 6*7", "42", True)
        board = "field-" + nonce
        exchange("board-put", "!board put " + board + " mugs-" + nonce, "committed", True)
        exchange("board-get", "!board get " + board, "mugs-" + nonce, True)
        exchange("board-delete", "!board delete " + board, "deleted; committed", True)
    note = "field-" + nonce
    exchange("note-put", "!remember " + note + " private-" + nonce, "committed")
    exchange("reminder-create", "!remind 150s reboot-" + nonce, "pending; due UTC")
    client = field.connect()
    try:
        field.checked(client, "reboot")
    finally:
        client.close()
    time.sleep(15)
    field.require_phy(field.admin_status(), field.active_profile())
    # A fresh stock flood exchange elicits the native authenticated reciprocal PATH
    # after reboot. An advert alone does not establish a reminder-eligible route.
    stock_cli.run_cli(label + "-fresh-path", [
        f"add_contact {bot} 1 beta-bot",
        "sync_msgs", "msgs_subscribe", "reset_path beta-bot", "advert", "sleep 2",
        "msg beta-bot '!ping'", "sleep 1", "ver", "sleep 8", "sync_msgs"], timeout=30)
    exchange("note-after-reboot", "!recall " + note, "private-" + nonce)
    reminders = stock_cli.run_cli(label + "-autonomous-reminder", [
        "sync_msgs", "msgs_subscribe", "sleep 100", "ver", "sleep 5", "sync_msgs"], timeout=120)
    records["autonomous-reminder"] = expected_reply(reminders, bot, "reboot-" + nonce)
    exchange("reminder-state", "!reminders", "sent")
    exchange("note-delete", "!forget " + note, "deleted; committed")
    exchange("https-health", "!service health", "Home health: ok")
    exchange("https-echo", "!service echo " + nonce, "Home echo: " + nonce)
    records["https-local-concurrent"] = network_concurrent(label, bot)
    print("PASS actual stock private notes/reboot/reminder and direct HTTPS commands with local work", flush=True)
    return records


def network_concurrent(label, bot):
    time.sleep(35)
    with field.Companion() as first, field.Companion() as second:
        if first.width != 3 or second.width != 3:
            raise ValueError("Concurrent protocol13 clients lost three-byte paths")
        state = field.dashboard()
        rows = stock_cli.run_cli(label + "-https-local-concurrent", [
            "sync_msgs", "msgs_subscribe", "msg beta-bot '!weather Calgary'",
            "sleep 1", "chan 1 '!ping'", "sleep 1", "ver", "sleep 15", "sync_msgs"], timeout=35)
        weather = expected_reply(rows, bot, "Weather ")
        ping = expected_reply(rows, bot, "Pong", channel=True)
        texts = [entry["text"] for entry in weather]
        if not any("Weather " in text and " C; code " in text and "; age " in text for text in texts):
            raise ValueError("Native weather response shape missing")
        pong_index = next(i for i, entry in enumerate(rows)
                          if entry.get("type") == "CHAN" and "Pong" in entry.get("text", ""))
        weather_index = next(i for i, entry in enumerate(rows)
                             if entry.get("type") == "PRIV" and entry.get("text", "").startswith("Weather "))
        if pong_index >= weather_index:
            raise ValueError("Local channel reply did not precede pending HTTPS weather result")
        result = {"private_messages": weather, "channel_messages": ping,
                  "local_reply_before_https": True,
                  "tcp_clients": 2, "dashboard_uptime_ms": state["uptime_ms"]}
    print("PASS independent channel Pong before HTTPS weather completion with two TCP clients", flush=True)
    return result


def source_smoke():
    label = "source-" + secrets.token_hex(4)
    _, bot, public_key = configure(label)
    nonce = secrets.token_hex(4)
    records = {}
    for name, command, expected in (
            ("ping", "!ping", "Pong"),
            ("test", "!test", "RSSI="),
            ("mt", "!mt 1", "unique paths in ")):
        time.sleep(35)
        rows = stock_cli.run_cli(label + "-" + name, [
            "sync_msgs", "msgs_subscribe", "chan 1 " + shlex.quote(command),
            "sleep 1", "ver", "sleep 8", "sync_msgs"], timeout=25)
        records[name] = expected_reply(rows, bot, expected, channel=True)
        print("PASS unchanged stock RF source smoke: " + name, flush=True)
    return {"profile": list(field.active_profile()), "stock_version": "1.17.1",
            "stock_public_key": public_key, "bot_key": bot, "source_smoke": records,
            "log_prefix": label, "peer_restored": False}


def admission_exercise(preserve_peer=False):
    label = "admission-" + secrets.token_hex(4)
    roles, bot, public_key = configure(label, preserve_peer)
    client = field.connect()
    try:
        initial = field.checked(client, "bot stats")
        started = int(time.time())
        rows = stock_cli.run_cli(label, [
            "ver", "sync_msgs", "msgs_subscribe", "sleep 3",
            "chan 1 '!ping'", "sleep 2", "ver",
            "chan 1 '!calc 6*7'", "sleep 2", "ver", "sync_msgs"], timeout=120)
        final = field.checked(client, "bot stats")
        admission = field.checked(client, "bot admission")
    finally:
        client.close()
    prefix = roles["command-bot"]["name"] + ": "
    messages = [row for row in channel_messages(rows)
                if row.get("sender_timestamp", 0) >= started - 5 and
                row.get("text", "").startswith(prefix)]
    for text in (": Pong", ": = 42"):
        expected_reply(messages, bot, text, channel=True)
    texts = [row["text"] for row in messages]
    if (len(texts) < 2 or
            any(": Not run: channel cooldown;" in text for text in texts) or
            any(type(row.get("SNR")) not in (int, float) or not -30 <= row["SNR"] <= 30
                for row in messages)):
        raise ValueError("Independent stock admission sequence/receive metadata mismatch")
    first = dict(item.split("=", 1) for item in initial.split())
    last = dict(item.split("=", 1) for item in final.split())
    if (int(last["Replies"]) - int(first["Replies"]) < 2 or
            last["vm-fail"] != first["vm-fail"] or "wait-ms=0 active=0" not in admission):
        raise ValueError("Independent stock admission/VM counters mismatch")
    print("PASS independent stock RF consecutive Pong and calculation without command cooldown; "
          "actual peer receipt confirmed", flush=True)
    return {"profile": list(field.active_profile()), "stock_version": "1.17.1",
            "stock_public_key": public_key, "bot_key": bot, "log_prefix": label,
            "initial_stats": initial, "final_stats": final, "admission": admission,
            "channel_messages": messages, "physical_peer_receipt": True, "peer_restored": False}


def path_exercise():
    label = "path-width-" + secrets.token_hex(4)
    roles, bot, public_key = configure(label, preserve_peer=True)
    relay = roles["repeater"]["public_key"]
    path = stock_cli.three_byte_relay_path(relay, bot)
    before = field.dashboard()
    rows = stock_cli.run_cli(label, [
        "ver", "sync_msgs", "msgs_subscribe", "advert", "sleep 2",
        f"change_path {bot} {path}", f"msg {bot} '!test'",
        "sleep 1", "ver", "sleep 6", "sync_msgs", f"reset_path {bot}"], timeout=35)
    replies = expected_reply(rows, bot, "local; RSSI/SNR unavailable")
    prefix = "0a81" + relay[:6].lower()
    received = [entry for entry in field.dashboard()["history"]["events"] if
                entry["direction"] == "rx" and entry["at_ms"] > before["uptime_ms"] and
                entry["preview_hex"].startswith(prefix) and
                type(entry.get("rssi_dbm")) in (int, float)]
    if not received:
        raise ValueError("No physical RX of the explicit three-byte request; inspect " + label)
    print("PASS actual RF request header " + prefix + ", independent stock endpoint reply; "
          "three-byte explicit route, not a three-radio acceptance", flush=True)
    return {"profile": list(field.active_profile()),
            "stock_public_key": public_key, "bot_key": bot, "route": path,
            "request_rf_events": received, "replies": replies,
            "physical_peer_receipt": True, "peer_restored": False, "log_prefix": label}


def management_reply(rows, management, prefix, expected):
    matches = [row for row in rows if row.get("type") == "PRIV" and
               row.get("pubkey_prefix") == management[:12] and
               row.get("text", "").startswith(prefix)]
    valid = [row for row in matches if expected in row["text"][len(prefix):] and
             type(row.get("SNR")) in (int, float)]
    if not valid:
        raise ValueError(f"Missing/mismatched stock RF management receipt for tag {prefix}: "
                         f"expected {expected!r}, received {len(matches)} tagged replies")
    return valid[-1]


def management_exercise():
    from tools.hardware.wifi_checks import environment_file, lan_credentials
    if environment_file().resolve() != (field.ROOT / ".env.dev.local").resolve():
        raise ValueError("Stock management requires the authorized .env.dev.local source")
    ssid, _ = lan_credentials()
    before = field.dashboard()
    keys = {role["role"]: role.get("public_key") for role in before["roles"]}
    management = keys["management"]
    original = field.admin_status()
    field.require_phy(original, field.active_profile())
    if "saved=1" not in original["wifi status"] or "connected=1" not in original["wifi status"]:
        raise ValueError("Expected the existing saved, connected WiFi override")
    stock.check()
    time.sleep(3)
    peer = stock_cli.read_only_status()
    if (peer["native_path_hash_mode"] != 2 or peer["phy"] != {
            "radio_freq": 912.525, "radio_bw": 250, "radio_sf": 7, "radio_cr": 5, "tx_power": 2}):
        raise ValueError("Stock peer must already have the three-byte lab profile")
    password = field.private_file(field.LAB / "password", 15).decode("ascii")
    label = "stock-management-" + str(time.time_ns())
    tag = secrets.randbits(8)
    expected = {}

    def command(lines, text, expect=None, delay=2):
        nonlocal tag
        prefix = f"{tag:02x}|"
        tag = (tag + 1) & 255
        lines.extend([f"cmd {management} " + shlex.quote(prefix + text),
                      f"sleep {delay}", "ver", "sync_msgs"])
        if text in ("status", "wifi status", "source hash", "role-path", "job"):
            lines.extend([f"cmd {management} " + shlex.quote(prefix + text),
                          "sleep 2", "ver", "sync_msgs"])
        if expect is not None:
            expected[prefix] = expect
        return prefix

    def login(lines):
        lines.append(f"reset_path {management}")
        for _ in range(2):
            lines.extend([f"login {management} " + shlex.quote(password), "sleep 2"])

    lines = ["ver", "infos", "clock sync", "sync_msgs", "msgs_subscribe",
             f"add_contact {management} 2 Aspen-Admin"]
    login(lines)
    command(lines, "status", "PHY=912525000,250000,7,5,2")
    command(lines, "source hash", original["source hash"])
    preflight = stock_cli.run_cli(label + "-login", lines, private_script=True, allow_login_failure=True)
    versions = stock_cli.values_with(preflight, "ver")
    if not versions or "1.17.1" not in versions[-1]["ver"]:
        raise ValueError("Management acceptance requires the unchanged stock 1.17.1 companion")
    logins = [index for index, row in enumerate(preflight) if row.get("login_success") is True]
    if not logins:
        raise ValueError("Stock companion did not authenticate to the management identity")
    preflight = preflight[logins[0] + 1:]
    for prefix, value in expected.items():
        management_reply(preflight, management, prefix, value)
    expected.clear()
    temporary_seconds = 60
    evidence = {"stock_version": "1.17.1", "stock_public_key": peer["public_key"],
                "management_key": management, "path_bytes": 3, "role_authority_changed": False,
                "offline_verified": False, "temporary_seconds": temporary_seconds}
    lines = ["sync_msgs", "msgs_subscribe"]
    login(lines)
    command(lines, "wifi ssid " + ("rf-only-" + str(time.time_ns())).encode().hex())
    command(lines, "wifi apply", delay=4)
    command(lines, "wifi status", "connected=0")
    command(lines, "radio 912525000 250000 7 5 1", "Accepted radio change")
    command(lines, "job", "Radio applied and saved;")
    command(lines, "status", "PHY=912525000,250000,7,5,1")
    command(lines, "reboot", "Accepted reboot", delay=14)
    login(lines)
    command(lines, "status", "PHY=912525000,250000,7,5,1")
    command(lines, "wifi status", "connected=0")
    command(lines, "source hash", original["source hash"])
    command(lines, "role-path", original["role-path"])
    command(lines, f"tempradio {temporary_seconds} 912525000 250000 8 5 1", "Accepted radio change")
    lines.extend(["set radio 912.525,250,8,5", "infos", "sleep 1"])
    login(lines)
    command(lines, "status", "PHY=912525000,250000,8,5,1")
    command(lines, "wifi status", "connected=0")
    command(lines, "status", "temp=1")
    lines.extend(["infos", f"sleep {temporary_seconds + 3}", "set radio 912.525,250,7,5", "sleep 1"])
    login(lines)
    command(lines, "status", "PHY=912525000,250000,7,5,1")
    command(lines, "status", "temp=0")
    command(lines, "job", "Temporary radio restored;")
    try:
        rows = stock_cli.run_cli(label + "-offline", lines, timeout=300,
                                 private_script=True, allow_login_failure=True)
        logins = [index for index, row in enumerate(rows) if row.get("login_success") is True]
        evidence["offline_logins"] = [row["login_success"] for row in rows if "login_success" in row]
        if logins:
            rows = rows[logins[0] + 1:]
        # Only these public readbacks/receipts are retained, never the private
        # script or replies to credential-bearing commands.
        evidence["offline_expected"] = dict(expected)
        evidence["offline_observed"] = [
            row for row in rows if row.get("type") == "PRIV" and
            row.get("pubkey_prefix") == management[:12] and
            any(row.get("text", "").startswith(prefix) for prefix in expected)]
        login_results = evidence["offline_logins"]
        if (len(login_results) != 8 or
                any(not any(login_results[index:index + 2]) for index in range(0, 8, 2))):
            raise ValueError("Stock companion reauthentication failed across offline transitions: " +
                             str(evidence["offline_logins"]))
        receipts = [management_reply(rows, management, prefix, value)
                    for prefix, value in expected.items()]
        modes = stock_cli.values_with(rows, "radio_sf")
        if not any(row["radio_sf"] == 8 and row.get("public_key") == peer["public_key"] for row in modes):
            raise ValueError("Stock companion never confirmed the temporary physical SF8 profile")
        evidence["offline_receipts"] = receipts
        evidence["physical_sf8_peer"] = True
        evidence["offline_verified"] = True
        print("PASS stock companion encrypted management with WiFi disconnected: persistent power "
              "change/reboot, actual SF8 exchange and automatic SF7 return", flush=True)
    finally:
        # A failed script can leave the peer on SF8. Wait out the bounded mast
        # temporary profile, then restore both through the same stock interface.
        stock.check()
        time.sleep(3)
        expected.clear()
        recovery = ["set radio 912.525,250,7,5", "set tx 2", "clock sync",
                    "sync_msgs", "msgs_subscribe"]
        if not evidence["offline_verified"]:
            recovery.append(f"sleep {temporary_seconds + 3}")
        login(recovery)
        command(recovery, "radio 912525000 250000 7 5 2", "Accepted radio change")
        command(recovery, "job", "Radio applied and saved;")
        command(recovery, "wifi ssid " + ssid.hex())
        command(recovery, "wifi apply", delay=8)
        command(recovery, "wifi status", "connected=1")
        command(recovery, "status", "PHY=912525000,250000,7,5,2")
        command(recovery, "source hash", original["source hash"])
        command(recovery, "role-path", original["role-path"])
        recovery.extend(["ver", "infos"])
        restored = stock_cli.run_cli(label + "-restore", recovery, timeout=180,
                                     private_script=True, allow_login_failure=True)
        logins = [index for index, row in enumerate(restored) if row.get("login_success") is True]
        if not logins:
            raise ValueError("Stock companion recovery login did not authenticate")
        restored = restored[logins[0] + 1:]
        evidence["restoration_receipts"] = [
            management_reply(restored, management, prefix, value) for prefix, value in expected.items()]
        infos = stock_cli.values_with(restored, "public_key")
        modes = stock_cli.values_with(restored, "path_hash_mode")
        if (not infos or infos[-1]["public_key"] != peer["public_key"] or
                any(infos[-1].get(key) != value for key, value in peer["phy"].items()) or
                not modes or modes[-1]["path_hash_mode"] != 2):
            raise ValueError("Stock peer identity/PHY/path settings were not restored")
        after = field.dashboard()
        if {role["role"]: role.get("public_key") for role in after["roles"]} != keys:
            raise ValueError("Mast identity changed during stock management acceptance")
        evidence["all_keys_retained"] = True
        evidence["wifi_restored_by_stock_rf"] = True
        field.save(label + ".json", evidence)
    return evidence


def main(extended=False, smoke=False, private_only=False, https_only=False, admission_only=False,
         keep_peer=False, lab=False, path_only=False, management_only=False):
    os.umask(0o077)
    if path_only and not keep_peer:
        raise ValueError("--path-only requires --keep-peer")
    if management_only and not keep_peer:
        raise ValueError("--management-only requires --keep-peer")
    if lab and not ((admission_only or path_only or management_only) and keep_peer):
        raise ValueError("--lab requires an admission, path or management check with --keep-peer")
    field.require_phy(field.admin_status(), field.active_profile())
    if keep_peer:
        if not (admission_only or path_only or management_only):
            raise ValueError("Persistent peer mode requires an admission, path or management check")
        evidence = (management_exercise() if management_only else
                    path_exercise() if path_only else admission_exercise(preserve_peer=True))
        evidence["peer_retained"] = True
        field.save("field-stock-evidence-" + str(time.time_ns()) + ".json", evidence)
        print("PASS independent RF; Cedar-Base retained on 912.525 with its stable identity "
              "and three-byte paths; no backup/restore performed", flush=True)
        return
    stock.verified_backup()
    gateway = stock.verify()
    if gateway["kiss"]["connected"] or gateway["scheduler"]["queued"]:
        raise ValueError("Authorized gateway peer has active clients/jobs")
    evidence = None
    try:
        stock.flash()
        time.sleep(5)
        evidence = (admission_exercise() if admission_only else
                    source_smoke() if smoke else exercise(https_only=True) if https_only else
                    exercise(private_only=True) if private_only else
                    exercise(extended=True) if extended else exercise())
    finally:
        try:
            stock.restore()
        except OSError as error:
            print(f"Restored gateway readback failed: {error}; attempting one reset/readback", flush=True)
            stock.check()
            stock.verify()
        if evidence is not None:
            evidence["peer_restored"] = True
            field.save("field-stock-evidence-" + str(time.time_ns()) + ".json", evidence)
    print("PASS finite real-PHY stock endpoint checks; peer restored to its original lab image", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--extended", action="store_true")
    modes.add_argument("--source-smoke", action="store_true")
    modes.add_argument("--private-only", action="store_true")
    modes.add_argument("--https-only", action="store_true")
    modes.add_argument("--admission-only", action="store_true")
    modes.add_argument("--path-only", action="store_true",
                       help="Verify the actual RF encoding of one explicit three-byte relay path")
    modes.add_argument("--management-only", action="store_true",
                       help="Stock encrypted management without mast WiFi, temporary PHY return and recovery")
    parser.add_argument("--keep-peer", action="store_true",
                        help="Use installed stock peer; preserve identity and retain its three-byte field profile")
    parser.add_argument("--lab", action="store_true",
                        help="Compatibility flag requiring kept-peer checks; all checks now use 912.525/BW250")
    args = parser.parse_args()
    main(args.extended, args.source_smoke, args.private_only, args.https_only, args.admission_only,
         args.keep_peer, args.lab, args.path_only, args.management_only)
