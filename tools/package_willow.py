#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Package the four Willow services and a compiled-in verified relocatable worker."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

from release_candidate import required_symbol_versions

ROOT = Path(__file__).resolve().parents[1]
WILLOW = ROOT / "hosts/willow"
BINARIES = (
    "hew-host-release",
    "hew-base-release",
    "hew-dashboard-release",
    "hew-broker-release",
    "native-worker",
)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def package(
    output,
    baseline_image=None,
    runtime=None,
    runtime_source=None,
    prepared_source=None,
    curl_source=None,
):
    # A release archive records a published source tree, not unstaged edits.
    if subprocess.check_output(
        ["git", "status", "--porcelain", "--untracked-files=no"], cwd=ROOT
    ):
        raise ValueError(
            "Commit the selected source changes before packaging a release"
        )
    products = json.loads((ROOT / "release/products.json").read_text())
    version = products["products"]["willow"]["version"]
    commit, epoch = subprocess.check_output(
        ["git", "show", "-s", "--format=%H%n%ct", "HEAD"], cwd=ROOT, text=True
    ).splitlines()
    source = ROOT
    if prepared_source:
        source = prepared_source.resolve()
        metadata = json.loads((source / "release/source.json").read_text())
        if metadata["commit"] != commit:
            raise ValueError("Prepared binary source differs from the selected commit")
        subprocess.run(
            ["python3", "-B", "tools/build_worker.py", "--verify"],
            cwd=source / "hosts/willow",
            check=True,
        )
    elif baseline_image:
        if runtime is None:
            raise ValueError("A matched Debian 12 runtime archive is required")
        from build_willow_linux import prepare

        source = prepare(
            ROOT,
            output.resolve() / "build-source",
            baseline_image,
            runtime,
            curl_source=curl_source,
        )
    else:
        run = lambda args, **kwargs: subprocess.run(
            args, cwd=WILLOW, check=True, **kwargs
        )
        run(["make", "-j2", "native-worker"], timeout=900)
        run(["python3", "-B", "tools/build_worker.py", "--portable"])
        run(
            ["make", "-j2", "release"],
            env=os.environ | {"WILLOW_BINARY_BUILD": "1"},
            timeout=900,
        )
    willow = source / "hosts/willow"
    stem = f"willow-v{version}-linux-x86_64"
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    tree = output / stem
    tree.mkdir()
    base = source

    def copy(path, name=None):
        dest = tree / (name or str(path.relative_to(base)))
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, dest)

    for name in BINARIES + ("native-worker.json", "native-worker.inputs"):
        copy(willow / "build" / name)
    if (willow / "build/lib").exists():
        for path in (willow / "build/lib").iterdir():
            if path.is_file():
                copy(path)
    for directory in ("tools", "docs"):
        for path in (willow / directory).rglob("*"):
            if (
                path.is_file()
                and "__pycache__" not in path.parts
                and path.suffix != ".pyc"
            ):
                copy(path)
    for name in ("README.md",):
        copy(willow / name)
    for name in (
        "LICENSE",
        "NOTICE",
        "THIRD_PARTY.md",
        "release/products.json",
        "internal/app/admin_page.html",
    ):
        copy(source / name)
    for path in (source / "LICENSES").rglob("*"):
        if path.is_file():
            copy(path)
    copy(source / "tools/install_willow_bundle.py", "install-willow.py")
    copy(source / "tools/install_willow.py", "tools/install_willow.py")
    copy(source / "tools/install_willow_bundle.py", "tools/install_willow_bundle.py")
    copy(source / "tools/INSTALL_WILLOW.md")
    (tree / "release/source.json").write_text(
        json.dumps({"commit": commit, "commit_epoch": epoch}, indent=2) + "\n"
    )
    with (tree / "source.tar.gz").open("wb") as stream:
        subprocess.run(
            ["git", "archive", "--format=tar.gz", commit],
            cwd=ROOT,
            stdout=stream,
            check=True,
        )
    if runtime_source is None:
        raise ValueError("Select the matching Hew source directory for runtime notices")
    for name in (
        "LICENSE-MIT",
        "LICENSE-APACHE",
        "NOTICE",
        "THIRD-PARTY-LICENSES",
        "Cargo.lock",
    ):
        copy(runtime_source / name, "licenses/hew/" + name)
    # Carry the actual dependency source, notices and native objects alongside
    # the tracked source, as in the existing host release bundles.
    with tarfile.open(tree / "native-relink.tar.gz", "w:gz") as archive:
        native = willow / "build/native"
        archive.add(
            native,
            arcname="hosts/willow/build/native",
            filter=lambda m: None if ".git" in Path(m.name).parts else m,
        )
        for path in (willow / "build").glob("*.o"):
            archive.add(path, arcname="hosts/willow/build/" + path.name)
        for name in ("libhew.a", "relink-commands.json", "relink-headers"):
            path = willow / "build" / name
            if path.exists():
                archive.add(path, arcname="hosts/willow/build/" + name)
    with tarfile.open(tree / "dependency-notices.tar.gz", "w:gz") as archive:
        archive.add(tree / "licenses/hew", arcname="hew")
        if (willow / "build/dependency-notices").exists():
            archive.add(willow / "build/dependency-notices", arcname="system")
        for name, path in (
            ("Crypto", willow / "build/native/deps/Crypto"),
            ("CayenneLPP", willow / "build/native/deps/CayenneLPP"),
            ("base64", willow / "build/native/usb-deps/base64"),
        ):
            archive.add(path, arcname=name)
    if (willow / "build/baseline.json").is_file():
        copy(willow / "build/baseline.json", "build.json")
    if (willow / "build/curl").exists():
        for name in ("COPYING", "docs/LICENSES"):
            path = willow / "build/curl/source" / name
            if path.is_file():
                copy(path, "licenses/curl/" + Path(name).name)
        pin = json.loads((source / "release/willow-runtime.json").read_text())["curl"]
        copy(willow / "build/curl" / Path(pin["url"]).name, "curl-source.tar.xz")
    # Store actual ELF requirements, without asserting an untested distribution.
    requirements = {}
    for name in (*BINARIES, "lib/libcurl.so.4"):
        path = tree / "hosts/willow/build" / name
        if not path.exists():
            continue
        dynamic = subprocess.check_output(["readelf", "-d", str(path)], text=True)
        requirements[name] = {
            "needed_sonames": sorted(
                re.findall(r"\(NEEDED\).*Shared library: \[([^\]]+)\]", dynamic)
            ),
            "required_symbol_versions": required_symbol_versions(
                subprocess.check_output(
                    ["readelf", "--version-info", str(path)], text=True
                )
            ),
        }
    if baseline_image or (willow / "build/baseline.json").exists():
        limits = {
            "GLIBC": "2.36",
            "GLIBCXX": "3.4.30",
            "CXXABI": "1.3.13",
            "OPENSSL": "3.0.0",
        }
        for name, needed in requirements.items():
            for family, abi_version in needed["required_symbol_versions"].items():
                if family not in limits or tuple(
                    map(int, abi_version.split("."))
                ) > tuple(map(int, limits[family].split("."))):
                    raise ValueError(
                        f"{name} exceeds Debian 12's {family} version: {abi_version}"
                    )
    manifest = {
        "schema_version": 1,
        "product": "willow",
        "version": version,
        "format": "linux-binary-bundle",
        "source": {"commit": commit, "commit_epoch": int(epoch)},
        "platform": "Linux x86-64",
        "compatibility": products["compatibility"],
        "runtime": {
            "python": "3.11 or newer with cryptography and PyNaCl",
            "binaries": requirements,
        },
        "layout": {
            "service_directory": "hosts/willow",
            "installer": "install-willow.py",
        },
        "files": [
            {
                "name": str(p.relative_to(tree)),
                "bytes": p.stat().st_size,
                "sha256": digest(p),
            }
            for p in sorted(tree.rglob("*"))
            if p.is_file()
        ],
    }
    (tree / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (tree / "SHA256SUMS").write_text(
        "".join(
            f"{digest(p)}  {p.relative_to(tree)}\n"
            for p in sorted(tree.rglob("*"))
            if p.is_file() and p.name != "SHA256SUMS"
        )
    )
    archive = output / (stem + ".tar.gz")
    with tarfile.open(archive, "w:gz") as tar:
        tar.add(tree, arcname=stem)
    print(archive)
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--prepared-source",
        type=Path,
        help="Already built source tree for this exact commit",
    )
    parser.add_argument(
        "--baseline-image",
        help="Local Debian 12 image with the documented native dependencies",
    )
    parser.add_argument(
        "--curl-source",
        type=Path,
        help="Cached curl archive matching release/willow-runtime.json",
    )
    parser.add_argument(
        "--runtime", type=Path, help="Matching optimized Debian 12 libhew.a"
    )
    parser.add_argument(
        "--runtime-source",
        type=Path,
        required=True,
        help="Matching Hew source tree for dependency notices",
    )
    args = parser.parse_args()
    try:
        package(
            args.output,
            args.baseline_image,
            args.runtime,
            args.runtime_source,
            args.prepared_source,
            args.curl_source,
        )
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Willow packaging failed: {error}\n")


if __name__ == "__main__":
    main()
