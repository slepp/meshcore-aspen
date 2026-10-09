#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Package Linux terminal clients from a clean, signed public source revision."""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CLIENT = ROOT / "clients/room-tui"


def run(args, cwd=ROOT, env=None):
    return subprocess.check_output(args, cwd=cwd, env=env, text=True).strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def json_stream(text):
    decoder = json.JSONDecoder()
    while text.strip():
        value, offset = decoder.raw_decode(text.lstrip())
        yield value
        text = text.lstrip()[offset:]


def archive(directory, destination, epoch):
    with destination.open("xb") as output:
        with gzip.GzipFile(filename="", mode="wb", fileobj=output, mtime=epoch) as compressed:
            with tarfile.open(fileobj=compressed, mode="w") as package:
                for path in sorted(directory.rglob("*")):
                    if not path.is_file():
                        continue
                    entry = package.gettarinfo(str(path), arcname=str(path.relative_to(directory.parent)))
                    entry.uid = entry.gid = 0
                    entry.uname = entry.gname = ""
                    entry.mtime = epoch
                    entry.mode = 0o755 if path.name == "room-tui" else 0o644
                    with path.open("rb") as contents:
                        package.addfile(entry, contents)


def verify_elf(binary, machine):
    data = binary.read_bytes()
    if data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", data, 18)[0] != machine:
        raise ValueError(f"{binary.name}: unexpected ELF architecture")
    table = struct.unpack_from("<Q", data, 32)[0]
    entry_size, count = struct.unpack_from("<HH", data, 54)
    if any(struct.unpack_from("<I", data, table + index * entry_size)[0] == 3
           for index in range(count)):
        raise ValueError(f"{binary.name}: unexpected dynamic ELF interpreter")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True, help="New output directory")
    parser.add_argument("--public-ref", default="refs/heads/main")
    args = parser.parse_args()
    version = (CLIENT / "VERSION").read_text().strip()
    if not re.fullmatch(
            r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version):
        raise ValueError("VERSION must be a stable Semantic Version")
    if run(["git", "status", "--porcelain", "--untracked-files=all"]):
        raise ValueError("Package from a clean source tree")
    revision = run(["git", "rev-parse", "HEAD"])
    run(["git", "verify-commit", revision])
    public = run(["git", "ls-remote", "https://github.com/slepp/meshcore-aspen.git", args.public_ref])
    if len(public.splitlines()) != 1 or public.split()[0] != revision:
        raise ValueError("Public ref must resolve to this exact source revision")
    epoch = int(run(["git", "show", "-s", "--format=%ct", revision]))
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    environment = {key: os.environ[key] for key in ("PATH", "HOME", "GOCACHE", "GOMODCACHE", "GOPROXY")
                   if key in os.environ}
    environment.update({"CGO_ENABLED": "0", "GOTOOLCHAIN": "local", "GOOS": "linux", "GOFLAGS": ""})
    with tempfile.TemporaryDirectory(prefix="room-tui-release-", dir=ROOT / ".tmp") as temporary:
        scratch = Path(temporary)
        raw_source = subprocess.check_output(["git", "archive", "--format=tar", revision], cwd=ROOT)
        source = scratch / "source"
        source.mkdir()
        with tarfile.open(fileobj=io.BytesIO(raw_source)) as package:
            package.extractall(source, filter="data")
        build_client = source / "clients/room-tui"
        modules = list(json_stream(run(["go", "list", "-mod=readonly", "-m", "-json", "all"],
                                      cwd=build_client, env=environment)))
        downloaded = list(json_stream(run(["go", "mod", "download", "-json"],
                                         cwd=build_client, env=environment)))
        run(["go", "mod", "verify"], cwd=build_client, env=environment)
        directories = {module["Path"]: Path(module["Dir"]) for module in downloaded if "Dir" in module}
        notices = scratch / "DEPENDENCY_LICENSES"
        notices.mkdir()
        inventory = []
        for module in modules:
            if module.get("Main"):
                continue
            directory = directories.get(module["Path"])
            if not directory:
                raise ValueError(f"Dependency directory missing: {module['Path']}")
            licenses = sorted(path for path in directory.iterdir() if path.is_file() and
                              path.name.upper().startswith(("LICENSE", "COPYING", "NOTICE")))
            if not licenses:
                raise ValueError(f"Dependency license missing: {module['Path']}")
            target = notices / (module["Path"].replace("/", "__") + "@" + module["Version"])
            target.mkdir()
            for license_file in licenses:
                shutil.copy2(license_file, target / license_file.name)
            inventory.append({"module": module["Path"], "version": module["Version"],
                              "sum": module.get("Sum"), "licenses": [path.name for path in licenses]})
        go_root = Path(run(["go", "env", "GOROOT"], env=environment))
        shutil.copy2(go_root / "LICENSE", notices / "GO_LICENSE")
        go_version = run(["go", "version"], env=environment)
        for architecture, machine in (("amd64", 62), ("arm64", 183)):
            name = f"room-tui-v{version}-linux-{architecture}"
            bundle = scratch / name
            bundle.mkdir()
            binary = bundle / "room-tui"
            target_environment = environment | {"GOARCH": architecture}
            run(["go", "build", "-mod=readonly", "-trimpath", "-buildvcs=false",
                 "-ldflags", f"-s -w -buildid= -X main.releaseVersion={version} -X main.sourceRevision={revision}",
                 "-o", str(binary), "."], cwd=build_client, env=target_environment)
            verify_elf(binary, machine)
            metadata = run(["go", "version", "-m", str(binary)], env=environment)
            if f"GOARCH={architecture}" not in metadata or "CGO_ENABLED=0" not in metadata:
                raise ValueError("Go build metadata differs from the selected target")
            shutil.copy2(source / "LICENSE", bundle / "LICENSE")
            shutil.copy2(build_client / "README.md", bundle / "README.md")
            shutil.copytree(notices, bundle / "DEPENDENCY_LICENSES")
            with (bundle / "source.tar.gz").open("xb") as raw:
                with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=epoch) as compressed:
                    compressed.write(raw_source)
            manifest = {"product": "room-tui", "version": version, "platform": f"linux/{architecture}",
                        "source": {"repository": "https://github.com/slepp/meshcore-aspen",
                                   "commit": revision, "public_ref": args.public_ref},
                        "build": {"go": go_version, "cgo": False, "trimpath": True},
                        "binary": {"name": "room-tui", "sha256": digest(binary),
                                   "bytes": binary.stat().st_size},
                        "dependencies": inventory}
            (bundle / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            shutil.copy2(bundle / "manifest.json", output / (name + ".manifest.json"))
            archive(bundle, output / (name + ".tar.gz"), epoch)
        assets = sorted(output.iterdir())
        (output / "SHA256SUMS").write_text("".join(f"{digest(path)}  {path.name}\n" for path in assets))
    print(output)


if __name__ == "__main__":
    main()
