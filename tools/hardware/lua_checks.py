#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Finite real-LAN source/VM smoke, invoked through the onchip Make targets."""
import argparse
import hashlib
import os
import threading
import time
import urllib.error

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware.esp32_device import DIRECTORY, device_port, status
from tools.hardware.management_rf_checks import public_command
from tools.hardware.admin import checked, install
from tools.hardware.wifi_checks import native, web

from tools.hardware.inventory import value as inventory_value


def fixture(bare=False):
    if bare:
        return b"function hello(name) reply('Hello '..name) end"
    source = (Path(__file__).resolve().parents[2] / "firmware/esp32/tests/bot_large.lua").read_bytes()
    if not 3500 <= len(source) <= 4096:
        raise ValueError("Expected a substantive bounded source fixture")
    return source.ljust(4096, b" ")


def active(client, digest):
    for _ in range(40):
        state = checked(client, "source status")
        if ("durably saved and active" in state and
                checked(client, "source hash").startswith("SHA256 " + digest + " ")):
            return
        time.sleep(.25)
    raise TimeoutError("Source did not become durably saved and active: " + state)

def connect():
    for attempt in range(8):
        try:
            return web()
        except urllib.error.HTTPError as error:
            if error.code != 429 or attempt == 7:
                raise
        except (urllib.error.URLError, TimeoutError):
            if attempt == 7:
                raise
        print("Waiting for real-LAN management reachability")
        time.sleep(2)


def start_capture():
    import serial
    connection = serial.Serial()
    connection.port, connection.baudrate, connection.timeout = str(device_port()), 115200, .2
    connection.dtr = connection.rts = False
    connection.open()
    done = threading.Event()
    lines, errors = [], []

    def capture():
        pending = bytearray()
        try:
            with connection:
                while not done.is_set():
                    pending.extend(connection.read(2048))
                    while b"\n" in pending:
                        line, _, pending = pending.partition(b"\n")
                        text = line.decode("ascii", errors="replace").strip()
                        if text.startswith(("Command ", "Mast source", "On-chip command bot:")):
                            lines.append(text)
                    if len(pending) > 8192 or len(lines) > 512:
                        raise ValueError("VM serial capture exceeded its bound")
        except (OSError, ValueError) as error:
            errors.append(str(error))

    reader = threading.Thread(target=capture)
    reader.start()
    return done, reader, lines, errors


def finish_capture(capture, filename):
    done, reader, lines, errors = capture
    done.set()
    reader.join(timeout=3)
    for line in lines:
        print(line)
    (DIRECTORY / filename).write_text("\n".join(lines) + "\n")
    if reader.is_alive() or errors:
        raise RuntimeError("VM serial capture failed: " + "; ".join(errors))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expect-budget-failure", action="store_true")
    parser.add_argument("--bare", action="store_true")
    args = parser.parse_args()
    if args.bare and args.expect_budget_failure:
        parser.error("The exact bare authoring fixture must succeed")
    os.umask(0o077)
    gateway = status(inventory_value("gateway_host"))
    if gateway["kiss"]["connected"] or gateway["scheduler"]["queued"]:
        raise ValueError("Lab gateway is in use")
    source = fixture(args.bare)
    digest = hashlib.sha256(source).hexdigest()
    capture = start_capture()
    client = None
    original = None
    try:
        client = connect()
        initial = checked(client, "status")
        if "PHY=912525000,250000,7,5,2" not in initial or "temp=0" not in initial:
            raise ValueError("Refusing VM fixture outside the permanent isolated lab PHY")
        if "connected=1" not in checked(client, "wifi status"):
            raise ValueError("Mast must be connected to the existing real LAN")
        if not checked(client, "source api").startswith("API named-commands-v1 "):
            raise ValueError("Mast needs the registered-handler image")
        original = checked(client, "source hash").split()[1]
        try:
            if args.bare:
                installer = native()
                try:
                    install(installer, source, lambda _: None)
                finally:
                    installer.close()
                active(client, digest)
            else:
                install(client, source, lambda _: None)
        except (ValueError, TimeoutError) as error:
            if not args.expect_budget_failure or not any(
                    word in str(error) for word in ("budget", "deadline", "memory")):
                raise
            print("REPRODUCED substantive 4096-byte source failure: " + str(error))
        else:
            if args.expect_budget_failure:
                raise ValueError("The expected budget failure did not reproduce")
            peer = native()
            try:
                bot = checked(peer, "bot key").removeprefix("KEY ")
                public_command(peer, bot, "slepp", "hello", "Hello ")
                time.sleep(35)
                if args.bare:
                    public_command(peer, bot, "", "ping", expected="Pong")
                else:
                    public_command(peer, bot, "32", "station",
                                   expected="Fixture East 32 [AD07ah] elevation=432")
            finally:
                peer.close()
            checked(client, "reboot")
            client.close()
            client = None
            time.sleep(8)
            client = connect()
            active(client, digest)
            time.sleep(3)
            peer = native()
            try:
                if checked(peer, "bot key").removeprefix("KEY ") != bot:
                    raise ValueError("Persistent bot identity changed across reboot")
                public_command(peer, bot, "slepp", "hello", "Hello ")
            finally:
                peer.close()
            if checked(client, "status") != initial:
                raise ValueError("Roles or PHY changed during source-only VM fixture")
            label = "literal bare hello without registration" if args.bare else "4096-byte/8-handler"
            print("PASS real-LAN " + label + " installation, RF invocation and reboot persistence")
    finally:
        try:
            if original and client is None:
                client = connect()
            if client and original:
                current = checked(client, "source hash").split()[1]
                if current not in (original, digest):
                    raise ValueError("Source changed concurrently; refusing unrelated rollback")
                if "upload=" + digest[:16] in checked(client, "source status"):
                    checked(client, "source cancel")
                if current != original:
                    checked(client, "source rollback")
                    active(client, original)
                if checked(client, "source hash").split()[1] != original or "ready=1" not in checked(client, "bot status"):
                    raise ValueError("Original source/hash and live bot readiness were not restored")
                if "connected=1" not in checked(client, "wifi status"):
                    raise ValueError("Mast disconnected from the real LAN during the probe")
                print("Restored original source; real-LAN connection retained")
        finally:
            if client:
                client.close()
            finish_capture(capture, "bare-phases.log" if args.bare else "vm-phases.log")


if __name__ == "__main__":
    main()
