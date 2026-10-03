#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Short, bounded RF beta acceptance using only the isolated mast/gateway."""
import hashlib
import argparse
import json
import secrets
import socket
import struct
import time
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware.esp32_device import DIRECTORY, status
from tools.hardware.admin import NativeClient, WebClient, checked, decode, encrypt, install, private_file
from tools.hardware.rf import _shared_secret, kiss, query_with_context, frame_with_context, wait_receipt, verify_status

from tools.hardware.inventory import value as inventory_value


def native(target, password=""):
    client = NativeClient(inventory_value("gateway_host"), 8001, DIRECTORY / "companion.seed", target, password, 12)
    time.sleep(.5)
    return client


def reconnect(target, power=2):
    last = None
    for _ in range(5):
        time.sleep(2)
        try:
            client = native(target)
            if f"PHY=912525000,250000,7,5,{power}" not in checked(client, "status"):
                client.close()
                raise ValueError("Mast is not on the expected isolated PHY")
            return client
        except (OSError, ValueError) as error:
            last = error
    raise RuntimeError(f"Isolated mast did not return after reboot: {last}")


def config_get():
    with socket.create_connection((inventory_value("mast_host"), 8001), timeout=3) as connection:
        connection.sendall(b"\xc0\x06\x20\x01\x00\xc0\xc0\x06\x22\x01\x00\xc0")
        until = time.monotonic() + 3
        frame, escaped = bytearray(), False
        while time.monotonic() < until:
            connection.settimeout(max(.01, until - time.monotonic()))
            incoming = connection.recv(256)
            if not incoming:
                raise ValueError("Mast KISS disconnected")
            for byte in incoming:
                if byte == 0xc0:
                    if len(frame) == 26 and frame[:4] == b"\x06\xa2\x01\x00":
                        return {"generation": struct.unpack_from("<I", frame, 4)[0],
                                "frequency_hz": struct.unpack_from("<I", frame, 8)[0],
                                "sf": frame[16], "power": frame[18]}
                    frame, escaped = bytearray(), False
                elif escaped:
                    if byte not in (0xdc, 0xdd):
                        raise ValueError("Invalid KISS escape")
                    frame.append(0xc0 if byte == 0xdc else 0xdb)
                    escaped = False
                elif byte == 0xdb:
                    escaped = True
                elif len(frame) < 256:
                    frame.append(byte)
                else:
                    raise ValueError("Oversized KISS frame")
        raise TimeoutError("No CONFIG GET reply")


def public_request(client, bot, text):
    key = bytes.fromhex(bot)
    shared = _shared_secret(client.key, key)
    stamp = max(client.timestamp + 1, int(time.time()))
    app = b"\x81beta-fixture-companion"
    message = client.public + struct.pack("<I", stamp) + app
    advert = b"\x11\x80" + client.public + struct.pack("<I", stamp) + client.key.sign(message) + app
    text = text.encode("ascii")
    if len(text) > 162 or any(byte < 32 or byte > 126 for byte in text):
        raise ValueError("Public command exceeds native printable text capacity")
    packet = (b"\x09\x80" + bytes((key[0], client.public[0])) +
              encrypt(shared, struct.pack("<I", stamp + 1) + b"\x00" + text))
    return key, shared, advert, packet, stamp


def public_command(client, bot, argument, name="beta", prefix="ESP32 ", expected=None):
    key, shared, advert, packet, _ = public_request(client, bot, "!" + name + " " + argument)
    if expected is None:
        expected = prefix + argument
    with socket.create_connection((inventory_value("gateway_host"), 8001), timeout=5) as connection:
        connection.sendall(kiss(advert))
        time.sleep(1)
        connection.sendall(kiss(packet))

        def verifier(raw, _):
            received = decode(shared, client.public, key, raw)
            if received and received[0] == 2 and received[1][4] == 0:
                result = received[1][5:].split(b"\0", 1)[0].decode("ascii")
                if result == expected:
                    return result
                raise ValueError("Unexpected authenticated bot reply: " + result)
            return None

        return wait_receipt(connection, None, 12, verifier, "bot reply")


def public_exchange(client, bot):
    key, shared, advert, packet, stamp = public_request(client, bot, "!exchange")
    answered = False
    with socket.create_connection((inventory_value("gateway_host"), 8001), timeout=5) as connection:
        connection.sendall(kiss(advert))
        time.sleep(1)
        connection.sendall(kiss(packet))

        def verifier(raw, _):
            nonlocal answered
            received = decode(shared, client.public, key, raw)
            if not received or received[0] != 2 or received[1][4] != 0:
                return None
            body = received[1]
            text = body[5:].split(b"\0", 1)[0]
            if text == b"question" and not answered:
                answered = True
                ack = b"\x0e\x80" + hashlib.sha256(body[:5] + text + key).digest()[:4]
                reply = (b"\x09\x80" + bytes((key[0], client.public[0])) +
                         encrypt(shared, struct.pack("<I", stamp + 2) + b"\x00answer:field"))
                connection.sendall(kiss(ack) + kiss(reply))
            elif text == b"answer:field" and answered:
                return text.decode("ascii")
            return None

        return wait_receipt(connection, None, 15, verifier, "bot coroutine exchange")


def signed_status(target):
    packet, context = query_with_context(private_file(DIRECTORY / "operator.seed", 32),
                                         bytes.fromhex(target), secrets.randbits(63) + 1)
    with socket.create_connection((inventory_value("gateway_host"), 8001), timeout=5) as connection:
        connection.sendall(kiss(packet))
        return wait_receipt(connection, context, 12, verify_status, "signed status")

def hello_smoke():
    status(inventory_value("gateway_host"))
    before = json.loads((DIRECTORY / "before.json").read_text())
    target = next(role["public_key"] for role in before["roles"] if role["role"] == "management")
    client = native(target)
    try:
        if "PHY=912525000,250000,7,5,2" not in checked(client, "status"):
            raise ValueError("Refusing hello smoke outside the lab PHY")
        bot = checked(client, "bot key").removeprefix("KEY ")
        if public_command(client, bot, "slepp", "hello", "Hello ") != "Hello slepp":
            raise ValueError("Installed hello did not return the expected result")
        print("PASS physical installed hello: independent gateway RF -> Lua hello('slepp') -> encrypted Hello slepp")
    finally:
        client.close()


def main():
    status(inventory_value("gateway_host"))
    before = json.loads((DIRECTORY / "before.json").read_text())
    target = next(role["public_key"] for role in before["roles"] if role["role"] == "management")
    evidence = {}
    client = native(target)
    try:
        evidence["initial"] = checked(client, "status")
        if "PHY=912525000,250000,7,5,2" not in evidence["initial"]:
            raise ValueError("Refusing to continue outside isolated PHY")
        print("PASS physical native trusted-full-key login and CLI", flush=True)
        client.close()
        time.sleep(1.1)
        client = native(target, private_file(DIRECTORY / "password", 15).decode())
        evidence["password_login"] = checked(client, "status")
        print("PASS physical native password login", flush=True)
        checked(client, "roles 0")
        checked(client, "bot on")
        checked(client, "apply")
        client.close()
        client = reconnect(target)
        evidence["independent"] = checked(client, "status")
        if "roles applied=0 saved=0" not in evidence["independent"]:
            raise ValueError("Roles did not apply on reboot")
        print("PASS mast and bot without repeater/room/companion", flush=True)
        signed = signed_status(target)
        if not signed["valid"] or signed["applied_mask"] != 0 or signed["saved_mask"] != 0:
            raise ValueError("Signed role status did not match shared backend")
        time.sleep(1.1)
        packet, context = frame_with_context(
            private_file(DIRECTORY / "operator.seed", 32), bytes.fromhex(target),
            signed["profile_generation"] + 1, secrets.randbits(63) + 1, 0)
        with socket.create_connection((inventory_value("gateway_host"), 8001), timeout=5) as connection:
            connection.sendall(kiss(packet))
            if wait_receipt(connection, context, 12) != 1:
                raise ValueError("Signed role update failed")
        evidence["signed_status"] = signed
        print("PASS physical existing signed role query/update/receipt", flush=True)
        source_text = ("function beta(text) reply('ESP32 '..text) end\n"
                       "command('beta','text:text:150','Physical smoke')\n"
                       "function hello(name) reply('Hello '..name) end\n")
        source = source_text.encode()
        (DIRECTORY / "handlers.lua").write_bytes(source)
        evidence["install"] = install(client, source, lambda message: print(message, flush=True))
        checked(client, "source help Isolated beta fixture; native diagnostics retained")
        key = checked(client, "bot key").removeprefix("KEY ")
        evidence["bot_key"] = key
        argument = "before-" + secrets.token_hex(4)
        evidence["bot_before"] = public_command(client, key, argument)
        print("PASS physical uploaded Lua command reply", flush=True)
        evidence["wifi_before"] = checked(client, "wifi status")
        web_outcome = "not reachable; RF administration remains available"
        try:
            web = WebClient("http://" + inventory_value("mast_host") + '', private_file(DIRECTORY / "password", 15).decode())
            try:
                evidence["web_status"] = checked(web, "status")
                evidence["web_source"] = checked(web, "source hash")
                web_outcome = "authenticated HTTP status/source hash succeeded"
            finally:
                web.close()
        except OSError as error:
            web_outcome = f"HTTP unavailable: {error}"
        evidence["web"] = web_outcome
        print(web_outcome, flush=True)
        try:
            initial_config = config_get()
        except OSError as error:
            initial_config = None
            evidence["config_get"] = f"Mast TCP unavailable: {error}"
        checked(client, "tempradio 2 912525000 250000 8 5 2")
        time.sleep(.7)
        if initial_config:
            changed_config = config_get()
            if changed_config["sf"] != 8 or changed_config["generation"] != initial_config["generation"] + 1:
                raise ValueError("CONFIG GET did not expose temporary PHY/generation")
        time.sleep(2.5)
        evidence["temporary_restore"] = checked(client, "status")
        if "PHY=912525000,250000,7,5,2" not in evidence["temporary_restore"] or "temp=0" not in evidence["temporary_restore"]:
            raise ValueError("Automatic PHY restore failed")
        if initial_config:
            restored_config = config_get()
            if restored_config["sf"] != 7 or restored_config["generation"] != initial_config["generation"] + 2:
                raise ValueError("CONFIG GET did not expose restored PHY/generation")
            evidence["config_get"] = [initial_config, changed_config, restored_config]
        checked(client, "radio 912525000 250000 7 5 1")
        time.sleep(.7)
        checked(client, "reboot")
        client.close()
        client = reconnect(target, power=1)
        evidence["persistent_phy"] = checked(client, "status")
        checked(client, "radio 912525000 250000 7 5 2")
        time.sleep(.7)
        print("PASS temporary PHY return and persistent PHY across reboot", flush=True)
        checked(client, "wifi ssid " + b"meshcore-beta-fixture-only".hex())
        checked(client, "wifi password " + private_file(DIRECTORY / "password", 15).hex())
        checked(client, "reboot")
        client.close()
        client = reconnect(target)
        evidence["wifi_after_reboot"] = checked(client, "wifi status")
        if "saved=1" not in evidence["wifi_after_reboot"] or "connected=0" not in evidence["wifi_after_reboot"]:
            raise ValueError("Expected durable isolated fixture WiFi configuration while offline")
        evidence["source_after_reboot"] = checked(client, "source hash")
        if not evidence["source_after_reboot"].startswith("SHA256 " + hashlib.sha256(source).hexdigest()):
            raise ValueError("Installed source did not survive reboot")
        key = checked(client, "bot key").removeprefix("KEY ")
        if key != evidence["bot_key"]:
            raise ValueError("Bot identity changed across reboot")
        evidence["bot_after"] = public_command(client, key, "after-" + secrets.token_hex(4))
        print("PASS durable source and WiFi settings; RF works with WiFi unavailable", flush=True)
        checked(client, "wifi forget")
        checked(client, "source rollback")
        for _ in range(10):
            time.sleep(.2)
            result = checked(client, "source status")
            if "durably saved and active" in result:
                break
        else:
            raise ValueError("Rollback did not become active")
        evidence["rollback"] = result
        checked(client, "reboot")
        client.close()
        client = reconnect(target)
        evidence["final_status"] = checked(client, "status")
        evidence["final_wifi"] = checked(client, "wifi status")
        evidence["final_source"] = checked(client, "source hash")
        bundled = (Path(__file__).resolve().parents[2] / "firmware/runtime/BotTypes.cpp").read_text().split('R"lua(', 1)[1].split(')lua"', 1)[0]
        if ("saved=0" not in evidence["final_wifi"] or
                not evidence["final_source"].startswith("SHA256 " + hashlib.sha256(bundled.encode()).hexdigest())):
            raise ValueError("Final bundled source/WiFi cleanup did not persist")
        print("PASS rollback and final reboot: bundled source, lab PHY, no fixture WiFi override", flush=True)
    finally:
        client.close()
        (DIRECTORY / "rf-results.json").write_text(json.dumps(evidence, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hello", action="store_true")
    args = parser.parse_args()
    hello_smoke() if args.hello else main()
