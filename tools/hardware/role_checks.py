#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Finite multirole stock-RF acceptance with restoration, invoked by Make."""
import json
import os
import subprocess
import time
from urllib.error import HTTPError, URLError

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware.esp32_device import ROOT, status, DIRECTORY
from tools.hardware.admin import checked
from tools.hardware.wifi_checks import web, config_get

from tools.hardware.inventory import value as inventory_value


def connect():
    for attempt in range(12):
        try:
            return web()
        except HTTPError as error:
            if error.code != 429 or attempt == 11:
                raise
            time.sleep(1.1)
        except (OSError, URLError):
            if attempt == 11:
                raise
            time.sleep(2)


def select(mask):
    client = connect()
    try:
        checked(client, "roles " + str(mask))
        checked(client, "reboot")
    finally:
        client.close()
    time.sleep(8)
    client = connect()
    try:
        result = checked(client, "status")
        if f"roles applied={mask} saved={mask}" not in result or "temp=0" not in result:
            raise ValueError("Role/PHY restoration mismatch: " + result)
    finally:
        client.close()


def main():
    os.umask(0o077)
    client = connect()
    try:
        initial = checked(client, "status")
        if "roles applied=0 saved=0" not in initial or "PHY=912525000,250000,7,5,2" not in initial:
            raise ValueError("Expected baseline lab role mask and PHY")
        source = checked(client, "source hash").split()[1]
        if "connected=1" not in checked(client, "wifi status"):
            raise ValueError("Real LAN is required")
    finally:
        client.close()
    before = status(inventory_value("mast_host"))
    gateway = status(inventory_value("gateway_host"))
    if gateway["kiss"]["connected"] or gateway["scheduler"]["queued"]:
        raise ValueError("Gateway has active work")
    flashed = False
    gatewayRestored = False
    try:
        select(7)
        state = status(inventory_value("mast_host"))
        for name in ("repeater", "room", "companion", "management", "command-bot"):
            role = next(role for role in state["roles"] if role["role"] == name)
            if not role["ready"] or not role.get("public_key"):
                raise ValueError("Role is not ready: " + name)
        keys = [role["public_key"] for role in state["roles"] if role.get("public_key")]
        if len(set(keys)) != len(keys):
            raise ValueError("Role identities are not distinct")
        if config_get()["frequency_hz"] != 912525000:
            raise ValueError("KISS effective PHY mismatch")
        print("PASS real-LAN repeater/room/companion/management/bot ready with distinct keys; KISS CONFIG responds", flush=True)
        flashed = True
        subprocess.run(["make", "-s", "-C", str(ROOT / "firmware/esp32"),
                        "beta-stock-flash"], check=True)
        time.sleep(5)
        subprocess.run(["make", "-s", "-C", str(ROOT / "firmware/esp32"),
                        "beta-stock-multirole-test"], check=True)
    finally:
        try:
            if flashed:
                subprocess.run(["make", "-s", "-C", str(ROOT / "firmware/esp32"),
                                "beta-stock-restore"], check=True)
                gatewayRestored = True
        finally:
            select(0)
            client = connect()
            try:
                if checked(client, "source hash").split()[1] != source:
                    raise ValueError("Multirole pass did not restore the original source")
                if "connected=1" not in checked(client, "wifi status") or "ready=1" not in checked(client, "bot status"):
                    raise ValueError("Restored bot/LAN is not ready")
            finally:
                client.close()
            after = status(inventory_value("mast_host"))
            for old in before["roles"]:
                if old.get("public_key"):
                    current = next(role for role in after["roles"] if role["role"] == old["role"])
                    if current.get("public_key") != old["public_key"]:
                        raise ValueError("Role identity changed: " + old["role"])
            (DIRECTORY / "multirole-restored.json").write_text(json.dumps(after) + "\n")
            print("Restored mast role mask 0, original source/identities and real LAN; " +
                  ("gateway full backup restored" if gatewayRestored else
                   "gateway restoration failed; inspect private restore log" if flashed else
                   "gateway was not changed"), flush=True)


if __name__ == "__main__":
    main()
