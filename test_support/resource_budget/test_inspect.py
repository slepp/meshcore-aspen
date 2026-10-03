import importlib.util
import json
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location("budget_inspect", Path(__file__).with_name("inspect.py"))
budget = importlib.util.module_from_spec(spec)
spec.loader.exec_module(budget)


class Text:
    def __init__(self, value):
        self.value = value

    def read_text(self, **kwargs):
        return self.value


class Bytes:
    def __init__(self, value):
        self.value = value

    def read_bytes(self):
        return self.value


class InspectTests(unittest.TestCase):
    def test_map_counts_merged_ranges_once(self):
        data = Text("""Discarded input sections
 .text.fake 0x100 0x100 libOnchipLua.a(fake.o)
Linker script and memory map
.flash.text 0x100 0x20
 .text.lua 0x100 0x10 libOnchipLua.a(lvm.o)
 .text.wasm
                0x108 0x10 libOnchipWamr.a(wasm.o)
Cross Reference Table
""")
        result = budget.map_inputs(data, {".flash.text": {"address": 0x100, "bytes": 0x20}})
        self.assertEqual(result["lua"][".flash.text"], 8)
        self.assertEqual(result["wasm"][".flash.text"], 8)
        self.assertEqual(result["merged_shared"][".flash.text"], 8)

    def test_map_clips_ranges_to_output(self):
        data = Text("Linker script and memory map\n.flash.text 0x100 0x10\n"
                    " .text.lua 0xff 0x100 libOnchipLua.a(lvm.o)\n")
        result = budget.map_inputs(data, {".flash.text": {"address": 0x100, "bytes": 0x10}})
        self.assertEqual(result["lua"][".flash.text"], 16)

    def test_partition_records_stop_at_checksum(self):
        entry = struct.pack("<HBBII16sI", 0x50AA, 0, 0x10, 0x10000, 0x330000, b"app0", 0)
        self.assertEqual(budget.partitions(Bytes(entry + b"\xff" * 32))[0]["bytes"], 3342336)

    def test_role_and_sdk_are_separate(self):
        self.assertEqual(budget.category(".pio/build/env/examples/kiss_modem/onchip/Room.cpp.o"), "role_room")
        self.assertEqual(budget.category("/sdk/lib/libmbedtls.a(ssl.o)"), "wifi_tls_web_admin")

    def test_recorded_image_accounting(self):
        report = json.loads(Path(__file__).with_name("measurements.json").read_text())
        for profile in report["profiles"].values():
            measured = profile["measurements"]
            self.assertEqual(measured["allocated_load_bytes"],
                             sum(row["bytes"] for row in measured["sections"].values() if row["load"]))
            self.assertLess(measured["binary_bytes"], 3342336)
            for section, row in measured["sections"].items():
                self.assertEqual(row["bytes"],
                                 measured["map_alignment_and_unattributed_bytes"][section] +
                                 sum(group.get(section, 0) for group in measured["map_input_estimates"].values()))
        profiles = report["profiles"]
        self.assertEqual(profiles["wasm"]["measurements"]["binary_bytes"] -
                         profiles["lua"]["measurements"]["binary_bytes"], 104112)
        self.assertEqual(report["nrf_native"]["memory"]["static_ram_bytes"], 49444)

    def test_full_https_pair_has_matched_inputs(self):
        report = json.loads(Path(__file__).with_name("measurements.json").read_text())
        off, on = (report["profiles"][key] for key in ("https", "https-wasm"))
        self.assertEqual(off["environment"], "Xiao_S3_WIO_onchip_https")
        self.assertEqual(on["environment"], off["environment"])
        self.assertEqual(on["profile_sha256"], off["profile_sha256"])
        self.assertEqual(on["compiler_options"], off["compiler_options"])
        self.assertEqual(on["all_other_effective_defines_sha256"],
                         off["all_other_effective_defines_sha256"])
        off_flags, on_flags = dict(off["effective_defines"]), dict(on["effective_defines"])
        self.assertEqual(off_flags.pop("ONCHIP_BOT_WASM"), "0")
        self.assertEqual(on_flags.pop("ONCHIP_BOT_WASM"), "1")
        self.assertEqual(off_flags, on_flags)
        self.assertEqual(off_flags["MESHCORE_MAST_ADMIN"], "1")
        self.assertEqual(off_flags["ONCHIP_BOT_HTTPS"], "1")
        self.assertEqual(on["measurements"]["partitions"], off["measurements"]["partitions"])
        pair = report["full_https_pair"]
        self.assertEqual(pair["baseline"], report["baseline"])
        for field, delta in pair["exact_deltas"].items():
            self.assertEqual(on["measurements"][field] - off["measurements"][field], delta)

    def test_historical_cedar_maps_are_separate_and_complete(self):
        report = json.loads(Path(__file__).with_name("measurements.json").read_text())
        rows = report["historical_cedar"]
        for row in rows.values():
            self.assertNotEqual(row["source"], report["baseline"])
            self.assertEqual(row["environment"], "Xiao_S3_WIO_onchip_https_probe")
            self.assertFalse(row["raw_artifacts_public_upload_allowed"])
            measured = row["measurements"]
            self.assertEqual(row["artifacts"]["bin"]["bytes"], measured["binary_bytes"])
            self.assertEqual(row["owner_reported_build_log"]["static_ram_bytes"],
                             measured["static_dram_data_bytes"] + measured["static_dram_bss_bytes"])
            for section, value in measured["sections"].items():
                self.assertEqual(value["bytes"],
                                 measured["map_alignment_and_unattributed_bytes"][section] +
                                 sum(group.get(section, 0) for group in measured["map_input_estimates"].values()))
        off, on = rows["cedar-wasm0"]["measurements"], rows["cedar-wasm1"]["measurements"]
        self.assertEqual(on["binary_bytes"] - off["binary_bytes"], 93328)
        self.assertEqual(on["allocated_load_bytes"] - off["allocated_load_bytes"], 93324)

    def test_historical_private_checksums_are_not_published(self):
        report = json.loads(Path(__file__).with_name("measurements.json").read_text())
        def check(value):
            if isinstance(value, dict):
                for key, child in value.items():
                    self.assertNotIn("sha256", key)
                    check(child)
            elif isinstance(value, list):
                for child in value:
                    check(child)
        check(report["historical_cedar"])
        for profile in report["profiles"].values():
            self.assertIn("elf_sha256", profile["measurements"])
            self.assertIn("binary_sha256", profile["measurements"])


if __name__ == "__main__":
    unittest.main()
