#!/usr/bin/env python3
"""Back up or restore the standard per-user installation during maintenance."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


SERVICE = "meshcore-host.service"
TARGETS = {
    "config": ".config/meshcore-host",
    "state": ".local/state/meshcore-host",
    "host": ".local/bin/meshcore-host",
    "check": ".local/bin/meshcore-check",
    "unit": ".config/systemd/user/meshcore-host.service",
}


def service(*arguments):
    subprocess.run(["systemctl", "--user", *arguments, SERVICE], check=True)


def copy(source, destination):
    if source.is_dir():
        shutil.copytree(source, destination)
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def backup(directory, home):
    for name in ("config", "state", "host", "unit"):
        if not (home / TARGETS[name]).exists():
            raise RuntimeError(f"Existing {name} installation not found; nothing was stopped.")
    directory.mkdir(mode=0o700, parents=True, exist_ok=False)
    service("stop")
    saved = []
    for name, relative in TARGETS.items():
        source = home / relative
        if source.exists():
            copy(source, directory / name)
            saved.append(name)
    (directory / "manifest.json").write_text(json.dumps({"version": 1, "saved": saved}) + "\n")
    print("Host backup complete. Service is stopped for maintenance.")


def restore(directory, home):
    manifest = json.loads((directory / "manifest.json").read_text())
    saved = manifest.get("saved")
    if (
        manifest.get("version") != 1
        or not isinstance(saved, list)
        or any(not isinstance(name, str) or name not in TARGETS for name in saved)
        or not {"config", "state", "host", "unit"}.issubset(saved)
        or any(not (directory / name).exists() for name in saved)
    ):
        raise RuntimeError("Invalid or incomplete host backup; nothing was stopped.")
    service("stop")
    previous = Path(tempfile.mkdtemp(prefix="replaced-", dir=directory))
    for name in saved:
        destination = home / TARGETS[name]
        if destination.exists() or destination.is_symlink():
            # Retain the entire newer state: overlaying old files could leave a
            # newer identity envelope authoritative over the restored key.
            shutil.move(str(destination), previous / name)
        copy(directory / name, destination)
    subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
    print("Host restored; replaced files retained privately beside the backup.")
    print("Restore matching modem firmware if necessary, then start meshcore-host.service.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("backup", "restore"))
    parser.add_argument("--directory", required=True)
    args = parser.parse_args()
    os.umask(0o077)
    home = Path.home().resolve()
    directory = Path(args.directory).resolve()
    for relative in TARGETS.values():
        target = (home / relative).resolve()
        if directory == target or directory in target.parents or target in directory.parents:
            parser.error("Backup directory must not overlap the installed files or state.")
    try:
        if args.action == "backup":
            backup(directory, home)
        else:
            restore(directory, home)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"meshcore-host {args.action}: {error}\nService may remain stopped; no automatic restart was attempted.\n")


if __name__ == "__main__":
    main()
