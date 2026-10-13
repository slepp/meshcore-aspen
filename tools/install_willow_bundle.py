#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Install a Willow binary bundle at a new prefix without changing service state."""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify(bundle):
    bundle = Path(bundle).resolve()
    manifest = json.loads((bundle / "manifest.json").read_text())
    if (
        manifest.get("schema_version") != 1
        or manifest.get("product") != "willow"
        or manifest.get("format") != "linux-binary-bundle"
    ):
        raise ValueError("Select an extracted Willow Linux binary bundle")
    names = set()
    for item in manifest["files"]:
        name = item["name"]
        relative = Path(name)
        if (
            not name
            or relative.is_absolute()
            or ".." in relative.parts
            or name in names
        ):
            raise ValueError("Willow bundle contains an unsafe or duplicate filename")
        names.add(name)
        path = bundle / relative
        if any(p.is_symlink() for p in (path, *path.parents)) or not path.is_file():
            raise ValueError("Willow bundle file missing or symlink: " + name)
        if path.stat().st_size != item["bytes"] or digest(path) != item["sha256"]:
            raise ValueError("Willow bundle hash or size mismatch: " + name)
    actual = set()
    for path in bundle.rglob("*"):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise ValueError(
                "Willow bundle contains a symlink or special file: "
                + str(path.relative_to(bundle))
            )
        if path.is_file():
            actual.add(str(path.relative_to(bundle)))
    if actual != names | {"manifest.json", "SHA256SUMS"}:
        raise ValueError("Willow bundle has missing or unexpected files")
    binaries = {
        "hosts/willow/build/" + n
        for n in (
            "hew-host-release",
            "hew-base-release",
            "hew-dashboard-release",
            "hew-broker-release",
            "native-worker",
        )
    }
    if not binaries <= names:
        raise ValueError("Willow bundle lacks a production service or native worker")
    return manifest


def install(bundle, prefix):
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        raise ValueError("Willow binary installation requires Linux x86-64")
    bundle, prefix = Path(bundle).resolve(), Path(prefix).absolute()
    verify(bundle)
    if prefix.exists() or prefix.is_symlink():
        raise ValueError(
            "Installation path already exists; select a new versioned prefix"
        )
    prefix.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(bundle, prefix)
    prefix.chmod(0o755)
    return prefix / "hosts/willow"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument(
        "--prefix",
        type=Path,
        required=True,
        help="New versioned installation directory",
    )
    args = parser.parse_args()
    try:
        directory = install(args.bundle, args.prefix)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Willow installation refused: {error}\n")
    print(
        f"Installed Willow at {directory}; no services started or private state changed."
    )
    print(
        f"Follow {directory}/docs/SERVICE.md to initialize a new node with its modem address and radio profile; use docs/MIGRATION.md to retain a stopped node."
    )
    print(
        f"Check private state: python3 -B {directory}/tools/willow.py check --state /path/to/private/willow"
    )


if __name__ == "__main__":
    main()
