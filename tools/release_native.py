#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Rebuild the Birch host bundle in the selected disposable Debian 12 image."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile

from product_versions import ROOT, load

PROFILE = "debian12-linux-x86_64"
DOCKERFILE = "release/Dockerfile.debian12"


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(args, cwd=ROOT, env=None):
    return subprocess.check_output(args, cwd=cwd, env=env, text=True).strip()


def image_receipt(image, env):
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", image):
        raise ValueError("Select the exact locally built Debian 12 image ID")
    info = json.loads(run(["docker", "image", "inspect", image], env=env))[0]
    labels = info["Config"].get("Labels") or {}
    if (info["Id"] != image or info["Os"] != "linux" or info["Architecture"] != "amd64" or
            labels.get("org.meshcore.release.profile") != PROFILE or
            labels.get("org.meshcore.release.dockerfile-sha256") != sha256(ROOT / DOCKERFILE)):
        raise ValueError("Native image differs from the selected public Dockerfile/profile")
    return {"profile": PROFILE, "image_id": image, "dockerfile": DOCKERFILE,
            "dockerfile_sha256": sha256(ROOT / DOCKERFILE)}


def container_command(image, source, go_root, command, *, network="none", epoch="0"):
    if network not in ("none", "bridge"):
        raise ValueError("Unexpected native build network mode")
    return ["docker", "run", "--rm", "--pull", "never", "--network", network,
            "--read-only", "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
            "--user", f"{os.getuid()}:{os.getgid()}", "--workdir", "/src",
            "--mount", f"type=bind,source={source},target=/src",
            "--mount", f"type=bind,source={go_root},target=/opt/go,readonly",
            "--env", "HOME=/src/.tmp/native-home", "--env", "TMPDIR=/src/.tmp",
            "--env", "GOROOT=/opt/go", "--env", "GOTOOLCHAIN=local",
            "--env", "GOPATH=/src/.tmp/gopath", "--env", "GOCACHE=/src/.tmp/go-cache",
            "--env", "PATH=/opt/go/bin:/usr/local/bin:/usr/bin:/bin",
            "--env", "PYTHONDONTWRITEBYTECODE=1", "--env", f"SOURCE_DATE_EPOCH={epoch}",
            "--entrypoint", command[0], image, *command[1:]]


def build_native(image, stem, env, command):
    receipt = image_receipt(image, env)
    source = ROOT / ".tmp" / ("onchip-release-debian12-" + stem)
    source.mkdir()  # Refuse stale source, compiled objects or candidate reuse.
    archive = source / "public-source.tar"
    command(["git", "archive", "--format=tar", "--output=" + str(archive), "HEAD"])
    with tarfile.open(archive) as package:
        package.extractall(source, filter="data")
    archive.unlink()
    scratch = source / ".tmp"
    scratch.mkdir()
    (scratch / "native-home").mkdir()
    # Only the pinned public upstream and selected source dependencies are staged.
    upstream = ROOT / ".tmp/onchip-upstream"
    if run(["git", "rev-parse", "HEAD"], cwd=upstream) != load()["upstream"]["commit"]:
        raise ValueError("Native source upstream differs from the release pin")
    for name in ("onchip-upstream", "MeshCore", "parity-upstream"):
        command(["git", "clone", "--no-hardlinks", str(upstream), str(scratch / name)])
    libs = Path(".tmp/MeshCore/.pio/libdeps/Xiao_S3_WIO_kiss_wifi")
    for dependency in (libs / "Crypto", libs / "CayenneLPP", Path(".tmp/native-test-deps/base64")):
        shutil.copytree(ROOT / dependency, source / dependency)
    lua = Path(".tmp/onchip-lua/lua-5.5.1.tar.gz")
    (source / lua).parent.mkdir(parents=True)
    shutil.copy2(ROOT / lua, source / lua)
    wamr = source / ".cache/meshcore-wamr"
    wamr.mkdir(parents=True)
    shutil.copy2(ROOT / ".cache/meshcore-wamr/wamr.tar.gz", wamr / "wamr.tar.gz")
    # Extract using host Python 3.13's data filter. Debian's Python 3.11 then
    # patches/configures this fresh pinned source, never a newer-glibc object.
    from importlib.util import spec_from_file_location, module_from_spec
    spec = spec_from_file_location("release_wamr_pin", ROOT / "firmware/esp32/prepare_wasm.py")
    pins = module_from_spec(spec)
    spec.loader.exec_module(pins)
    if sha256(wamr / "wamr.tar.gz") != pins.SHA256:
        raise ValueError("Native WAMR source archive differs from the pin")
    with tarfile.open(wamr / "wamr.tar.gz") as package:
        package.extractall(wamr, filter="data")
    (wamr / ("wasm-micro-runtime-" + pins.REVISION)).rename(wamr / "source")
    go_root = Path(run(["go", "env", "GOROOT"], env=env))
    receipt.update(go=run(["go", "version"], env=env), go_sha256=sha256(go_root / "bin/go"),
                   source_commit=run(["git", "rev-parse", "HEAD"]), source_date_epoch=int(env["SOURCE_DATE_EPOCH"]))

    def inside(args, network="none"):
        command(container_command(image, source, go_root, args, network=network, epoch=env["SOURCE_DATE_EPOCH"]))

    # Fetch public, go.sum-verified sources and the complete module metadata
    # into a new empty cache. Unused graph nodes need .info for the inventory.
    # Compilation and worker qualification can subsequently run without network.
    inside(["go", "mod", "download"], network="bridge")
    inside(["go", "list", "-m", "-json", "all"], network="bridge")
    for name in ("go.mod", "go.sum"):
        if (source / name).read_bytes() != (ROOT / name).read_bytes():
            raise ValueError("Native dependency fetch changed the selected public " + name)
    inside(["make", "host-build"])
    inside(["make", "-C", "firmware/esp32", "bot-native-worker",
            "BOT_BUILD=/src/.tmp/onchip-native-worker", "BUILD=/src/.tmp/onchip-native-source",
            "CONFIG=/src/firmware/esp32/platformio.public.ini.example", "BOT_JSON_INCLUDE=/usr/include/cjson"])
    inside(["python3", "tools/release_native.py", "inspect", "--output", ".tmp/native-receipt.json"])
    receipt.update(json.loads((scratch / "native-receipt.json").read_text()))
    (scratch / "native-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    return source, receipt


def inspect_build(output):
    from release_candidate import HOST_FILES, json_stream, native_abi, native_libraries
    worker = ROOT / ".tmp/onchip-native-worker/bot-native-worker"
    env = dict(os.environ)
    binaries = {name: worker if name == "bot-native-worker" else ROOT / "bin" / name for name in HOST_FILES}
    distro = platform.freedesktop_os_release()
    if distro.get("ID") != "debian" or distro.get("VERSION_ID") != "12":
        raise ValueError("Native build is not running on the intended Debian 12 baseline")
    data = {"distribution": distro, "python": platform.python_version(), "build_os": platform.platform(),
            "toolchain": {"go": run(["go", "version"]), "cxx": run(["c++", "--version"]).splitlines()[0],
                          "native_cxx_sha256": sha256(Path(shutil.which("c++"))),
                          "readelf": run(["readelf", "--version"]).splitlines()[0],
                          "readelf_sha256": sha256(Path(shutil.which("readelf"))),
                          "cmake": run(["cmake", "--version"]).splitlines()[0],
                          "cmake_sha256": sha256(Path(shutil.which("cmake")))},
            "packages": run(["dpkg-query", "-W", "-f=${binary:Package}\t${Version}\n"]),
            "native_linked_libraries": native_libraries(worker, env),
            "native_abi": {name: native_abi(path, env) for name, path in sorted(binaries.items())},
            "go_modules": json_stream(run(["go", "list", "-m", "-json", "all"]))}
    # Headers and dependency notices used for native relinking accompany objects.
    material = ROOT / ".tmp/native-toolchain"
    shutil.copytree("/usr/include/cjson", material / "cjson")
    for package in ("libc6", "libstdc++6", "libgcc-s1", "libcjson1", "libcjson-dev", "libssl3", "libssl-dev"):
        copyright = Path("/usr/share/doc") / package / "copyright"
        if copyright.is_file():
            dest = material / "notices" / package
            dest.mkdir(parents=True)
            shutil.copy2(copyright, dest / "copyright")
    for module in data["go_modules"]:
        if module.get("Version") and module.get("Dir"):
            for path in Path(module["Dir"]).rglob("*"):
                if path.is_file() and path.name.upper().startswith(("LICENSE", "COPYING", "NOTICE")):
                    dest = material / "notices/go" / module["Path"] / path.relative_to(module["Dir"])
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(path, dest)
    shutil.copy2(Path(os.environ["GOROOT"]) / "LICENSE", material / "notices/GO-TOOLCHAIN-LICENSE")
    output.write_text(json.dumps(data, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("inspect",))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    inspect_build(args.output)
