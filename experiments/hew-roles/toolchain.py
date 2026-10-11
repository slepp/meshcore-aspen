#!/usr/bin/env python3
"""Track the selected Hew installation so normal Make builds follow upgrades."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil


def inputs(compiler):
    executable = Path(shutil.which(compiler) or compiler).resolve(strict=True)
    directory = executable.parent
    prefix = directory.parent
    files = {executable}
    # Observe installed and Cargo layouts; Hew itself selects the libraries.
    roots = {prefix, prefix / "share/hew", directory.parent.parent}
    if os.environ.get("HEW_STD"):
        files.update(path.resolve() for path in Path(os.environ["HEW_STD"]).rglob("*.hew"))
    for root in roots:
        std = root / "std"
        if (std / "builtins.hew").is_file():
            files.update(path.resolve() for path in std.rglob("*.hew"))
    library_dirs = {directory, prefix / "lib", prefix / "lib/hew", prefix / "lib64/hew",
                    prefix / "debug", prefix / "release-lib", prefix / "release"}
    for root in (prefix / "lib", prefix / "lib/hew", prefix / "lib64/hew", prefix):
        if root.is_dir():
            library_dirs.update(path for path in root.iterdir() if path.is_dir() and
                                ("-" in path.name or path.name in ("debug", "release", "release-lib")))
    for root in tuple(library_dirs):
        if "-" in root.name:
            library_dirs.update(root / name for name in ("debug", "release-lib", "release"))
    for root in library_dirs:
        files.update(path.resolve() for path in root.glob("libhew*.a") if path.is_file())
    return executable, sorted(files)


def update(compiler, output):
    try:
        previous = json.loads(output.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        previous = {}
    executable, paths = inputs(compiler)
    recorded = {}
    for path in paths:
        info = path.stat()
        stamp = [info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns]
        old = previous.get("inputs", {}).get(str(path), {})
        digest = old.get("sha256") if old.get("stamp") == stamp else None
        if digest is None:
            with path.open("rb") as source:
                digest = hashlib.file_digest(source, "sha256").hexdigest()
        recorded[str(path)] = {"stamp": stamp, "sha256": digest}
    document = {"compiler": str(executable), "inputs": recorded,
                "environment": {key: os.environ.get(key, "") for key in
                                ("HEW_STD", "HEW_CC", "HEW_SANITIZE_ADDRESS")}}
    if document != previous:
        output.parent.mkdir(parents=True, exist_ok=True)
        pending = output.with_suffix(".pending")
        pending.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")
        pending.replace(output)
        return True
    return False


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="hew")
    parser.add_argument("--output", type=Path, default=Path("build/toolchain.json"))
    args = parser.parse_args()
    update(args.compiler, args.output)
