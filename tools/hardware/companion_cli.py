#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Short OTA acceptance through the installed, unmodified meshcli program."""
import argparse
import ast
import hashlib
import json
import os
import re
import secrets
import shlex
import shutil
import subprocess
import time
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware.esp32_device import DIRECTORY as MAST_DIRECTORY, status
from tools.hardware.admin import private_file
from tools.hardware.rf import public_hex
from tools.hardware.companion_device import DIRECTORY, device_port, check as reset_stock_peer, verified_backup

from tools.hardware.inventory import value as inventory_value


class NoResponseError(ValueError):
    """The stock client exited without a decoded response."""


def documents(output, allow_login_failure=False):
    decoder = json.JSONDecoder()
    remaining = output.strip()
    result = []
    while remaining:
        value, end = decoder.raw_decode(remaining)
        batch = value if isinstance(value, list) else [value]
        for entry in batch:
            if not isinstance(entry, dict):
                raise ValueError("Expected stock CLI JSON objects or message arrays")
            failed_login = (allow_login_failure and entry.get("login_success") is False and
                            entry.get("error") == "login failed")
            if "error" in entry and not failed_login:
                raise ValueError(f"Stock CLI reported: {entry}")
            result.append(entry)
        if len(result) > 512:
            raise ValueError("Stock CLI result budget exceeded")
        remaining = remaining[end:].lstrip()
    return result


def run_cli(label, lines, timeout=60, channel_info=False, private_script=False,
            allow_login_failure=False):
    if not device_port().is_symlink() or device_port().resolve().name == "ttyACM0":
        raise ValueError("Isolated stock-peer by-id mapping unavailable")
    executable = shutil.which("meshcli")
    if not executable:
        raise ValueError("Install/use an existing meshcore-cli; no private RF framing substitute")
    home = DIRECTORY / ("public-cli-home" if label.startswith("public-") else "cli-home")
    home.mkdir(mode=0o700, exist_ok=True)
    script = DIRECTORY / (label + ".meshcli")
    payload = ("\n".join(lines) + "\n").encode()
    descriptor = None
    if private_script:
        from firmware.esp32.https_profile import memory_header
        descriptor = memory_header(payload)
        script = Path(f"/proc/self/fd/{descriptor}")
    else:
        fd = os.open(script, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "wb") as output:
            output.write(payload)
    environment = os.environ.copy()
    environment["HOME"] = str(home)
    try:
        if descriptor is not None:
            os.lseek(descriptor, 0, os.SEEK_SET)
        completed = subprocess.run(
            [executable, "-j", "-s", str(device_port()), "script", str(script)],
            env=environment, capture_output=True, text=True, timeout=timeout, check=False,
            pass_fds=() if descriptor is None else (descriptor,))
    finally:
        if descriptor is not None:
            os.close(descriptor)
    if not private_script:
        (DIRECTORY / (label + ".stdout")).write_text(completed.stdout)
        (DIRECTORY / (label + ".stderr")).write_text(completed.stderr)
    diagnostic = "private output withheld" if private_script else f"see private {label} logs"
    if completed.returncode:
        raise ValueError(f"Stock CLI exited {completed.returncode}; {diagnostic}")
    if not completed.stdout.strip():
        raise NoResponseError(f"Stock CLI returned no response; {diagnostic}")
    try:
        if channel_info:
            lines = completed.stdout.strip().splitlines()
            if not 1 <= len(lines) <= 40:
                raise ValueError("Stock channel result count exceeds native capacity")
            return [channel_document(line) for line in lines]
        output = completed.stdout
        if allow_login_failure:
            # Stock meshcli emits this known failure as text even with -j.
            output = output.replace("Login failed : Error or Timeout waiting response\n",
                                    '{"login_success":false,"error":"login failed"}\n')
        return documents(output, allow_login_failure=allow_login_failure)
    except ValueError:
        if private_script:
            reason = ("native login failed/timed out" if "Login failed" in completed.stdout else
                      "contact unknown" if "contact unknown" in completed.stdout else
                      "private script unavailable" if "not found" in completed.stdout else
                      "invalid/mixed JSON")
            raise ValueError("Private stock CLI: " + reason + "; output withheld") from None
        raise


def channel_document(output):
    # The installed stock get_channel command prints a Python dict even with -j.
    if len(output) > 4096:
        raise ValueError("Stock channel readback exceeds its bound")
    try:
        value = ast.literal_eval(output.strip())
    except (ValueError, SyntaxError) as error:
        raise ValueError("Invalid stock channel readback") from error
    if (not isinstance(value, dict) or "error" in value or
            type(value.get("channel_idx")) is not int or
            not isinstance(value.get("channel_name"), str) or
            not isinstance(value.get("channel_secret"), str) or
            not re.fullmatch("[0-9a-fA-F]{32}", value["channel_secret"])):
        raise ValueError("Malformed stock channel readback")
    return value


def values_with(documents, key):
    return [value for value in documents if key in value]


def three_byte_relay_path(relay, target):
    if any(not re.fullmatch("[0-9a-fA-F]{64}", key) for key in (relay, target)):
        raise ValueError("Expected full native relay and target public keys")
    prefix = bytes.fromhex(relay)[:3]
    if prefix == bytes.fromhex(target)[:3]:
        raise ValueError("Ambiguous three-byte direct fixture route")
    return prefix.hex() + ":2"


def native_private_key(seed):
    # Pinned upstream lib/ed25519/keypair.c format, not seed || public key.
    private = bytearray(hashlib.sha512(seed).digest())
    private[0] &= 248
    private[31] &= 63
    private[31] |= 64
    return bytes(private)


def configure(public=False, field=False, preserve_identity=False, name=None):
    label = "public-" if public else ""
    probe = run_cli(label + "probe", ["ver", "infos", "clock"])
    version = values_with(probe, "ver")
    clock = values_with(probe, "time")
    if not version or "1.17.1" not in version[-1]["ver"] or not clock:
        raise ValueError("Stock companion version/clock readback missing")
    if clock[-1]["time"] > int(time.time()) + 120:
        raise ValueError("Stock peer RTC is in the future; power-cycle required, no rollback bypass")
    identity_commands = []
    if preserve_identity:
        info = values_with(probe, "public_key")
        if not info or not re.fullmatch("[0-9a-f]{64}", info[-1]["public_key"]):
            raise ValueError("Stable stock identity readback missing")
        public_key = info[-1]["public_key"]
    else:
        owner_seed = private_file(MAST_DIRECTORY / "companion.seed", 32)
        if len(owner_seed) != 32:
            raise ValueError("Disposable companion seed must be 32 bytes")
        seed = secrets.token_bytes(32) if public else owner_seed
        public_key = public_hex(seed)
        identity_commands.append("set private_key " + native_private_key(seed).hex())
    before = json.loads((MAST_DIRECTORY / "before.json").read_text())
    management = next(role["public_key"] for role in before["roles"] if role["role"] == "management")
    previous = json.loads((MAST_DIRECTORY / "rf-results.json").read_text())
    bot = previous["bot_key"]
    if public and public_key in (management, bot):
        raise ValueError("Public fixture must have an unrelated identity")
    if public and not preserve_identity and public_key == public_hex(owner_seed):
        raise ValueError("Public fixture must not reuse the owner identity")
    contacts = [f"add_contact {bot} 1 beta-bot"]
    if not public:
        contacts.insert(0, f"add_contact {management} 2 beta-mast")
    frequency, bandwidth = (910.525, 62.5) if field else (912.525, 250)
    run_cli(label + "configure", [
        *identity_commands,
        "set name " + shlex.quote(name or ("stock-public-fixture" if public else "stock-beta-fixture")),
        "set path_hash_mode 2",
        f"set radio {frequency},{bandwidth},7,5",
        "set tx 2",
        "clock sync",
        *contacts,
        *(["reset_path beta-mast"] if not public else []),
        "reset_path beta-bot",
    ])
    readback = run_cli(label + "readback", ["ver", "infos", "clock"])
    info = values_with(readback, "public_key")
    if not info or info[-1]["public_key"] != public_key:
        raise ValueError("Stock peer identity readback mismatch")
    modes = values_with(readback, "path_hash_mode")
    if not modes or modes[-1]["path_hash_mode"] != 2:
        raise ValueError("Stock peer three-byte originated paths not confirmed")
    actual = info[-1]
    if (actual.get("radio_freq") != frequency or actual.get("radio_bw") != bandwidth or
            actual.get("radio_sf") != 7 or actual.get("radio_cr") != 5 or
            actual.get("tx_power") != 2):
        raise ValueError(f"Stock peer PHY readback mismatch: {actual}")
    clocks = values_with(readback, "time")
    if not clocks or abs(clocks[-1]["time"] - int(time.time())) > 120:
        raise ValueError("Stock peer RTC synchronization was not confirmed")
    (DIRECTORY / (label + "stock-info.json")).write_text(json.dumps(readback, indent=2) + "\n")
    return management, bot, public_key


def public_commands():
    # Keep the normal local query as part of the stock USB receive workaround.
    return ["sync_msgs", "msgs_subscribe", "advert", "sleep 1", "msg beta-bot '!ping'",
            "sleep 1", "ver", "sleep 2", "sleep 31", "msg beta-bot '!test'",
            "sleep 1", "ver", "sleep 2"]


def has_radio_diagnostic(messages):
    for entry in messages:
        match = re.search(r"RSSI=(-?\d+(?:\.\d+)?) dBm SNR=(-?\d+(?:\.\d+)?) dB",
                          entry.get("text", ""))
        if match and -160 <= float(match[1]) <= 0 and -30 <= float(match[2]) <= 30:
            return True
    return False


def public_replies(replies, bot):
    if any("login_success" in entry for entry in replies):
        raise ValueError("Public-user acceptance must not use an admin login")
    messages = [entry for entry in replies
                if entry.get("type") == "PRIV" and entry.get("pubkey_prefix") == bot[:12]]
    if any(entry.get("txt_type") != 0 for entry in messages):
        raise ValueError("Public replies must use ordinary native chat text, not private CLI text")
    if not any(entry.get("text") == "Pong" for entry in messages):
        raise ValueError("Unrelated stock public user did not receive Pong")
    if not has_radio_diagnostic(messages):
        raise ValueError("Unrelated stock public user did not receive RF !test diagnostics")
    if any("SNR" not in entry for entry in messages):
        raise ValueError("Public stock native RF receive metadata missing")
    return messages


def public_bot_test():
    _, bot, public_key = configure(public=True)
    messages = public_replies(run_cli("public-ota", public_commands(), timeout=90), bot)
    evidence = {"stock_companion": "1.17.1", "bot_key": bot, "messages": messages,
                "public_user_key": public_key, "admin_login": False,
                "usb_receive": "stock msgs_subscribe with local ver query",
                "scope": "Unrelated disposable stock public user; normal contact/advert/msg only"}
    (DIRECTORY / "public-results.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print("PASS stock public user: unrelated identity, no admin login, OTA Pong and !test")


def read_only_status():
    label = "public-status-" + str(time.time_ns())
    rows = run_cli(label, ["ver", "infos"])
    versions, identities = values_with(rows, "path_hash_mode"), values_with(rows, "public_key")
    if not versions or not identities:
        raise ValueError("Native stock version/identity readback missing")
    mode, info = versions[-1]["path_hash_mode"], identities[-1]
    if type(mode) is not int or not 0 <= mode <= 2:
        raise ValueError("Invalid native stock path-hash mode")
    return {"name": info["name"], "public_key": info["public_key"],
            "native_path_hash_mode": mode, "path_hash_bytes": mode + 1,
            "phy": {key: info[key] for key in
                    ("radio_freq", "radio_bw", "radio_sf", "radio_cr", "tx_power")},
            "read_only": True, "log_prefix": label}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--public-only", action="store_true")
    parser.add_argument("--expected-roles", type=int, choices=(0, 7), default=0)
    parser.add_argument("--local-relay", action="store_true")
    parser.add_argument("--status", action="store_true",
                        help="Read native identity, PHY and path mode only; no reset or provisioning")
    args = parser.parse_args()
    os.umask(0o077)
    if args.status:
        print(json.dumps(read_only_status(), indent=2))
        return
    verified_backup()
    public_bot_test()
    if args.public_only:
        return
    # Clear the prior USB subscription before changing the physical fixture's identity.
    reset_stock_peer()
    management, bot, owner_key = configure()
    source = b"function hello(name) reply('Hello '..name) end"
    digest = hashlib.sha256(source).hexdigest()
    identifier = digest[:16]
    password = private_file(MAST_DIRECTORY / "password", 15).decode()
    nonce = secrets.token_hex(4)
    expected = {}
    lines = ["sync_msgs", 'login beta-mast ""', "sleep 1"]
    # The supported short tag avoids the observed stock USB 64-byte ACK boundary.
    tag = secrets.randbits(8)

    def command(text, expected_text=None):
        nonlocal tag
        prefix = f"{tag:02x}|"
        tag = (tag + 1) & 255
        if len(prefix + text) > 162:
            raise ValueError("Test command exceeds native text capacity")
        lines.extend(["cmd beta-mast " + shlex.quote(prefix + text), "sleep 2", "sync_msgs"])
        if expected_text is not None:
            expected[prefix] = expected_text
        return prefix

    initial_tag = command("status", f"roles applied={args.expected_roles} saved={args.expected_roles}")
    lines += ["sleep 1", "login beta-mast " + shlex.quote(password), "sleep 1"]
    command("bot status", "bot applied=1 saved=1 ready=1")
    lines += ["advert", "sleep 1"]
    command(f"source begin {identifier} {len(source)} {digest}", f"ACK {identifier} next=0")
    total = (len(source) + 47) // 48
    for index in range(total):
        command(f"source chunk {identifier} {index} {source[index * 48:(index + 1) * 48].hex()}",
                f"ACK {identifier} next={index + 1}")
    command(f"source commit {identifier}", "Accepted verification")
    lines += ["sleep 2"]
    active_tag = command("source status")
    installed_hash_tag = command("source hash", "SHA256 " + digest)
    command("source help Installed through unmodified stock meshcli", "Saved source help")
    lines += ["msg beta-bot " + shlex.quote("!hello " + nonce), "sleep 2", "sync_msgs"]
    replies = run_cli("ota", lines, timeout=300)
    if not any(entry.get("type") == "PRIV" and entry.get("pubkey_prefix") == management[:12] and
               entry.get("text", "").startswith(active_tag) and
               "durably saved and active" in entry.get("text", "") for entry in replies):
        print("Stock activation readback missing; retrying source status once", flush=True)
        replies += run_cli("ota-status-retry", [
            "msgs_subscribe", "cmd beta-mast " + shlex.quote(active_tag + "source status"),
            "sleep 1", "ver", "sleep 2", "sync_msgs",
        ])
    if not any(entry.get("type") == "PRIV" and entry.get("pubkey_prefix") == management[:12] and
               entry.get("text", "").startswith(installed_hash_tag + "SHA256 " + digest)
               for entry in replies):
        print("Stock source-hash receipt missing; retrying the read once before rollback", flush=True)
        replies += run_cli("ota-hash-retry", [
            "msgs_subscribe", "cmd beta-mast " + shlex.quote(installed_hash_tag + "source hash"),
            "sleep 1", "ver", "sleep 2", "sync_msgs",
        ])
    lines = []
    command("source rollback", "Accepted verification")
    lines += ["sleep 2"]
    rollback_tag = command("source status")
    command("source hash", "SHA256 " + hashlib.sha256(
        (Path(__file__).resolve().parents[2] / "firmware/runtime/BotTypes.cpp").read_text().split('R"lua(', 1)[1].split(')lua"', 1)[0].encode()).hexdigest())
    replies += run_cli("ota-rollback", lines, timeout=60)
    if not any(entry.get("type") == "PRIV" and entry.get("pubkey_prefix") == management[:12] and
               entry.get("text", "").startswith(rollback_tag) and
               "durably saved and active" in entry.get("text", "") for entry in replies):
        print("Stock rollback readback missing; retrying source status once", flush=True)
        replies += run_cli("ota-rollback-status-retry", [
            "msgs_subscribe", "cmd beta-mast " + shlex.quote(rollback_tag + "source status"),
            "sleep 1", "ver", "sleep 2", "sync_msgs",
        ])
    messages = [entry for entry in replies if entry.get("type") == "PRIV"]
    if sum(entry.get("login_success") is True for entry in replies) != 2:
        raise ValueError("Both native trusted-key and password login must succeed")
    for prefix, text in expected.items():
        matches = [entry for entry in messages
                   if entry.get("pubkey_prefix") == management[:12] and entry.get("text", "").startswith(prefix)]
        if not matches or any(not entry["text"][len(prefix):].startswith(text) for entry in matches):
            raise ValueError(f"Missing/mismatched authenticated stock CLI reply for {prefix}")
    for prefix in (active_tag, rollback_tag):
        if not any(entry.get("pubkey_prefix") == management[:12] and
                   entry.get("text", "").startswith(prefix) and
                   "source durably saved and active" in entry["text"] for entry in messages):
            raise ValueError("Source activation/rollback not confirmed through stock peer")
    bot_messages = [entry for entry in messages if entry.get("pubkey_prefix") == bot[:12]]
    if not any(entry.get("text") == "Hello " + nonce for entry in bot_messages):
        raise ValueError("No installed command reply received by stock peer")
    if any("SNR" not in entry for entry in messages):
        raise ValueError("Stock native RF receive metadata missing")
    evidence = {"stock_companion": "1.17.1", "management_key": management, "owner_key": owner_key,
                "bot_key": bot, "source_sha256": digest, "chunks": total,
                "messages": messages, "status_tag": initial_tag,
                "scope": "Unmodified meshcli -> stock USB companion -> 912.525 MHz LoRa -> beta mast"}
    (DIRECTORY / "results.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"PASS stock MeshCore 1.17.1 owner OTA: both logins, controls, {total} durable source chunks, Lua reply and rollback")
    if args.local_relay:
        state = status(inventory_value("mast_host"))
        relay = next(role["public_key"] for role in state["roles"] if role["role"] == "repeater" and role["ready"])
        path = three_byte_relay_path(relay, bot)
        routed = run_cli("local-relay", [
            "sync_msgs", "msgs_subscribe", "sleep 31",
            "change_path beta-bot " + path,
            "msg beta-bot '!test'", "sleep 1", "ver", "sleep 3",
            "reset_path beta-bot",
        ], timeout=60)
        messages = [item for item in routed if item.get("type") == "PRIV" and
                    item.get("pubkey_prefix") == bot[:12]]
        if not any("local; RSSI/SNR unavailable" in item.get("text", "") for item in messages):
            raise ValueError("Stock request did not traverse the selected local repeater role")
        print("PASS stock three-byte direct route: RF to mast repeater, local delivery to distinct bot, RF reply; not a third-radio path")


if __name__ == "__main__":
    main()
