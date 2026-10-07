#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Install a downloaded Birch host bundle into a new directory, without starting services."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil


BINARIES = {"meshcore-host", "meshcore-check", "meshcore-rf-check", "bot-native-worker"}
MATERIAL = {"source.tar.gz", "native-relink.tar.gz", "build-profile.ini", "LICENSE", "NOTICE",
            "THIRD_PARTY.md", "dependency-notices.tar.gz", "install-birch.py",
            "meshcore-host.json.example", "HOST_GUIDE.md"}


def install(bundle, prefix):
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        raise ValueError("Birch host installation requires Linux x86_64; Debian 12 is the supported baseline")
    bundle, prefix = bundle.resolve(), prefix.resolve()
    manifest = json.loads((bundle / "manifest.json").read_text())
    if (manifest.get("schema_version") != 1 or manifest.get("product") != "birch" or
            manifest.get("build", {}).get("scope") != "host_only" or
            manifest.get("build", {}).get("profile") != "public_birch_host"):
        raise ValueError("Select the Birch host-only release bundle")
    names = set()
    for item in manifest["files"]:
        name = item["name"]
        if not isinstance(name, str) or Path(name).name != name or name in names:
            raise ValueError("Birch manifest has an unsafe or duplicate filename")
        names.add(name)
        path = bundle / name
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"Birch bundle file is missing or a symlink: {name}")
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if path.stat().st_size != item["bytes"] or digest != item["sha256"]:
            raise ValueError(f"Birch bundle hash/size mismatch: {name}")
    if names != BINARIES | MATERIAL:
        raise ValueError("Birch host-only bundle is incomplete or includes unexpected files")
    config = json.loads((bundle / "meshcore-host.json.example").read_text())
    config.update(phy_authority="modem", phy_tracking="follow", require_parity=True,
                  bot_runtime="native_lua", bot_native_worker=str(prefix / "bin/bot-native-worker"),
                  state_dir=str(prefix / "state"))
    prefix.mkdir(mode=0o700, parents=True, exist_ok=False)
    binaries = prefix / "bin"
    binaries.mkdir()
    for name in sorted(BINARIES):
        shutil.copy2(bundle / name, binaries / name)
        (binaries / name).chmod(0o755)
    settings = prefix / "config"
    settings.mkdir(mode=0o700)
    path = settings / "meshcore-host.json"
    with path.open("x") as stream:
        path.chmod(0o600)
        json.dump(config, stream, indent=2)
        stream.write("\n")
    (prefix / "state").mkdir(mode=0o700)
    for name in ("manifest.json", "LICENSE", "NOTICE", "THIRD_PARTY.md", "HOST_GUIDE.md"):
        shutil.copy2(bundle / name, prefix / name)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--prefix", type=Path, required=True,
                        help="New versioned installation directory; existing directories are never overwritten")
    args = parser.parse_args()
    try:
        path = install(args.bundle, args.prefix)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Birch installation refused: {error}\n")
    print(f"Installed Birch host tools. Edit {path}: set radio_address, enabled_roles and room password environment.")
    print(f"Check without opening a radio: {args.prefix.resolve()}/bin/meshcore-host -config {path} -check")
    print("No services were started. Existing host state, identities and configuration were not changed.")


if __name__ == "__main__":
    main()
