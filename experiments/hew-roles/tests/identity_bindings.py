"""Compare staged native public/signing/ECDH operations with Go's identity loader."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from migrate_go import authority, identity_public, private, write_private

ROOT = Path(__file__).resolve().parents[1]


def check(source, staged):
    scratch = ROOT/"build"/f"identity-bindings-{os.getpid()}"
    scratch.mkdir(mode=0o700)
    try:
        files = {}
        for role in ("repeater", "room", "observer"):
            for name in ("identity.seed", "identity.expanded", "identity-state.json", "state.json"):
                path = source/role/name
                if path.exists():
                    private(path)
                    files[role+"/"+name] = path.read_bytes()
                    write_private(scratch/role/name, files[role+"/"+name])
        expected = json.loads(subprocess.check_output([ROOT/"build/oracle"], env=os.environ |
            {"MESHCORE_WILLOW_IDENTITY_BINDINGS": str(scratch)}))
        peer = identity_public(bytes([42])*32)
        results = {}
        for role, target in (("repeater", "relay"), ("room", "room"), ("observer", "observer")):
            material, public, _ = authority(files, role)
            path = staged/(target+(".expanded" if len(material) == 64 else ".seed"))
            private(path)
            assert path.read_bytes() == material, role+": staged private material differs from active Go identity"
            raw = subprocess.check_output([ROOT/"build/identity-bindings"],
                                          input=bytes([len(material)])+material+peer)
            reference = expected[role]
            assert raw == bytes.fromhex(reference["public"]+reference["signature"]+reference["cipher"]), role+": native/Go private-key algebra mismatch"
            assert public.hex() == reference["public"]
            results[role] = {"public_key": public.hex(), "public_signature_ecdh_match": True}
        return results
    finally:
        shutil.rmtree(scratch)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--staged", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(check(args.source, args.staged), indent=2))
