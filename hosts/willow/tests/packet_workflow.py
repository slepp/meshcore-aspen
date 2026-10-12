#!/usr/bin/env python3
"""Install the shared sampler through local admin and receive loopback RF copies."""
import hashlib
import json
import os
import struct
import subprocess
import sys
import time
import unittest
import zlib

from parity import ROOT, packet, parse, public
from service_demo import RunningService, advert, addressed, body
from willow import owner_rpc
from migrate_go import native_records
sys.path.insert(0, str(ROOT.parents[1]))
from tools.hardware import admin, lua_sources

CLI = ROOT.parents[1] / "tools/hardware/admin.py"


class PacketWorkflow(unittest.TestCase):
    def until(self, callback, timeout=12):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            value = callback()
            if value:
                return value
            time.sleep(.05)
        self.fail("packet sampler condition timed out")

    def test_shared_sampler_commands_and_restart(self):
        for binary in ("hew-host", "hew-host-release"):
            with self.subTest(binary=binary):
                os.environ["MESHCORE_HEW_HOST"] = str(ROOT / "build" / binary)
                with RunningService() as service:
                    endpoint = service.root / "admin.sock"
                    self.until(endpoint.exists)
                    client = admin.WillowClient(endpoint)
                    key = client.command("bot key")
                    keys = {p.name: p.read_bytes() for p in service.root.glob("*.seed")}
                    original_wasm = client.command("source wasm hash")
                    grants = client.command("bot policy")

                    def cli(*arguments):
                        run = subprocess.run([sys.executable, "-B", str(CLI), "--willow-socket", str(endpoint),
                            *map(str, arguments)], text=True, capture_output=True, timeout=30)
                        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                        return run.stdout

                    def reports():
                        value = json.loads(owner_rpc(service.root, b"\x01\x06\x00"))
                        return value["bot"].get("packet_reports", [])

                    retained = service.root / "retained.lua"
                    retained.write_text("function retained() return 'retained' end\ncommand('retained','','Retained fixture')\n")
                    cli("source", "install", "retained", retained)
                    original = admin.download(client)
                    cli("source", "install", "snr", ROOT.parents[1] / "firmware/runtime/plugins/examples/snr.lua")
                    source = admin.download(client)
                    for name, contents in lua_sources.decode(original).items():
                        self.assertEqual(lua_sources.decode(source)[name], contents)
                    module = (ROOT / "build/repeater-snr.wasm").read_bytes()
                    cli("packet", "install", "0", ROOT / "build/repeater-snr.wasm")
                    cli("packet", "budget", "0", "1", "100000", "20000", "9")
                    cli("packet", "enable", "0", "on")
                    self.assertEqual(admin.download_packet(client, 0), module)
                    self.assertIn("saved=1 live=1", client.command("packet 0 status"))
                    self.assertEqual(client.command("source wasm hash"), original_wasm)
                    self.assertEqual(client.command("bot policy"), grants)

                    # Same payload arrives once on the bot port. Other logical ports do not duplicate it.
                    def receive(path, *, route=1, metadata=b"\xf9\x18\xb0", marker=0):
                        raw = packet(0, b"ACK" + bytes([marker]), route=route, width=3, path=path)
                        service.emulator.send(2, 0, raw, metadata=metadata)
                    receive(b"", route=2, marker=1)  # Direct route has no measured repeater hop.
                    receive(b"\xaa\xbb\xcc", metadata=b"\xf9\x80\x7f", marker=2)  # Local reflection.
                    receive(b"\x11\x22\x33", metadata=None, marker=3)  # Unmeasured remote.
                    receive(b"\xd1\x26\x00", marker=4)
                    sampled = self.until(lambda: reports() or None)
                    self.assertEqual(len(sampled), 1)
                    report = sampled[0]
                    self.assertEqual(report["name"], "repeater_snr")
                    self.assertEqual([r["label"] for r in report["rows"]], ["D12600"])
                    row = dict(zip(report["fields"], report["rows"][0]["values"]))
                    self.assertEqual(row["samples"], 1)
                    self.assertEqual(row["mean_db"], 6)
                    self.assertEqual(row["rssi_mean_dbm"], -80)
                    self.assertGreater(row["last_seen_utc"], 0)

                    # The Lua command reads the exact same native report; case-insensitive prefixes work.
                    service.emulator.send(2, 0, packet(4, parse(advert(2))[4], route=2, width=3))
                    plain = struct.pack("<IB", int(time.time()), 0) + b"!snr d1"
                    service.emulator.send(2, 0, addressed(2, 5, 2, plain, route=2, width=3))
                    try:
                        reply = service.emulator.receive(lambda job: job["port"] == 2 and parse(job["raw"])[0] == 2)
                    except AssertionError as error:
                        raise AssertionError(f"{error}; status={client.command('bot diagnostics')}; logs={service.logs[-12:]}; errors={service.errors[-12:]}") from error
                    text = body(reply["raw"], 2, 5)[5:].rstrip(b"\0")
                    self.assertIn(b"D12600: SNR 6.00/6.00", text)
                    self.assertIn(b"RSSI -80.0", text)
                    self.assertIn(b"samples 1/256", text)

                    service.stop_process()
                    service.start_process(True)
                    self.assertEqual(admin.download(client), source)
                    self.assertEqual(admin.download_packet(client, 0), module)
                    self.assertIn("saved=1 live=1", self.until(lambda: (v if "live=1" in (v := client.command("packet 0 status")) else None)))
                    self.assertEqual(client.command("bot key"), key)
                    self.assertEqual(client.command("bot policy"), grants)
                    self.assertEqual(client.command("source wasm hash"), original_wasm)
                    self.assertEqual({p.name: p.read_bytes() for p in service.root.glob("*.seed")}, keys)
                    self.until(lambda: not reports())
                    # A damaged packet journal seals only its slot. Explicit removal must
                    # permit immediate reinstall without a separate budget/reset command.
                    service.stop_process()
                    snapshot = service.root / "native/nvs/nvs.snapshot"
                    contents = bytearray(snapshot.read_bytes())
                    record = native_records(contents)[("mc-mast-admin", "packet0")]
                    self.assertEqual(contents.count(record), 1)
                    contents[contents.index(record) + len(record) - 1] ^= 1
                    contents[-4:] = struct.pack("<I", zlib.crc32(contents[:-4]))
                    snapshot.write_bytes(contents)
                    service.start_process(True)
                    self.assertIn("sealed=1", client.command("packet 0 status"))
                    cli("packet", "remove", "0")
                    cli("packet", "install", "0", ROOT / "build/repeater-snr.wasm")
                    self.assertEqual(admin.download_packet(client, 0), module)
                    self.assertEqual(client.command("packet 0 budget").split()[0], "stages=1")
                    self.assertEqual(admin.download(client), source)
                    print("WILLOW_PACKET_WORKFLOW_OK", binary, "shared reports/commands, measured RF only, retained source/config/identities")


if __name__ == "__main__":
    unittest.main()
