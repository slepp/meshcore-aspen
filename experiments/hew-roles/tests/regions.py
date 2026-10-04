"""Actual Go named-region commands compared with native Hew debug and release."""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from parity import ROOT
sys.path.insert(0, str(ROOT))
import region_state

GO_SOURCE = Path(os.environ.get("MESHCORE_GO_SOURCE", ROOT.parents[1]))


def state(entries=None, **extra):
    value = {"NextRegionID": max((entry["id"] for entry in entries or []), default=0)+1,
        "Preferences": {"version": 1, "regions": entries or [], "default_scope": {"name": "", "key": [0]*16}}}
    value.update(extra)
    return value


class Regions(unittest.TestCase):
    def test_go_command_and_state_differential(self):
        cases = [
            {"State": state(), "Commands": ["region", "region def can ab edm", "region home ab", "region home",
                "region get edm", "region default", "region save", "region list allowed", "region denyf *",
                "region denyf edm", "region list denied", "region default ab", "region default",
                "region home AB", "region remove ab", "region put can edm", "region remove edm",
                "region remove ab", "region home", "region default", "region default <null>"]},
            {"State": state(), "Commands": ["region put east", "region put eastern", "region home eas", "region home",
                "region get eas", "region put eas", "region", "region remove ea", "region get *", "region default *",
                "region default", "region default $missing", "region put #east", "region put bad!name",
                "region def a|* b,c", "region def a,|*", "region def a|", "region list wrong",
                "region put", "region home missing", "region unsupported"]},
            {"State": state(), "Commands": ["region def a b c", "region denyf b", "region load", "* F", " a F",
                "  b F", "          deep F", " c F", "", "region", "region home", "region default"]},
            {"State": state(), "Commands": ["region load", "  orphan F", " a F", " a F", "", "region"]},
            {"State": state([{"id": 1, "parent": 0, "flags": 128, "name": "$private",
                "keys": [list(bytes([byte])*16) for byte in (1, 2, 3, 4)]}]),
             "Commands": ["region get $private", "region default $private", "region denyf $private", "region list denied",
                "region allowf $private", "region load", " $private", "", "region get $private"]},
            {"State": state([{"id": 1, "name": "$empty"}]), "Commands": ["region default $empty", "region get $empty"]},
            {"State": state([{"id": 65535, "name": "last"}], NextRegionID=65536),
             "Commands": ["region put new", "region put last", "region home last", "region remove last", "region put new"]},
            {"State": state([{"id": i+1, "name": f"entry{i}"} for i in range(32)]),
             "Commands": ["region put full", "region default full", "region put entry0 *", "region list allowed",
                "region remove entry31", "region put replacement", "region def entry0 entry1 entry2", "region"]},
        ]
        with tempfile.TemporaryDirectory(dir=ROOT/"build") as name:
            directory = Path(name)
            source, destination, overlay = (directory/file for file in ("input.json", "output.json", "overlay.json"))
            source.write_text(json.dumps(cases))
            overlay.write_text(json.dumps({"Replace": {
                str(GO_SOURCE/"internal/roles/willow_region_oracle_test.go"): str(ROOT/"tests/regions_oracle_test.go.in")}}))
            result = subprocess.run(["go", "test", "-buildvcs=false", "-overlay", str(overlay),
                                     "./internal/roles", "-run", "^TestWillowRegionOracle$", "-count=1"],
                cwd=GO_SOURCE, env=os.environ|{"TMPDIR": str(ROOT/"build"), "MESHCORE_REGION_INPUT": str(source),
                    "MESHCORE_REGION_OUTPUT": str(destination)}, capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            expected = json.loads(destination.read_text())
            count = 0
            for binary in ("region-checks", "region-checks-release"):
                for fixture, steps in zip(cases, expected):
                    initial = region_state.encode(region_state.from_go(fixture["State"]))
                    result = subprocess.run([str(ROOT/"build"/binary), initial.hex(), *fixture["Commands"]],
                        capture_output=True, text=True, check=True, timeout=15)
                    actual = [json.loads(line) for line in result.stdout.splitlines()]
                    self.assertEqual(len(actual), len(steps))
                    for command, hew, go in zip(fixture["Commands"], actual, steps):
                        with self.subTest(binary=binary, command=command):
                            self.assertEqual(hew["reply"], go["Reply"])
                            self.assertEqual(hew["loading"], go["Loading"])
                            native = region_state.decode(bytes.fromhex(hew["state"]))
                            wanted = region_state.from_go(go["State"])
                            self.assertEqual(native, wanted)
                            count += 1
            print(f"REGION_DIFFERENTIAL_OK steps={count} cases={len(cases)} modes=debug,release")

    def test_four_key_and_reserved_flag_go_roundtrip(self):
        initial = state([{"id": 1, "parent": 0, "flags": 128, "name": "$rotated",
            "keys": [list(bytes([byte])*16) for byte in (1, 2, 3, 4)]}],
            HomeRegion=1, DefaultRegion=1, ManagedDefaultRegion=True, DiscoveryModified=1234)
        initial["Preferences"]["wildcard_flags"] = 254
        initial["Preferences"]["default_scope"] = {"name": "$rotated", "key": [1]*16}
        encoded = region_state.encode(region_state.from_go(initial))
        returned = copy.deepcopy(initial)
        region_state.apply(returned, region_state.decode(encoded))
        self.assertEqual(returned, initial)
        for end in range(len(encoded)):
            with self.assertRaises(ValueError):
                region_state.decode(encoded[:end])


if __name__ == "__main__":
    unittest.main(verbosity=2)
