#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify the source dependencies used by fresh release build trees."""
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import shutil
import stat
import tarfile
import urllib.request

from product_versions import load

LIBS = ".tmp/MeshCore/.pio/libdeps/Xiao_S3_WIO_kiss_wifi"
PINS = {
    "Crypto": {
        "version": "0.4.0",
        "url": "https://dl.registry.platformio.org/download/rweather/library/Crypto/0.4.0/Crypto-0.4.0.tar.gz",
        "sha256": "1867740aad0d61bdcbac25f6dbc8eefe6eed9e7b37f48d9d0b9d80500ad431e0",
        "path": LIBS + "/Crypto",
    },
    "CayenneLPP": {
        "version": "1.6.1",
        "url": "https://dl.registry.platformio.org/download/electroniccats/library/CayenneLPP/1.6.1/CayenneLPP-1.6.1.tar.gz",
        "sha256": "e3717ac5f6c17ee617cc129506989973acdd40ada8724c7cd1fad8c21e784423",
        "path": LIBS + "/CayenneLPP",
    },
    "base64": {
        "version": "1.4.0",
        "url": "https://dl.registry.platformio.org/download/densaugeo/library/base64/1.4.0/base64-1.4.0.tar.gz",
        "sha256": "cfb062549d19647e0300c8f01b0b542a7482fcdb85d432552900d3909bf75e5c",
        "path": ".tmp/native-test-deps/base64",
    },
}
ARCHIVES = {
    "lua": {
        "path": ".tmp/onchip-lua/lua-5.5.1.tar.gz",
        "sha256": "1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce",
    },
    "wamr": {
        "path": ".cache/meshcore-wamr/wamr.tar.gz",
        "sha256": "c197d6d811c23b3df0446e91abffba1091caf282e026156d19ace8ba2fc83f3e",
    },
}
CACHE = ".cache/meshcore-release-inputs"


def file_record(name, data):
    return {"path": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def checked_archive(path, pin):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"Release input archive is missing or not a regular file: {path}")
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != pin["sha256"]:
        raise ValueError(f"Release input archive SHA256 mismatch: {path}; inspect or restore the pinned archive")
    return data


def archive_inventory(data):
    rows = []
    names = set()
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        for member in archive:
            path = PurePosixPath(member.name)
            if (path.is_absolute() or ".." in path.parts or str(path) != member.name.rstrip("/") or
                    member.name in names or not (member.isfile() or member.isdir())):
                raise ValueError(f"Unsafe or duplicate release input archive member: {member.name}")
            names.add(member.name)
            if member.isfile():
                rows.append(file_record(member.name, archive.extractfile(member).read()))
    if not rows:
        raise ValueError("Release input archive contains no files")
    return sorted(rows, key=lambda row: row["path"])


def tree_inventory(root, *, allow_piopm=False):
    if root.is_symlink() or not root.is_dir():
        raise ValueError(f"Release input source directory is missing or not a directory: {root}")
    rows = []
    for path in sorted(root.rglob("*")):
        mode = path.lstat().st_mode
        relative = path.relative_to(root).as_posix()
        if stat.S_ISDIR(mode):
            continue
        if not stat.S_ISREG(mode):
            raise ValueError(f"Release input source is not a regular file: {path}")
        if allow_piopm and relative == ".piopm":
            continue
        rows.append(file_record(relative, path.read_bytes()))
    return rows


def assert_inventory(actual, expected, label):
    if len({row["path"] for row in actual}) != len(actual) or len({row["path"] for row in expected}) != len(expected):
        raise ValueError(f"Duplicate release source inventory entry: {label}")
    actual_by_name = {row["path"]: row for row in actual}
    expected_by_name = {row["path"]: row for row in expected}
    for name in sorted(actual_by_name.keys() | expected_by_name.keys()):
        if actual_by_name.get(name) != expected_by_name.get(name):
            raise ValueError(f"Release source input differs from its pinned archive: {label}/{name}; "
                             "inspect or restore this dependency before rebuilding")


def package_archive(root, name):
    pin = PINS[name]
    path = root / CACHE / Path(pin["url"]).name
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        partial = path.with_suffix(path.suffix + ".partial")
        with urllib.request.urlopen(pin["url"], timeout=60) as response, partial.open("xb") as output:
            shutil.copyfileobj(response, output)
        checked_archive(partial, pin)
        partial.rename(path)
    return path, checked_archive(path, pin)


def check_piopm(data, name, pin):
    try:
        metadata = json.loads(data)
    except (ValueError, UnicodeDecodeError) as error:
        raise ValueError(f"Invalid PlatformIO source metadata: {name}/.piopm") from error
    if (not isinstance(metadata, dict) or metadata.get("type") != "library" or
            metadata.get("name") != name or metadata.get("version") != pin["version"] or
            not isinstance(metadata.get("spec"), dict) or
            metadata["spec"].get("name") != name or
            metadata["spec"].get("owner") != pin["url"].split("/")[4]):
        raise ValueError(f"PlatformIO source metadata differs from the pin: {name}/.piopm")


def package_inventory(root, name, pin):
    rows = tree_inventory(root, allow_piopm=True)
    metadata = root / ".piopm"
    if metadata.exists():
        check_piopm(metadata.read_bytes(), name, pin)
    return rows


def stage_source(root, destination, command, run):
    """Use only tracked source, checked archives and the selected upstream pin."""
    destination.mkdir()
    archive = destination / "public-source.tar"
    command(["git", "archive", "--format=tar", "--output=" + str(archive), "HEAD"])
    with tarfile.open(archive) as package:
        package.extractall(destination, filter="data")
    archive.unlink()
    upstream = root / ".tmp/onchip-upstream"
    if run(["git", "rev-parse", "HEAD"], cwd=upstream) != load()["upstream"]["commit"]:
        raise ValueError("Release source upstream differs from the pin")
    (destination / ".tmp").mkdir()
    for name in ("onchip-upstream", "MeshCore", "parity-upstream"):
        command(["git", "clone", "--no-hardlinks", str(upstream), str(destination / ".tmp" / name)])
    packages = {}
    for name, pin in PINS.items():
        archive_path, data = package_archive(root, name)
        expected = archive_inventory(data)
        cached = root / pin["path"]
        if cached.exists() or cached.is_symlink():
            assert_inventory(package_inventory(cached, name, pin), expected, pin["path"])
        target = destination / pin["path"]
        target.mkdir(parents=True)
        with tarfile.open(fileobj=io.BytesIO(data)) as package:
            package.extractall(target, filter="data")
        assert_inventory(tree_inventory(target), expected, pin["path"])
        bundled = destination / CACHE / archive_path.name
        bundled.parent.mkdir(parents=True, exist_ok=True)
        bundled.write_bytes(data)
        packages[name] = {**pin, "archive_path": f"{CACHE}/{archive_path.name}", "files": expected}
    archives = {}
    for name, pin in ARCHIVES.items():
        data = checked_archive(root / pin["path"], pin)
        target = destination / pin["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        archives[name] = {**pin, "bytes": len(data)}
    return {"packages": packages, "archives": archives}


def prepared_inventory(source, lua):
    return {
        lua: tree_inventory(source / lua),
        ".cache/meshcore-wamr/source/core": tree_inventory(source / ".cache/meshcore-wamr/source/core"),
        ".cache/meshcore-wamr/source/build-scripts": tree_inventory(source / ".cache/meshcore-wamr/source/build-scripts"),
    }


def input_layout(profile=None, *, bot=True):
    packages = {}
    for name, pin in PINS.items():
        if profile and name == "base64" and not bot:
            continue
        filename = Path(pin["url"]).name
        packages[name] = {
            **pin,
            "path": f".pio/libdeps/{profile}/{name}" if profile else pin["path"],
            "archive_path": f".release-inputs/{filename}" if profile else f"{CACHE}/{filename}",
        }
    archives = {
        name: {**pin, "path": f".release-inputs/{Path(pin['path']).name}" if profile else pin["path"]}
        for name, pin in ARCHIVES.items() if not profile or bot
    }
    prepared = ({"lib/OnchipLua", "lib/OnchipWamr"} if bot else set()) if profile else {
        ".tmp/onchip-native-worker/lib/OnchipLua", ".cache/meshcore-wamr/source/core",
        ".cache/meshcore-wamr/source/build-scripts"}
    return packages, archives, prepared


def firmware_inputs(source, work, profile, *, bot):
    packages, archives, prepared = input_layout(profile, bot=bot)
    material = work / ".release-inputs"
    material.mkdir()
    for name, pin in packages.items():
        archive_path = source / CACHE / Path(pin["url"]).name
        data = checked_archive(archive_path, pin)
        expected = archive_inventory(data)
        actual = package_inventory(work / pin["path"], name, pin)
        assert_inventory(actual, expected, pin["path"])
        (work / pin["archive_path"]).write_bytes(data)
        packages[name] = {**pin, "files": actual}
    for name, pin in archives.items():
        data = checked_archive(source / ARCHIVES[name]["path"], pin)
        (work / pin["path"]).write_bytes(data)
        archives[name] = {**pin, "bytes": len(data)}
    return {"packages": packages, "archives": archives,
            "prepared": {path: tree_inventory(work / path) for path in sorted(prepared)}}


def verify_relink(path, receipt, *, profile=None, bot=True):
    """Check packaged dependencies against pinned archives, without extraction."""
    if not isinstance(receipt, dict) or set(receipt) != {"packages", "archives", "prepared"}:
        raise ValueError("Release is missing its verified source input receipt")
    packages, archives, expected_prepared = input_layout(profile, bot=bot)
    if set(receipt["packages"]) != set(packages) or set(receipt["archives"]) != set(archives):
        raise ValueError("Release source input receipt has missing or extra dependencies")
    with tarfile.open(path) as archive:
        members = {}
        for member in archive:
            if member.name in members:
                raise ValueError(f"Duplicate release relink archive member: {member.name}")
            members[member.name] = member

        def contents(name):
            member = members.get(name)
            if not member or not member.isfile():
                raise ValueError(f"Release relink input is missing or not a regular file: {name}")
            return archive.extractfile(member).read()

        for name, pin in packages.items():
            item = receipt["packages"][name]
            if item != {**pin, "files": item.get("files")}:
                raise ValueError(f"Release source package pin mismatch: {name}")
            data = contents(pin["archive_path"])
            if hashlib.sha256(data).hexdigest() != pin["sha256"]:
                raise ValueError(f"Release source archive SHA256 mismatch: {name}")
            expected = archive_inventory(data)
            assert_inventory(item["files"], expected, name)
            prefix = pin["path"] + "/"
            actual = []
            for member_name, member in members.items():
                if member_name.startswith(prefix) and not member.isdir():
                    if member_name == prefix + ".piopm":
                        check_piopm(contents(member_name), name, pin)
                        continue
                    actual.append(file_record(member_name[len(prefix):], contents(member_name)))
            assert_inventory(actual, expected, name)
        for name, pin in archives.items():
            data = contents(pin["path"])
            if (receipt["archives"][name] != {**pin, "bytes": len(data)} or
                    hashlib.sha256(data).hexdigest() != pin["sha256"]):
                raise ValueError(f"Release source archive pin mismatch: {name}")
        if set(receipt["prepared"]) != expected_prepared:
            raise ValueError("Release prepared source receipt has missing or extra trees")
        for prefix, expected in receipt["prepared"].items():
            actual = [file_record(name[len(prefix) + 1:], contents(name))
                      for name, member in members.items() if name.startswith(prefix + "/") and not member.isdir()]
            if not expected:
                raise ValueError(f"Release prepared source receipt is empty: {prefix}")
            assert_inventory(actual, expected, prefix)
