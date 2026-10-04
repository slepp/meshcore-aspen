#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Finite retained-VM/KV/timer acceptance on the authorized lab mast."""
import hashlib
import os
import secrets
import time

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.hardware.esp32_device import status
from tools.hardware.management_rf_checks import public_command, public_exchange
from tools.hardware.admin import checked, install
from tools.hardware.lua_checks import active, connect, start_capture, finish_capture
from tools.hardware.wifi_checks import native

from tools.hardware.inventory import value as inventory_value


def exercise(check_gateway=True):
    if check_gateway:
        gateway = status(inventory_value("gateway_host"))
        if gateway["kiss"]["connected"] or gateway["scheduler"]["queued"]:
            raise ValueError("Lab gateway is in use")
    key = "field-" + secrets.token_hex(4)
    source = f"""
counter=0
function remember(note) kv.put('{key}',note) return 'committed' end
command('remember','note:text:80','Remember test record')
function list_memories() return kv.get('{key}') or 'missing' end
command('list-memories','','Read test record','list_memories')
function nap() counter=counter+1 timer.sleep(1000) return ping()..' #'..tostring(counter) end
function cleanup() kv.delete('{key}') return 'deleted' end
function exchange()
  local tx=mesh.send(mesh.compose{{kind='dm',text='question'}})
  if not tx.queued or not tx.transmitted then return 'bad tx' end
  local ack=mesh.wait{{kind='ack',timeout_ms=5000}}
  if not ack.acknowledged then return 'bad ack' end
  local packet=mesh.wait{{kind='text',prefix='answer:',timeout_ms=5000}}
  if not packet.authenticated then return 'bad sender' end
  return packet.text
end
""".encode("ascii")
    client = connect()
    original = checked(client, "source hash").split()[1]
    bot = checked(client, "bot key").removeprefix("KEY ")
    record_attempted = False

    def command(name, expected):
        peer = native()
        try:
            return public_command(peer, bot, "", name, expected=expected)
        finally:
            peer.close()

    try:
        if "mesh=dm,wait" not in checked(client, "source api"):
            raise ValueError("Retained-runtime image is not installed")
        install(client, source, lambda _: None)
        peer = native()
        try:
            record_attempted = True
            public_command(peer, bot, "get groceries", "remember", expected="committed")
        finally:
            peer.close()
        time.sleep(35)
        command("nap", "Pong #1")
        time.sleep(35)
        command("nap", "Pong #2")
        time.sleep(35)
        peer = native()
        try:
            if public_exchange(peer, bot) != "answer:field":
                raise ValueError("Native coroutine exchange failed")
        finally:
            peer.close()
        checked(client, "reboot")
        client.close()
        client = None
        time.sleep(8)
        client = connect()
        active(client, hashlib.sha256(source).hexdigest())
        if checked(client, "bot key").removeprefix("KEY ") != bot:
            raise ValueError("Persistent bot identity changed")
        command("list-memories", "get groceries")
        print("PASS physical RF: shared globals across commands, nested timer/ping, "
              "committed private KV, native DM/TX/ACK/filtered wait and source reload across ESP32 restart")
    finally:
        if client is None:
            client = connect()
        try:
            try:
                if record_attempted:
                    time.sleep(35)
                    command("cleanup", "deleted")
                    print("Removed only the unique test KV key")
            finally:
                checked(client, "source cancel")
                if checked(client, "source hash").split()[1] != original:
                    checked(client, "source rollback")
                    active(client, original)
                if checked(client, "source hash").split()[1] != original:
                    raise ValueError("Original source was not restored")
                if "ready=1" not in checked(client, "bot status"):
                    raise ValueError("Restored command bot is not ready")
                if "connected=1" not in checked(client, "wifi status"):
                    raise ValueError("Real LAN connection was not retained")
                print("Restored original source and retained LAN/identity")
        finally:
            client.close()


def main():
    os.umask(0o077)
    gateway = status(inventory_value("gateway_host"))
    if gateway["kiss"]["connected"] or gateway["scheduler"]["queued"]:
        raise ValueError("Lab gateway is in use")
    capture = start_capture()
    try:
        exercise(check_gateway=False)
    finally:
        finish_capture(capture, "retained-runtime-phases.log")


if __name__ == "__main__":
    main()
