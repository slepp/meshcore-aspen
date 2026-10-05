#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build and verify unpublished product candidates from an exact public commit.

This tool does not tag, publish, flash, or provision a device.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import tarfile
import zipfile

from product_versions import ROOT, display, generated, identity, load, tag
from elf_abi import inspect_elf

PROFILES = {
    "aspen": ("public_aspen", "firmware/esp32/platformio.public.ini.example"),
    "birch": ("public_birch", "release/platformio.birch.ini"),
}
IMAGES = {"firmware.bin", "bootloader.bin", "partitions.bin", "boot_app0.bin"}
HOST_FILES = {"meshcore-host", "meshcore-check", "meshcore-rf-check", "bot-native-worker"}


def stage_birch_files(work):
    dest = work / "examples/kiss_modem"
    shutil.copy2(ROOT / "firmware/esp32/wifi_kiss_main.cpp", dest / "main.cpp")
    for name in ("WifiKissMultiplexer.h", "WifiKissMultiplexer.cpp", "QueuedTxProtocol.h",
                 "RadioDashboard.h", "RadioDashboard.cpp", "RadioDashboardPage.h",
                 "RadioNetwork.h", "RadioFirmwareIdentity.h", "EspSntpClock.h", "SntpConfig.h"):
        shutil.copy2(ROOT / "firmware/shared" / name, dest / name)
    shutil.copy2(ROOT / "firmware/esp32/FirmwareIdentity.h", dest / "FirmwareIdentity.h")


def required_symbol_versions(text):
    # Definitions/exported symbols are not runtime requirements.
    needs = text.partition("Version needs section")[2]
    result = {}
    for family, version in re.findall(r"Name: (GLIBCXX|GLIBC|CXXABI|OPENSSL)_([0-9]+(?:\.[0-9]+)+)\b", needs):
        if family not in result or tuple(map(int, version.split("."))) > tuple(map(int, result[family].split("."))):
            result[family] = version
    return result


def native_abi(binary, env):
    program = run(["readelf", "--program-headers", "--wide", str(binary)], env=env)
    interpreter = re.search(r"Requesting program interpreter: ([^\]]+)\]", program)
    dynamic = run(["readelf", "--dynamic", "--wide", str(binary)], env=env)
    receipt = {
        "architecture": "x86_64",
        "interpreter": interpreter.group(1) if interpreter else None,
        "needed_sonames": sorted(re.findall(r"\(NEEDED\).*Shared library: \[([^\]]+)\]", dynamic)),
        "required_symbol_versions": required_symbol_versions(run(["readelf", "--version-info", str(binary)], env=env)),
        "distribution_qualified": False,
    }
    if receipt != inspect_elf(binary):
        raise ValueError("ELF inspection disagrees with GNU readelf")
    return receipt


def package_owner(text, path):
    for line in text.splitlines():
        match = re.fullmatch(r"([a-z0-9][a-z0-9+.-]*(?::[a-z0-9-]+)?): (.+)", line)
        if match and match.group(2) == str(path):
            return match.group(1)
    return None


def json_stream(text):
    decoder = json.JSONDecoder()
    values = []
    while text.strip():
        value, end = decoder.raw_decode(text.lstrip())
        values.append(value)
        text = text.lstrip()[end:]
    return values


def run(args, cwd=ROOT, env=None):
    return subprocess.check_output(args, cwd=cwd, env=env, text=True).strip()


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def record(path, role):
    return {"name": path.name, "role": role, "bytes": path.stat().st_size, "sha256": digest(path)}


def artifact_role(name):
    return ("application" if name == "firmware.bin" else "initial_install" if name in IMAGES
            else "host" if name in HOST_FILES else "build_material")


def partitions(path):
    content = path.read_bytes()
    rows = []
    for offset in range(0, len(content), 32):
        row = content[offset:offset + 32]
        if len(row) != 32 or row[:2] != b"\xaa\x50":
            break
        _, kind, subtype, address, size, label, _ = struct.unpack("<HBBII16sI", row)
        rows.append({"name": label.rstrip(b"\0").decode("ascii"), "type": kind, "subtype": subtype,
                     "offset": address, "bytes": size})
    if not rows or any(item["offset"] + item["bytes"] > 0x800000 for item in rows):
        raise ValueError("Expected an 8 MiB ESP32 partition layout")
    names = set()
    end = 0x9000  # Reserve bootloader and the partition-table sector.
    for item in sorted(rows, key=lambda row: row["offset"]):
        if (not item["bytes"] or item["bytes"] % 0x1000 or item["offset"] % 0x1000 or
                (item["type"] == 0 and item["offset"] % 0x10000)):
            raise ValueError("Partition size/address alignment invalid")
        if item["name"] in names or not re.fullmatch(r"[A-Za-z0-9_-]+", item["name"]):
            raise ValueError("Partition name invalid or duplicated")
        if item["offset"] < end:
            raise ValueError("Partition ranges overlap reserved storage or another partition")
        names.add(item["name"])
        end = item["offset"] + item["bytes"]
    return rows


def native_libraries(binary, env):
    libraries = []
    for line in run(["ldd", str(binary)], env=env).splitlines():
        match = re.search(r"(\S+)\s+=>\s+(/\S+)", line)
        if match:
            name, path = match.groups()
        else:
            loader = re.match(r"\s*(/\S+)\s+\(", line)
            if not loader:
                continue
            path = loader.group(1)
            name = Path(path).name
        item = {"soname": name, "sha256": digest(Path(path)), "bytes": Path(path).stat().st_size}
        if shutil.which("dpkg-query", path=env.get("PATH")):
            for probe in (Path(path), Path(path).resolve()):
                found = subprocess.run(["dpkg-query", "-S", str(probe)], env=env, text=True,
                                       capture_output=True, check=False)
                package = package_owner(found.stdout, probe) if found.returncode == 0 else None
                if package:
                    item["package"] = package
                    item["package_version"] = run(["dpkg-query", "-W", "-f=${Version}", package], env=env)
                    break
        libraries.append(item)
    if not {"libcjson.so.1", "libssl.so.3", "libcrypto.so.3"} <= {item["soname"] for item in libraries}:
        raise ValueError("Native worker linked-library receipt is missing cJSON or OpenSSL 3")
    return libraries


def public_source(ref):
    if not ref.startswith(("refs/heads/", "refs/tags/")):
        raise ValueError("Select an exact public refs/heads/... or refs/tags/... ref")
    if run(["git", "status", "--porcelain", "--untracked-files=all"]):
        raise ValueError("Candidate builds require a clean source checkout")
    sha = run(["git", "rev-parse", "HEAD"])
    # Use the approved repository, never a private remote or replacement URL.
    url = load()["repository"] + ".git"
    rows = run(["git", "ls-remote", url, ref, ref + "^{}"]).splitlines()
    refs = dict(line.split()[::-1] for line in rows)
    if refs.get(ref + "^{}", refs.get(ref)) != sha:
        raise ValueError("Selected public ref does not resolve to the clean source HEAD")
    return {"repository": load()["repository"], "commit": sha, "ref": ref, "dirty": False}


def archive_tree(source, destination):
    # This is only a tool-created build tree with a tracked public config.
    with tarfile.open(destination, "w:gz") as archive:
        for path in sorted(source.rglob("*")):
            if ".git" in path.relative_to(source).parts or path.is_symlink():
                continue
            if path.is_file():
                archive.add(path, arcname=path.relative_to(source), recursive=False)


def build(product, ref, output, native_image=None):
    if (product == "birch") != bool(native_image):
        raise ValueError("Birch requires --native-image with the exact Debian 12 image ID; Aspen does not use it")
    data = load()
    for path, content in generated(data).items():
        if (ROOT / path).read_text() != content:
            raise ValueError(f"Stale generated version: {path}")
    source = public_source(ref)
    version = data["products"][product]["version"]
    profile, config = PROFILES[product]
    stem = f"{tag(product, version)}-xiao-esp32s3-sx1262-{source['commit'][:12]}"
    if product == "birch":
        if platform.system() != "Linux" or platform.machine() != "x86_64":
            raise ValueError("The initial Birch candidate bundle requires Linux x86_64")
        stem += "-linux-x86_64"
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    directory = output / stem
    directory.mkdir()  # Never overwrite a candidate, even from the same SHA.
    work = ROOT / ".tmp" / ("onchip-release-" + stem)
    work.mkdir(parents=True)
    env = {key: os.environ[key] for key in ("HOME", "USER", "PATH") if key in os.environ}
    env.update(PYTHONDONTWRITEBYTECODE="1", TMPDIR=str(ROOT / ".tmp"),
               SOURCE_DATE_EPOCH=run(["git", "show", "-s", "--format=%ct", "HEAD"]))
    log = directory / "build.log"

    def command(args):
        with log.open("a") as stream:
            stream.write("$ " + " ".join(args) + "\n")
            stream.flush()
            subprocess.run(args, cwd=ROOT, env=env, check=True, stdout=stream, stderr=subprocess.STDOUT)

    upstream = ROOT / ".tmp/onchip-upstream"
    command(["make", "onchip-upstream"])
    if run(["git", "rev-parse", "HEAD"], cwd=upstream) != data["upstream"]["commit"]:
        raise ValueError("Upstream checkout differs from the release pin")
    toolchain = {"platformio": run(["pio", "--version"], env=env),
                 "python": platform.python_version(), "build_os": platform.platform()}
    linked_libraries = []
    native_receipt = {}
    native_source = None
    if product == "aspen":
        command(["make", "-C", "firmware/esp32", "bot-firmware", f"BUILD={work}",
                 f"CONFIG={ROOT / config}", f"ENV={profile}"])
    else:
        # Stage a fresh modem without reading private firmware/platformio.local.ini.
        command(["git", "clone", "--no-hardlinks", str(upstream), str(work / "upstream")])
        command(["git", "-C", str(work / "upstream"), "archive", "--output=" + str(work / "upstream.tar"), "HEAD"])
        with tarfile.open(work / "upstream.tar") as archive:
            archive.extractall(work, filter="data")
        shutil.rmtree(work / "upstream")
        (work / "upstream.tar").unlink()
        command(["patch", "--directory", str(work), "-p1", "--input", str(ROOT / "firmware/shared/radio-reconfigure.patch")])
        stage_birch_files(work)
        shutil.copy2(ROOT / config, work / "platformio.local.ini")
        command(["pio", "run", "--project-dir", str(work), "-e", profile])
        from release_native import build_native
        native_source, native_receipt = build_native(native_image, stem, env, command)
        native = native_source / ".tmp/onchip-native-worker"
        for name in HOST_FILES:
            path = native / name if name == "bot-native-worker" else native_source / "bin" / name
            shutil.copy2(path, directory / name)
        toolchain.update(native_receipt["toolchain"])
        linked_libraries = native_receipt["native_linked_libraries"]
        # Include the older-baseline objects/sources/headers actually used.
        with tarfile.open(directory / "native-relink.tar.gz", "w:gz") as archive:
            for relative in (".tmp/onchip-native-worker", ".tmp/onchip-native-source", ".tmp/onchip-phy",
                             ".tmp/onchip-lua", ".tmp/MeshCore/.pio/libdeps", ".tmp/native-test-deps",
                             ".cache/meshcore-wamr", ".tmp/native-toolchain", ".tmp/native-receipt.json"):
                archive.add(native_source / relative, arcname=relative)
    pio_build = work / ".pio/build" / profile
    for name in IMAGES - {"boot_app0.bin"}:
        shutil.copy2(pio_build / name, directory / name)
    framework = Path(env["HOME"]) / ".platformio/packages/framework-arduinoespressif32"
    shutil.copy2(framework / "tools/partitions/boot_app0.bin", directory / "boot_app0.bin")
    if identity(product, version).encode() not in (directory / "firmware.bin").read_bytes():
        raise ValueError("Built image does not contain the selected product identity")
    metadata = json.loads(run(["pio", "project", "metadata", "--project-dir", str(work),
                               "-e", profile, "--json-output"], env=env))
    inventory = {"resolved_packages": run(["pio", "pkg", "list", "--project-dir", str(work),
                                           "-e", profile], env=env), "build_metadata": metadata[profile]}
    compiler = Path(metadata[profile]["cxx_path"])
    toolchain["firmware_cxx"] = run([str(compiler), "--version"], env=env).splitlines()[0]
    toolchain["firmware_cxx_sha256"] = digest(compiler)
    # Include actual resolved libraries, objects, ELF/map and build source/config.
    archive_tree(work, directory / "firmware-relink.tar.gz")
    command(["git", "archive", "--format=tar.gz", "--output=" + str(directory / "source.tar.gz"), "HEAD"])
    shutil.copy2(ROOT / config, directory / "build-profile.ini")
    for name in ("LICENSE", "NOTICE", "THIRD_PARTY.md"):
        shutil.copy2(ROOT / name, directory / name)
    with tarfile.open(directory / "dependency-notices.tar.gz", "w:gz") as archive:
        archive.add(ROOT / "LICENSES", arcname="LICENSES")
        if product == "birch":
            archive.add(native_source / ".tmp/native-toolchain/notices", arcname="native-and-go")
    versioned = tag(product, version)
    manifest = {
        "schema_version": 1, "product": product, "version": version, "tag": versioned,
        "display": display(product, version, data["upstream"]["version"]),
        "identity": identity(product, version), "source": source, "upstream": data["upstream"],
        "build": {"profile": profile, "config_path": config, "config_sha256": digest(ROOT / config),
                  "source_date_epoch": int(env["SOURCE_DATE_EPOCH"]), "toolchain": toolchain,
                  "dependencies": inventory,
                  "go_modules": native_receipt.get("go_modules", []),
                  "go_sum_sha256": digest(ROOT / "go.sum"),
                  "native_linked_libraries": linked_libraries,
                  "native_abi": native_receipt.get("native_abi", {}),
                  "native_environment": native_receipt,
                  "lua_archive_sha256": "1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce",
                  "wamr_commit": "b124f70345d712bead5c0c2393acb2dc583511de"},
        "compatibility": data["compatibility"],
        "layout": {"partitions": partitions(directory / "partitions.bin"),
                   "initial_install_offsets": {"bootloader.bin": 0, "partitions.bin": 0x8000,
                                               "boot_app0.bin": 0xe000, "firmware.bin": 0x10000},
                   "application_update": "Only the selected health-confirmed application slot; preserve NVS/SPIFFS/OTA selection"},
        "hardware": "Seeed XIAO ESP32-S3R8 + Wio SX1262; 8 MiB flash/PSRAM",
        "qualification": {"state": "unqualified_candidate", "hardware_tested": False,
                          "blockers": (["Blank-board WiFi provisioning for the matching Go host modem is not qualified"]
                                       if product == "birch" else []) +
                                      ["Review exact build/test receipts and perform separately authorized hardware acceptance before publishing"]},
        "files": [record(path, artifact_role(path.name))
                  for path in sorted(directory.iterdir()) if path.name != "build.log"],
    }
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    verify(directory)
    bundle = output / (stem + ".zip")
    with zipfile.ZipFile(bundle, "x", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(directory.iterdir()):
            if path.name != "build.log":
                archive.write(path, arcname=stem + "/" + path.name)
    sums = output / (stem + ".SHA256SUMS")
    sums.write_text(f"{digest(bundle)}  {bundle.name}\n{digest(directory / 'manifest.json')}  {stem}/manifest.json\n")
    print(directory / "manifest.json", flush=True)


def verify(directory):
    manifest = json.loads((directory / "manifest.json").read_text())
    product, version = manifest["product"], manifest["version"]
    if manifest["schema_version"] != 1 or product not in PROFILES:
        raise ValueError("Unknown candidate schema or product")
    if manifest["tag"] != tag(product, version) or manifest["identity"] != identity(product, version):
        raise ValueError("Product/tag/identity mismatch")
    data = load()
    if version != data["products"][product]["version"] or manifest["upstream"] != data["upstream"]:
        raise ValueError("Candidate differs from the selected release authority")
    if manifest["compatibility"] != data["compatibility"]:
        raise ValueError("Candidate protocol compatibility mismatch")
    source = manifest["source"]
    if source["repository"] != data["repository"] or source["dirty"] or not re.fullmatch(r"[0-9a-f]{40}", source["commit"]):
        raise ValueError("Candidate source is not an exact clean public commit")
    profile, config = PROFILES[product]
    if manifest["build"]["profile"] != profile or manifest["build"]["config_path"] != config:
        raise ValueError("Candidate build profile mismatch")
    if manifest["build"]["config_sha256"] != digest(directory / "build-profile.ini"):
        raise ValueError("Candidate build config hash mismatch")
    build = manifest["build"]
    toolchain = build["toolchain"]
    for key in ("platformio", "python", "build_os", "firmware_cxx"):
        if not isinstance(toolchain.get(key), str) or not toolchain[key]:
            raise ValueError(f"Candidate is missing toolchain receipt: {key}")
    for value in (toolchain.get("firmware_cxx_sha256", ""), build.get("go_sum_sha256", ""),
                  build.get("lua_archive_sha256", "")):
        if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
            raise ValueError("Candidate has an invalid compiler/dependency hash")
    if not re.fullmatch(r"[0-9a-f]{40}", build.get("wamr_commit", "")):
        raise ValueError("Candidate is missing the exact WAMR commit")
    if type(build.get("source_date_epoch")) is not int or build["source_date_epoch"] <= 0:
        raise ValueError("Candidate is missing the build epoch")
    if not build["dependencies"].get("resolved_packages") or not build["dependencies"].get("build_metadata"):
        raise ValueError("Candidate is missing resolved PlatformIO dependencies")
    if product == "birch":
        if not toolchain.get("go") or not toolchain.get("cxx") or not build.get("go_modules"):
            raise ValueError("Birch is missing host/native build receipts")
        if not re.fullmatch(r"[0-9a-f]{64}", toolchain.get("native_cxx_sha256", "")):
            raise ValueError("Birch is missing the native compiler hash")
        libraries = build.get("native_linked_libraries", [])
        if not {"libcjson.so.1", "libssl.so.3", "libcrypto.so.3"} <= {item["soname"] for item in libraries}:
            raise ValueError("Birch is missing the linked cJSON/OpenSSL receipt")
        for item in libraries:
            if not re.fullmatch(r"[0-9a-f]{64}", item["sha256"]) or type(item["bytes"]) is not int or item["bytes"] <= 0:
                raise ValueError("Birch has an invalid native-library hash/size")
        if not toolchain.get("readelf") or not re.fullmatch(r"[0-9a-f]{64}", toolchain.get("readelf_sha256", "")):
            raise ValueError("Birch is missing the ELF inspection tool receipt")
        abi = build.get("native_abi", {})
        if set(abi) != HOST_FILES:
            raise ValueError("Birch is missing host ABI receipts")
        for item in abi.values():
            if (item.get("architecture") != "x86_64" or item.get("distribution_qualified") is not False or
                    (item.get("interpreter") is not None and item["interpreter"] != "/lib64/ld-linux-x86-64.so.2") or
                    not isinstance(item.get("needed_sonames"), list) or
                    any(not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_.+-]+", name) for name in item["needed_sonames"]) or
                    not isinstance(item.get("required_symbol_versions"), dict) or
                    any(family not in {"GLIBC", "GLIBCXX", "CXXABI", "OPENSSL"} or
                        not isinstance(version, str) or not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", version)
                        for family, version in item["required_symbol_versions"].items())):
                raise ValueError("Birch has an invalid or falsely qualified host ABI receipt")
        if not {"libcjson.so.1", "libssl.so.3", "libcrypto.so.3"} <= set(abi["bot-native-worker"]["needed_sonames"]):
            raise ValueError("Birch worker ABI omits cJSON/OpenSSL dependencies")
        if not {"GLIBC", "GLIBCXX", "CXXABI", "OPENSSL"} <= set(abi["bot-native-worker"]["required_symbol_versions"]):
            raise ValueError("Birch worker ABI omits required symbol versions")
        if not set(abi["bot-native-worker"]["needed_sonames"]) <= {item["soname"] for item in libraries}:
            raise ValueError("Birch worker ABI and linked-library receipts differ")
        # Older candidates retain their unqualified, host-native receipts. New
        # Debian 12 builds must identify the exact image and actual baseline ABI.
        native = build.get("native_environment")
        if native:
            from release_native import DOCKERFILE, PROFILE
            if (native.get("profile") != PROFILE or
                    not re.fullmatch(r"sha256:[0-9a-f]{64}", native.get("image_id", "")) or
                    native.get("dockerfile") != DOCKERFILE or
                    not re.fullmatch(r"[0-9a-f]{64}", native.get("dockerfile_sha256", "")) or
                    native.get("source_commit") != source["commit"] or
                    native.get("source_date_epoch") != build["source_date_epoch"] or
                    native.get("distribution", {}).get("ID") != "debian" or
                    native.get("distribution", {}).get("VERSION_ID") != "12" or
                    native.get("native_abi") != abi or native.get("native_linked_libraries") != libraries or
                    native.get("go_modules") != build["go_modules"] or
                    any(toolchain.get(key) != value for key, value in native.get("toolchain", {}).items())):
                raise ValueError("Birch Debian 12 environment receipt mismatch")
            limits = {"GLIBC": "2.36", "GLIBCXX": "3.4.30", "CXXABI": "1.3.13", "OPENSSL": "3.0.0"}
            for item in abi.values():
                for family, required_version in item["required_symbol_versions"].items():
                    if tuple(map(int, required_version.split("."))) > tuple(map(int, limits[family].split("."))):
                        raise ValueError("Birch packaged ELF exceeds its Debian 12 baseline")
    names = set()
    for entry in manifest["files"]:
        name = entry["name"]
        if name in names or Path(name).name != name:
            raise ValueError("Duplicate or unsafe manifest filename")
        names.add(name)
        if entry["role"] != artifact_role(name):
            raise ValueError(f"Candidate artifact role mismatch: {name}")
        path = directory / name
        if path.is_symlink() or not path.is_file() or record(path, entry["role"]) != entry:
            raise ValueError(f"Candidate file hash/size mismatch: {name}")
    required = IMAGES | {"source.tar.gz", "firmware-relink.tar.gz", "build-profile.ini", "LICENSE", "NOTICE",
                        "THIRD_PARTY.md", "dependency-notices.tar.gz"}
    if product == "birch":
        required |= HOST_FILES | {"native-relink.tar.gz"}
    if required != names:
        raise ValueError("Incomplete product bundle; Birch requires the host and matching modem together")
    if product == "birch":
        for name in HOST_FILES:
            if build["native_abi"][name] != inspect_elf(directory / name):
                raise ValueError(f"Birch ABI receipt differs from packaged ELF: {name}")
    extras = {path.name for path in directory.iterdir()} - names - {"manifest.json", "build.log"}
    if extras:
        raise ValueError("Unmanifested files in candidate directory")
    if identity(product, version).encode() not in (directory / "firmware.bin").read_bytes():
        raise ValueError("Candidate firmware identity mismatch")
    if manifest["layout"]["partitions"] != partitions(directory / "partitions.bin"):
        raise ValueError("Candidate partition layout mismatch")
    expected_offsets = {"bootloader.bin": 0, "partitions.bin": 0x8000,
                        "boot_app0.bin": 0xe000, "firmware.bin": 0x10000}
    if manifest["layout"]["initial_install_offsets"] != expected_offsets:
        raise ValueError("Candidate initial-install offsets mismatch")
    apps = [item for item in manifest["layout"]["partitions"] if item["type"] == 0]
    if not apps or not any(item["offset"] == 0x10000 for item in apps) or any(
            item["bytes"] < (directory / "firmware.bin").stat().st_size for item in apps):
        raise ValueError("Candidate image does not fit the application layout")
    if manifest["qualification"]["state"] != "unqualified_candidate" or manifest["qualification"]["hardware_tested"] is not False:
        raise ValueError("This tool records build candidates; qualification requires separate review")
    with tarfile.open(directory / "source.tar.gz") as archive:
        if archive.pax_headers.get("comment") != source["commit"]:
            raise ValueError("Source archive does not identify the candidate commit")
        for path, expected in generated(data).items():
            if archive.extractfile(path).read().decode() != expected:
                raise ValueError(f"Archived product identity differs: {path}")
        if archive.extractfile("release/products.json").read() != (ROOT / "release/products.json").read_bytes():
            raise ValueError("Archived release authority differs from the selected candidate")
        if hashlib.sha256(archive.extractfile(config).read()).hexdigest() != manifest["build"]["config_sha256"]:
            raise ValueError("Build configuration differs from the public source archive")
        if hashlib.sha256(archive.extractfile("go.sum").read()).hexdigest() != build["go_sum_sha256"]:
            raise ValueError("Go dependencies differ from the public source archive")
        if product == "birch" and build.get("native_environment"):
            native = build["native_environment"]
            if hashlib.sha256(archive.extractfile(native["dockerfile"]).read()).hexdigest() != native["dockerfile_sha256"]:
                raise ValueError("Native Dockerfile differs from the public source archive")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    builder = commands.add_parser("build")
    builder.add_argument("--product", choices=PROFILES, required=True)
    builder.add_argument("--public-ref", required=True)
    builder.add_argument("--output", type=Path, default=ROOT / ".tmp/candidates")
    builder.add_argument("--native-image", help="Exact sha256:... ID built from release/Dockerfile.debian12 (Birch)")
    checker = commands.add_parser("verify")
    checker.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "build":
            build(args.product, args.public_ref, args.output, args.native_image)
        else:
            verify(args.directory)
            print("Candidate hashes and metadata verified; hardware qualification remains pending")
    except (ValueError, FileExistsError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Candidate refused: {error}\n")


if __name__ == "__main__":
    main()
